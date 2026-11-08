#!/usr/bin/env python3
"""Contract for the Patreon token handling in scripts/update-patrons.py.

The same token pair lives in the maintainer's config and in the Actions
secrets of several repositories, so only --refresh-tokens may spend the
refresh token, and it has to leave every copy holding the new pair.
"""

import importlib.util
import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
from pathlib import Path
from unittest import mock

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("update_patrons", ROOT / "scripts" / "update-patrons.py")
patrons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patrons)

OLD = {"access_token": "old-access", "refresh_token": "old-refresh",
       "client_id": "client-id", "client_secret": "client-secret"}
NEW = {"access_token": "new-access", "refresh_token": "new-refresh"}


def stop_message(call):
    try:
        call()
    except SystemExit as stop:
        return str(stop.code)
    raise AssertionError("expected the run to stop")


def token_endpoint(request, timeout=None):
    assert request.full_url == patrons.TOKEN_URL, request.full_url
    form = urllib.parse.parse_qs(request.data.decode())
    assert form["grant_type"] == ["refresh_token"], form
    assert form["refresh_token"] == [OLD["refresh_token"]], form
    return io.BytesIO(json.dumps(NEW).encode())


def run_main(config_path, argv, gh_run, environ=None, endpoint=token_endpoint):
    environ = dict(environ or {})
    with mock.patch.object(patrons, "CONFIG_PATH", config_path), \
         mock.patch.object(patrons.urllib.request, "urlopen", endpoint), \
         mock.patch.object(patrons.shutil, "which", lambda name: "/usr/bin/gh"), \
         mock.patch.object(patrons.subprocess, "run", gh_run), \
         mock.patch.object(sys, "argv", ["update-patrons.py"] + argv), \
         mock.patch.dict(os.environ):
        os.environ.pop("GITHUB_ACTIONS", None)
        os.environ.update(environ)
        patrons.main()


def config_file(directory):
    path = Path(directory) / "patreon.json"
    path.write_text(json.dumps(OLD), encoding="utf-8")
    os.chmod(path, 0o600)
    return path


def rejected_token_never_refreshes():
    def reject(url, token):
        raise urllib.error.HTTPError(url, 401, "Unauthorized", {}, None)

    with mock.patch.object(patrons, "api_get", reject), \
         mock.patch.object(patrons, "refresh_tokens") as refresh:
        message = stop_message(lambda: patrons.fetch_members(dict(OLD)))
    assert not refresh.called, "a rejected access token must not spend the refresh token"
    assert "--refresh-tokens" in message, message


def refresh_updates_every_copy():
    calls = []

    def gh_run(argv, input=None, text=None, capture_output=None):
        calls.append((argv, input))
        return subprocess.CompletedProcess(argv, 0, "", "")

    with tempfile.TemporaryDirectory() as directory:
        path = config_file(directory)
        run_main(path, ["--refresh-tokens"], gh_run)
        saved = json.loads(path.read_text(encoding="utf-8"))
        mode = stat.S_IMODE(path.stat().st_mode)
    assert saved == dict(OLD, **NEW), saved
    assert mode == 0o600, oct(mode)
    expected = {(repo, secret) for repo in patrons.SECRET_REPOS for secret, _ in patrons.SECRET_KEYS}
    written = {(argv[argv.index("--repo") + 1], argv[3]) for argv, _ in calls}
    assert written == expected and len(calls) == len(expected), calls
    keys = dict(patrons.SECRET_KEYS)
    for argv, value in calls:
        assert value == saved[keys[argv[3]]], (argv, value)
        assert value not in argv, "a secret value must not appear on the command line"


def failed_write_keeps_the_new_pair():
    failing_repo = patrons.SECRET_REPOS[-1]

    def gh_run(argv, input=None, text=None, capture_output=None):
        code = 1 if argv[argv.index("--repo") + 1] == failing_repo else 0
        return subprocess.CompletedProcess(argv, code, "", "HTTP 403" if code else "")

    with tempfile.TemporaryDirectory() as directory:
        path = config_file(directory)
        message = stop_message(lambda: run_main(path, ["--refresh-tokens"], gh_run))
        saved = json.loads(path.read_text(encoding="utf-8"))
    assert saved == dict(OLD, **NEW), "the refreshed pair must be saved before any secret write"
    assert failing_repo in message and "--push-secrets" in message, message


def failed_refresh_saves_nothing():
    def gh_run(*_args, **_kwargs):
        raise AssertionError("a failed refresh must not write any secret")

    def answering(status=200, body=b""):
        def endpoint(request, timeout=None):
            token_endpoint(request, timeout)
            if status != 200:
                raise urllib.error.HTTPError(request.full_url, status, "", {}, None)
            return io.BytesIO(body)
        return endpoint

    # Patreon's answer, and whether the pair may still be good so the run
    # says to retry the refresh before replacing it.
    answers = ((answering(400), False), (answering(401), False),
               (answering(429), True), (answering(503), True),
               (answering(body=b"<html>"), True), (answering(body=b"{}"), True))
    for endpoint, may_retry in answers:
        with tempfile.TemporaryDirectory() as directory:
            path = config_file(directory)
            message = stop_message(lambda: run_main(path, ["--refresh-tokens"], gh_run,
                                                    endpoint=endpoint))
            saved = json.loads(path.read_text(encoding="utf-8"))
        assert saved == OLD, (message, saved)
        assert "--push-secrets" in message, message
        assert ("--refresh-tokens" in message) == may_retry, message


def maintenance_refused_in_a_workflow():
    def gh_run(*_args, **_kwargs):
        raise AssertionError("a workflow must not write secrets")

    with tempfile.TemporaryDirectory() as directory:
        path = config_file(directory)
        for argv in (["--refresh-tokens"], ["--push-secrets"]):
            stop_message(lambda: run_main(path, argv, gh_run, {"GITHUB_ACTIONS": "true"}))
        saved = json.loads(path.read_text(encoding="utf-8"))
    assert saved == OLD, "a refused run must leave the saved pair alone"


CASES = (rejected_token_never_refreshes, refresh_updates_every_copy,
         failed_write_keeps_the_new_pair, failed_refresh_saves_nothing,
         maintenance_refused_in_a_workflow)

if __name__ == "__main__":
    for case in CASES:
        case()
        print(f"PASS {case.__name__}")
    print(f"Patreon token contract: {len(CASES)} cases passed")
