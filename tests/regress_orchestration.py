#!/usr/bin/env python3
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


repo = Path(sys.argv[1]).resolve()


def wait_for(predicate, what="regression runner did not reach the expected state"):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError(what)


# An exited process reads Z until its parent reaps it and X while that happens,
# and then its /proc entry goes; none of those is running.
def running(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0] not in "ZX"
    except (FileNotFoundError, ProcessLookupError, IndexError):
        return False


with tempfile.TemporaryDirectory(prefix="dusk-regress-contract-") as scratch:
    root = Path(scratch)
    runner_tmp = root / "tmp"
    runner_tmp.mkdir()
    locks = root / "locks"
    child_environment = dict(os.environ, DUSK_REGRESS_RECORD_FILE=str(root / "results.tsv"),
                             TMPDIR=str(runner_tmp), DUSK_REGRESS_LOCK_DIR=str(locks))
    sandbox_harness = r'''
set -euo pipefail
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/common.sh"
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/scenarios.sh"
bb_begin interrupted-scratch
env "${SANDBOX_ENV[@]}" python3 -c '
import os, pathlib, tempfile
scratch = pathlib.Path(tempfile.mkdtemp(prefix="duskstudio-scenario-"))
(scratch / "restore.json").write_text("{}")
os._exit(0)
'
bb_end 0
'''
    result = subprocess.run(["bash", "-c", sandbox_harness],
                            env=dict(child_environment, DUSK_REGRESS_TEST_ROOT=str(repo),
                                     DUSK_REGRESS_SCENARIO_KEEP="0"),
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not any(runner_tmp.iterdir()), "interrupted scenario scratch escaped its sandbox"

    # The app binds $XDG_RUNTIME_DIR/dusk-studio/instance-<16 hex>.sock, so a
    # launch gets its single-instance slot only while that fits sun_path.
    long_tmp = runner_tmp / ("a-folder-name-long-enough-to-overrun-a-single-instance-socket-" + "x" * 32)
    long_tmp.mkdir()
    assert len(str(long_tmp)) >= 90, long_tmp
    bind_slot = root / "bind-slot.py"
    bind_slot.write_text(
        "import os, socket, stat\n"
        "runtime = os.environ['XDG_RUNTIME_DIR']\n"
        "print('runtime', runtime, oct(stat.S_IMODE(os.stat(runtime).st_mode)))\n"
        "os.mkdir(os.path.join(runtime, 'dusk-studio'), 0o700)\n"
        "with socket.socket(socket.AF_UNIX) as slot:\n"
        "    slot.bind(os.path.join(runtime, 'dusk-studio', 'instance-0123456789abcdef.sock'))\n"
        "print('bound')\n")
    sandbox_slot_harness = r'''
set -euo pipefail
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/common.sh"
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/scenarios.sh"
bb_begin long-tmpdir-slot
printf 'first %s\n' "$(readlink "$BB_SDIR/runtime")"
sandbox_env "$BB_SDIR"
env "${SANDBOX_ENV[@]}" python3 -I "$DUSK_REGRESS_TEST_BIND"
bb_end 0
'''
    pipewire_stub = Path(tempfile.mkdtemp(prefix="dusk-pw-stub.", dir="/tmp"))
    pipewire_socket = socket.socket(socket.AF_UNIX)
    pipewire_socket.bind(str(pipewire_stub / "pipewire-0"))
    gnome_slot_harness = r'''
set -uo pipefail
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/gnome-session.sh"
gnome_session_setup long-tmpdir-gnome
printf 'first %s\n' "$GNOME_RT"
XDG_RUNTIME_DIR="$GNOME_RT" python3 -I "$DUSK_REGRESS_TEST_BIND" || exit 1
'''
    try:
        # The second case is a session whose own runtime dir is too long to use.
        for runtime_base in (os.environ.get("XDG_RUNTIME_DIR"), str(long_tmp)):
            for name, harness in (("sandbox_env", sandbox_slot_harness), ("gnome-session.sh", gnome_slot_harness)):
                environment = dict(child_environment, TMPDIR=str(long_tmp), DUSK_REGRESS_TEST_ROOT=str(repo),
                                   DUSK_REGRESS_TEST_BIND=str(bind_slot), DUSK_REGRESS_SCENARIO_KEEP="0",
                                   DUSK_REGRESS_PIPEWIRE_DIR=str(pipewire_stub))
                if runtime_base is None:
                    environment.pop("XDG_RUNTIME_DIR", None)
                else:
                    environment["XDG_RUNTIME_DIR"] = runtime_base
                result = subprocess.run(["bash", "-c", harness], env=environment,
                                        capture_output=True, text=True, timeout=20)
                where = (name, runtime_base, result.stdout, result.stderr)
                assert result.returncode == 0, where
                first = result.stdout.split("first ", 1)[1].split("\n", 1)[0]
                runtime, mode = result.stdout.split("runtime ", 1)[1].split("\n", 1)[0].rsplit(" ", 1)
                assert runtime == first, where
                assert len(runtime) + len("/dusk-studio/instance-0123456789abcdef.sock") < 108, where
                assert mode == "0o700", where
                assert "bound" in result.stdout, where
                assert not runtime.startswith("/var/tmp"), where
                assert not Path(runtime).exists(), ("runtime dir outlived the leg",) + where
                assert not any(long_tmp.iterdir()), ("scratch outlived the leg",) + where
        interrupted_harness = r'''
set -euo pipefail
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/common.sh"
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/scenarios.sh"
trap 'bb_end 0 >/dev/null 2>&1 || true; regress_run_exit_hooks' EXIT
bb_begin interrupted-slot
printf 'first %s\n' "$(readlink "$BB_SDIR/runtime")"
kill -INT $$
sleep 5
'''
        result = subprocess.run(["bash", "-c", interrupted_harness],
                                env=dict(child_environment, TMPDIR=str(long_tmp), DUSK_REGRESS_TEST_ROOT=str(repo),
                                         DUSK_REGRESS_SCENARIO_KEEP="0"),
                                capture_output=True, text=True, timeout=20)
        runtime = result.stdout.split("first ", 1)[1].split("\n", 1)[0]
        assert runtime and not Path(runtime).exists(), ("an interrupted leg left its runtime dir", result)
        assert not any(long_tmp.iterdir()), ("an interrupted leg left its scratch", result)
    finally:
        pipewire_socket.close()
        shutil.rmtree(pipewire_stub)
    long_tmp.rmdir()

    scripts = root / "scripts"
    runners = scripts / "regress"
    runners.mkdir(parents=True)
    shutil.copyfile(repo / "scripts/regress.sh", scripts / "regress.sh")
    stub_runners = ("linux", "mac", "windows-source")
    for platform in stub_runners:
        (runners / f"{platform}.sh").write_text(
            "#!/usr/bin/env bash\n"
            "set -eu\n"
            f"echo $$ > '{root}/{platform}.pid'\n"
            f"printf '%s\\n' \"$@\" > '{root}/{platform}.args'\n"
            "timeout --kill-after=2 60 bash -c '\n"
            f'echo $$ > "{root}/{platform}.managed"\n'
            "sleep 60 &\n"
            f'echo $! > "{root}/{platform}.child"\n'
            "wait\n' &\n"
            f"echo $! > '{root}/{platform}.timeout'\n"
            "wait\n"
        )
    (runners / "clean.sh").write_text(f"#!/usr/bin/env bash\ntouch '{root}/clean.called'\n")
    process = subprocess.Popen(
        ["bash", str(scripts / "regress.sh"), "prepush", "--clean"],
        env=child_environment,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    groups = {process.pid}
    try:
        markers = [root / f"{platform}.{kind}"
                   for platform in stub_runners
                   for kind in ("pid", "timeout", "managed", "child")]
        wait_for(lambda: all(marker.exists() and marker.read_text().strip()
                             for marker in markers))
        children = [int(marker.read_text()) for marker in markers]
        for pid in children:
            group = os.getpgid(pid)
            groups.add(group)
            assert group != process.pid, "runner shares the caller's process group"
        assert (root / "linux.args").read_text().splitlines() == ["--gui-scenarios"], \
            "prepush did not enable GUI scenarios by default"
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 130, (process.returncode, stdout, stderr)
        wait_for(lambda: not any(running(pid) for pid in children))
        log_dir = Path(stdout.split("logs in ", 1)[1].splitlines()[0])
        assert log_dir.is_dir(), "an interrupted prepush removed its logs"
        assert not (root / "clean.called").exists(), "an interrupted prepush --clean cleaned the hosts"
    finally:
        for group in groups:
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()

    logs_root = root / "prepush-logs"
    logs_runners = logs_root / "scripts/regress"
    logs_runners.mkdir(parents=True)
    shutil.copyfile(repo / "scripts/regress.sh", logs_root / "scripts/regress.sh")
    for runner, platform in (("linux", "linux"), ("mac", "mac"), ("windows-source", "windows"),
                             ("windows", "windows")):
        (logs_runners / f"{runner}.sh").write_text(
            "#!/usr/bin/env bash\n"
            f'status="${{REGRESS_TEST_{platform.upper()}:-PASS}}"\n'
            'printf "args:%s\\n" "$*"\n'
            '[[ "$status" != INT ]] || exit 130\n'
            'if [[ "$status" == SLEEP ]]; then\n'
            '    sleep 3600 &\n'
            '    child=$!\n'
            f'    trap \'kill $child; echo $$ >"{logs_root}/{runner}.term"; exit 143\' TERM\n'
            f'    echo $$ >"{logs_root}/{runner}.sleeping"\n'
            '    wait "$child"\n'
            'fi\n'
            f'[[ -z "${{DUSK_REGRESS_RECORD_FILE:-}}" ]] || printf "{platform}\\t%s\\tstub\\t0\\t\\n" "$status" >>"$DUSK_REGRESS_RECORD_FILE"\n'
            '[[ "$status" == PASS ]]\n'
        )
    clean_trace = logs_root / "clean.args"
    (logs_runners / "clean.sh").write_text(
        "#!/usr/bin/env bash\n"
        f"printf '%s\\n' \"$@\" >'{clean_trace}'\n"
        'status="${REGRESS_TEST_CLEAN:-PASS}"\n'
        '[[ -z "${DUSK_REGRESS_RECORD_FILE:-}" ]] || printf "mac\\t%s\\tclean\\t0\\t\\n" "$status" >>"$DUSK_REGRESS_RECORD_FILE"\n'
        '[[ "$status" == PASS ]]\n'
    )
    logs_tmp = logs_root / "tmp"
    logs_tmp.mkdir()
    week = 7 * 24 * 3600

    def log_folder(name):
        folder = logs_tmp / name
        folder.mkdir()
        (folder / "linux.log").write_text("old\n")
        return folder

    def age(path, seconds):
        stamp = time.time() - seconds
        for item in [*path.iterdir(), path]:
            os.utime(item, (stamp, stamp))

    stale = log_folder("dusk-prepush.stale")
    fresh = log_folder("dusk-prepush.fresh")
    owned = log_folder("dusk-prepush.owned")
    other = log_folder("dusk-regress-all.stale")
    capture = log_folder("dusk-capture.stale")
    activation = log_folder("duskstudio-gnome-activation.stale")
    file_drop = log_folder("duskstudio-gnome-drop.stale")
    native_activation = log_folder("duskstudio-native-activation.stale")
    native_gui = log_folder("duskstudio-native-gui.stale")
    native_embed = log_folder("duskstudio-gnome-native-embed.stale")
    asked_to_keep = log_folder("duskstudio-x11-drop.stale")
    owner_lock = open(owned / ".lock", "w")
    fcntl.flock(owner_lock, fcntl.LOCK_EX)
    for folder in (stale, owned, other, capture, activation, file_drop, native_activation, native_gui,
                   native_embed, asked_to_keep):
        age(folder, week + 3600)
    age(fresh, week - 3600)

    def prepush_logs(**statuses):
        result = subprocess.run(["/bin/bash", str(logs_root / "scripts/regress.sh"), "prepush"],
                                env=dict(child_environment, TMPDIR=str(logs_tmp), **statuses),
                                capture_output=True, text=True, timeout=20)
        return result, Path(result.stdout.split("logs in ", 1)[1].splitlines()[0])

    try:
        result, log_dir = prepush_logs()
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert "regress prepush: OK" in result.stdout, result.stdout
        assert log_dir.parent == logs_tmp, log_dir
        assert not log_dir.exists(), "a passing prepush kept its logs"
        assert str(log_dir) not in result.stdout.split("RESULT", 1)[1], result.stdout
        assert not stale.exists(), "a week-old prepush folder was not pruned"
        for folder in (capture, activation, file_drop, native_activation, native_gui, native_embed):
            assert not folder.exists(), f"a week-old {folder.name} kept by a failed leg was not pruned"
        for folder in (fresh, owned, other, asked_to_keep):
            assert folder.is_dir(), f"pruning removed {folder.name}"

        result, log_dir = prepush_logs(REGRESS_TEST_MAC="FAIL")
        assert result.returncode == 1, (result.stdout, result.stderr)
        verdict = result.stdout.split("regress prepush: FAILED", 1)[1]
        assert log_dir.is_dir() and (log_dir / "mac.log").is_file(), "a failing prepush lost its logs"
        assert str(log_dir) in verdict, result.stdout
        assert not clean_trace.exists(), "prepush without --clean cleaned the hosts"
    finally:
        owner_lock.close()
    assert owned.is_dir()

    def regress(*arguments, **statuses):
        clean_trace.unlink(missing_ok=True)
        environment = dict(child_environment, TMPDIR=str(logs_tmp), **statuses)
        environment.pop("DUSK_REGRESS_RECORD_FILE")
        result = subprocess.run(["/bin/bash", str(logs_root / "scripts/regress.sh"), *arguments],
                                env=environment, capture_output=True, text=True, timeout=20)
        cleaned = clean_trace.read_text().splitlines() if clean_trace.exists() else None
        return result, cleaned

    result, cleaned = regress("prepush", "--clean", "--host", "u@h")
    assert result.returncode == 0 and "regress prepush: OK" in result.stdout, (result.stdout, result.stderr)
    assert cleaned == ["all", "--host", "u@h"], cleaned
    assert "mac/clean" in result.stdout, result.stdout
    result, cleaned = regress("prepush", "--clean", REGRESS_TEST_CLEAN="FAIL")
    assert result.returncode == 1 and "regress prepush: FAILED" in result.stdout, result.stdout
    assert cleaned == ["all"], cleaned
    result, cleaned = regress("prepush", "--clean", REGRESS_TEST_WINDOWS="FAIL")
    assert result.returncode == 1 and cleaned == ["all"], ("a failed prepush --clean did not clean", result.stdout)
    for arguments, host in ((["mac", "--clean", "--host", "u@h"], ["mac", "--host", "u@h"]),
                            (["windows-source", "--clean"], ["windows"]),
                            (["clean", "windows"], ["windows"])):
        result, cleaned = regress(*arguments)
        assert result.returncode == 0, (arguments, result.stdout, result.stderr)
        assert cleaned == host, (arguments, cleaned)
        assert "--clean" not in result.stdout, ("--clean reached a platform runner", result.stdout)
    result, cleaned = regress("mac", "--clean", REGRESS_TEST_MAC="FAIL")
    assert result.returncode == 1 and cleaned == ["mac"], (result.returncode, cleaned)
    result, cleaned = regress("mac", "--clean", REGRESS_TEST_MAC="INT")
    assert result.returncode == 130 and cleaned is None, ("an interrupted run was cleaned", result.returncode, cleaned)
    result, cleaned = regress("mac", REGRESS_TEST_MAC="PASS")
    assert result.returncode == 0 and cleaned is None, cleaned
    result, cleaned = regress("linux", "--clean")
    assert result.returncode == 2 and cleaned is None, (result.returncode, result.stderr)
    result, cleaned = regress("all", "--clean", REGRESS_TEST_MAC="INT")
    assert result.returncode == 130 and cleaned is None, ("an interrupted all --clean cleaned", result.stdout)
    assert result.stdout.count("args:") == 2 and "stopped in mac" in result.stdout, \
        ("all ran on after an interrupt", result.stdout)
    result, cleaned = regress("all", "--clean")
    assert result.returncode == 0 and cleaned == ["all"], (result.stdout, cleaned)

    # A TERM to the --clean wrapper reaches the runner, and nothing is cleaned.
    clean_trace.unlink(missing_ok=True)
    term_environment = dict(child_environment, TMPDIR=str(logs_tmp), REGRESS_TEST_MAC="SLEEP")
    term_environment.pop("DUSK_REGRESS_RECORD_FILE")
    wrapper = subprocess.Popen(["/bin/bash", str(logs_root / "scripts/regress.sh"), "mac", "--clean"],
                               env=term_environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        sleeping = logs_root / "mac.sleeping"
        wait_for(lambda: sleeping.exists() and sleeping.read_text().strip())
        wrapper.send_signal(signal.SIGTERM)
        stdout, stderr = wrapper.communicate(timeout=10)
        assert wrapper.returncode == 143, (wrapper.returncode, stdout, stderr)
        assert (logs_root / "mac.term").exists(), "the TERM did not reach the runner"
        assert not clean_trace.exists(), "a terminated run was cleaned"
    finally:
        stub = logs_root / "mac.sleeping"
        for pid, kill in ((wrapper.pid, os.killpg),
                          (int(stub.read_text()) if stub.exists() else None, os.kill)):
            if pid is not None and running(pid):
                try:
                    kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        wrapper.wait()

    # The three runners of a prepush take their locks at the same moment, and
    # after a reboot the lock folder is not there yet: every one of them has to
    # get its lock, whichever makes the folder.
    taker = (
        'source "$1/scripts/regress/common.sh"\n'
        'until [[ -e "$2" ]]; do :; done\n'
        'regress_host_lock "$3" shared && echo locked\n')
    for round_ in range(5):
        race_dir = root / f"lock-race-{round_}" / "dusk-regress"
        go = root / f"lock-race-{round_}.go"
        takers = [subprocess.Popen(["bash", "-c", taker, "taker", str(repo), str(go), host],
                                   env=dict(child_environment, DUSK_REGRESS_LOCK_DIR=str(race_dir)),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                  for host in ("mac", "windows", "mac", "windows", "mac", "windows")]
        go.touch()
        outcomes = [taker_process.communicate(timeout=20) for taker_process in takers]
        assert all(out.strip() == "locked" for out, _ in outcomes), ("a lock taker lost the race", outcomes)
        assert oct(race_dir.stat().st_mode & 0o777) == "0o700", oct(race_dir.stat().st_mode)
    for name, make in (("open-mode", lambda d: d.mkdir(mode=0o755)),
                       ("a-link", lambda d: d.symlink_to(root / "lock-race-0" / "dusk-regress"))):
        bad = root / f"lock-bad-{name}" / "dusk-regress"
        bad.parent.mkdir()
        make(bad)
        result = subprocess.run(["bash", "-c", 'source "$1/scripts/regress/common.sh"; regress_host_lock mac shared',
                                 "taker", str(repo)], env=dict(child_environment, DUSK_REGRESS_LOCK_DIR=str(bad)),
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and "not a private folder" in result.stderr, (name, result)

    def lock_is_held(host):
        with open(locks / f"{host}.lock", "a") as probe:
            try:
                fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
            fcntl.flock(probe, fcntl.LOCK_UN)
            return False

    shutil.copyfile(repo / "scripts/regress/mac.sh", runners / "mac.sh")
    shutil.copyfile(repo / "scripts/regress/common.sh", runners / "common.sh")
    (root / "DONOR_REV").write_text("0" * 40 + "\n")
    (root / "DAF_REV").write_text("2" * 40 + "\n")
    mocks = root / "bin"
    mocks.mkdir()
    remote_home = root / "remote"
    remote_home.mkdir()
    mock_scripts = {
        "git": "#!/bin/bash\nprintf '0123456789abcdef\\n'\n",
        "ssh": "#!/usr/bin/python3\nimport subprocess, sys, time\n"
               "if ' dusk-cancel ' in sys.argv[-1]: time.sleep(0.1)\n"
               "launcher = \"import os, sys\\nif os.fork(): os._exit(0)\\n"
               "os.setsid()\\nos.execv('/bin/bash', ['bash', '-c', sys.argv[1]])\"\n"
               "p = subprocess.Popen([sys.executable, '-c', launcher, sys.argv[-1]], "
               "stdout=subprocess.PIPE, stderr=subprocess.PIPE)\n"
               "out, err = p.communicate()\n"
               "sys.stdout.buffer.write(out)\nsys.stderr.buffer.write(err)\nsys.exit(p.returncode)\n",
        "bash": "#!/bin/bash\n"
                "if [[ $1 != -s ]]; then exec /bin/bash \"$@\"; fi\n"
                "sleep 60 &\n"
                f"echo $! > '{root}/remote-child'\n"
                "wait\n",
    }
    for name, contents in mock_scripts.items():
        path = mocks / name
        path.write_text(contents)
        path.chmod(0o755)
    environment = dict(child_environment, HOME=str(remote_home),
                       PATH=f"{mocks}:{os.environ['PATH']}")
    process = subprocess.Popen(
        ["/bin/bash", str(scripts / "regress.sh"), "prepush"], env=environment,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    remote_pid = None
    try:
        marker = root / "remote-child"
        wait_for(lambda: marker.exists() and marker.read_text().strip())
        remote_pid = int(marker.read_text())
        # SSH disconnects leave this separate remote group alive.
        assert os.getpgid(remote_pid) != process.pid
        assert lock_is_held("mac"), "a running mac leg does not hold the mac host lock"
        refused = subprocess.run(["/bin/bash", str(repo / "scripts/regress/clean.sh"), "mac"],
                                 env=environment, capture_output=True, text=True, timeout=10)
        assert refused.returncode == 1 and "a run on this box is using the mac host" in refused.stdout, \
            ("a clean ran beside a live mac run", refused.stdout, refused.stderr)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=20)
        assert process.returncode == 130, (process.returncode, stdout, stderr)
        log_dir = Path(stdout.split("logs in ", 1)[1].splitlines()[0])
        mac_log = (log_dir / "mac.log").read_text()
        assert "not acknowledged" not in mac_log, mac_log
        wait_for(lambda: not running(remote_pid), "the remote run outlived the cancellation prepush waited for")
    finally:
        for pid in (remote_pid, process.pid):
            if pid is not None and running(pid):
                try:
                    os.killpg(os.getpgid(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        process.wait()

    cache = root / "cache" / "gui-case-ms.tsv"
    environment = dict(child_environment, DUSK_REGRESS_TEST_ROOT=str(repo),
                       DUSK_REGRESS_TEST_CACHE=str(cache))
    harness = r'''
set -euo pipefail
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/common.sh"
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/scenarios.sh"
GUI_TIMES_FILE="$DUSK_REGRESS_TEST_CACHE"
DUSK_REGRESS_GUI_SHARDS=1
scenarios_list() { printf 'gui.stub\n'; }
sandboxed_app_run() {
    printf '[RUN] gui.stub\n[PASS] gui.stub (7 ms)\n=== scenarios: 1 pass, 0 fail, 0 skip ===\n'
}
scenarios_gui_sharded_leg scenarios-gui 5
'''
    result = subprocess.run(["bash", "-c", harness], env=environment,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert cache.read_text() == "gui.stub\t7\n", "fresh timing cache was not published"

    configured_source = remote_home / "source $literal; dedicated"
    configured_daf = remote_home / "DAF $literal; dedicated"
    configured_helper = remote_home / "configure $literal; dedicated"
    app_build = configured_source / "app build"
    test_build = configured_source / "test build"
    trace = root / "configured-trace.jsonl"
    configured_source.mkdir()
    configured_daf.mkdir()
    for build in (app_build, test_build):
        (build / "_deps/dusk-plugins").mkdir(parents=True)
        (build / "CMakeCache.txt").write_text("DUSK_PLUGINS_PATH:PATH=\n")
    fake_app = app_build / "DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio"
    fake_app.parent.mkdir(parents=True)
    recorder = '''#!/usr/bin/python3
import json,os,pathlib,sys
with open(os.environ['REGRESS_TEST_TRACE'],'a') as out:
    out.write(json.dumps({'command':pathlib.Path(sys.argv[0]).name,'args':sys.argv[1:],
                          'cwd':os.getcwd(),'fixtures':os.environ.get('DUSKSTUDIO_FIXTURE_DIR'),
                          'tmpdir':os.environ.get('TMPDIR')})+'\\n')
'''
    git_mock = recorder + '''
args=sys.argv[1:]
cwd=os.getcwd()
if args[:1]==['-C']:
    cwd=args[1]
    args=args[2:]
if args[:1]==['pull']:
    sys.exit('Dedicated validation must not pull a developer dependency')
if args[:1]==['checkout'] and '--detach' in args and os.environ.get('REGRESS_TEST_RESTORE_FAIL'):
    sys.exit('error: pathspec did not match')
if args[:1]==['status'] and cwd==os.environ['REGRESS_TEST_DAF_ROOT']:
    if os.environ.get('REGRESS_TEST_DAF_DIRTY'): print(' M file')
elif args[:1]==['config']:
    print(os.environ['REGRESS_TEST_MARKER'])
elif args[:1]==['rev-parse']:
    if '--abbrev-ref' in args: print('HEAD')
    elif cwd==os.environ['REGRESS_TEST_DAF_ROOT']: print(os.environ['REGRESS_TEST_DAF_HEAD'])
    elif cwd.endswith('/_deps/dusk-plugins'): print('0'*40)
    else: print('1'*40)
'''
    for name, contents in {
        "git": git_mock,
        "bash": '#!/bin/bash\nexec /bin/bash "$@"\n',
        "ssh": "#!/usr/bin/python3\nimport subprocess,sys\n"
               "sys.exit(subprocess.run(['/bin/bash','-c',sys.argv[-1]],"
               "start_new_session=True).returncode)\n",
        "mktemp": '''#!/usr/bin/python3
import os,sys,tempfile
args=sys.argv[1:]
if '-t' not in args: os.execv('/usr/bin/mktemp',['mktemp']+args)
prefix=args[args.index('-t')+1]+'-'
if '-d' in args: print(tempfile.mkdtemp(prefix=prefix,dir=os.environ['REGRESS_TEST_TMP']))
else:
    fd,path=tempfile.mkstemp(prefix=prefix,dir=os.environ['REGRESS_TEST_TMP'])
    os.close(fd)
    print(path)
''',
        "cmake": recorder,
        "ctest": recorder,
        "sw_vers": "#!/bin/bash\necho 26.6.2\n",
    }.items():
        path = mocks / name
        path.write_text(contents)
        path.chmod(0o755)
    configured_helper.write_text(recorder)
    configured_helper.chmod(0o755)
    fake_app.write_text(recorder + "print('[PASS] fixture (1 ms)')\n"
                        "print('=== scenarios: 1 pass, 0 fail, 0 skip ===')\n")
    fake_app.chmod(0o755)
    remote_tmp = root / "remote-tmp"
    remote_tmp.mkdir()
    environment = dict(child_environment, HOME=str(remote_home), PATH=f"{mocks}:{os.environ['PATH']}",
                       REGRESS_TEST_TRACE=str(trace), REGRESS_TEST_MARKER="true",
                       REGRESS_TEST_DAF_HEAD="2" * 40, REGRESS_TEST_DAF_ROOT=str(configured_daf),
                       REGRESS_TEST_TMP=str(remote_tmp),
                       DUSK_REGRESS_MAC_REPO=str(configured_source),
                       DUSK_REGRESS_MAC_DAF=str(configured_daf),
                       DUSK_REGRESS_MAC_CONFIGURE=str(configured_helper),
                       DUSK_REGRESS_MAC_BUILD_DIR="app build",
                       DUSK_REGRESS_MAC_TEST_BUILD_DIR="test build",
                       DUSK_REGRESS_MAC_FREE_COLD_GB="0", DUSK_REGRESS_MAC_FREE_WARM_GB="0")

    def configured_run(overrides):
        trace.write_text("")
        result = subprocess.run(["/bin/bash", str(runners / "mac.sh")],
                                env=dict(environment, **overrides), capture_output=True,
                                text=True, timeout=30)
        calls = [json.loads(line) for line in trace.read_text().splitlines()]
        return result, calls

    def run_folders(home):
        return sorted((home / ".cache/dusk-regress").glob("regress-*"))

    # A restore that fails is a failed row, the exit tries it again, and the
    # run's folder still goes after that second try.
    result, calls = configured_run({"REGRESS_TEST_RESTORE_FAIL": "1"})
    assert result.returncode != 0 and "FAIL   mac-restore" in result.stdout, (result.stdout, result.stderr)
    assert result.stderr.count("error: could not restore") == 2, result.stderr
    assert not run_folders(remote_home), ("a failed restore left the run's folder", run_folders(remote_home))

    short_home = Path(tempfile.mkdtemp(prefix="d", dir="/tmp"))
    try:
        result, calls = configured_run({"HOME": str(short_home)})
        assert result.returncode == 0, (result.stdout, result.stderr)
        ctest = next(call for call in calls if call["command"] == "ctest")
        run_tmp = Path(ctest["tmpdir"])
        assert run_tmp.name == "t" and run_tmp.parent.parent == short_home / ".cache/dusk-regress", ctest
        assert len(str(run_tmp)) + len("/dusk-si-XXXXXX/dusk-studio/instance.sock") < 104, run_tmp
        assert not run_tmp.parent.exists() and not run_folders(short_home), "the run's folder outlived it"
        result, calls = configured_run({"HOME": str(short_home), "DUSK_REGRESS_MAC_FREE_WARM_GB": "999999"})
        assert result.returncode != 0 and "a warm run needs 999999 GB" in result.stderr, (result.stdout, result.stderr)
        assert not any(call["command"] == "cmake" for call in calls), calls
        assert not run_folders(short_home), "a refused run left its folder"
    finally:
        shutil.rmtree(short_home)

    result, calls = configured_run({})
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "[PASS] footprint" in result.stdout and "warm trees app build " in result.stdout, result.stdout
    assert not run_folders(remote_home), ("a passing run left its folder", run_folders(remote_home))
    # This HOME is too long for a socket path under the run's folder, so the
    # tests keep the node's TMPDIR.
    assert all(call["tmpdir"] == environment["TMPDIR"] for call in calls), calls
    assert any(call["command"] == configured_helper.name and
               call["args"][0] == str(app_build) for call in calls), calls
    assert any(call["command"] == configured_helper.name and
               call["args"][0] == str(test_build) for call in calls), calls
    for build in (app_build, test_build):
        assert any(call["command"] == "cmake" and
                   call["args"][:2] == ["--build", str(build)] for call in calls), calls
    assert any(call["command"] == "ctest" and
               call["args"][:2] == ["--test-dir", str(test_build)] for call in calls), calls
    assert any(call["command"] == "DuskStudio" and
               call["fixtures"] == f"{test_build}:{configured_source}/tests/fixtures"
               for call in calls), calls
    for call in calls:
        assert "/src/dusk-studio" not in str(call), call
        assert "/src/DAF" not in str(call), call
        assert call["args"][:1] != ["pull"], call

    result, calls = configured_run({
        "DUSK_REGRESS_MAC_REPO": configured_source.name,
        "DUSK_REGRESS_MAC_DAF": configured_daf.name,
        "DUSK_REGRESS_MAC_CONFIGURE": configured_helper.name,
        "DUSK_REGRESS_MAC_BUILD_DIR": str(app_build),
        "DUSK_REGRESS_MAC_TEST_BUILD_DIR": str(test_build),
    })
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert any(call["command"] == "ctest" and
               call["args"][:2] == ["--test-dir", str(test_build)] for call in calls), calls

    for overrides in ({"REGRESS_TEST_MARKER": "false"},
                      {"REGRESS_TEST_DAF_HEAD": "3" * 40},
                      {"REGRESS_TEST_DAF_DIRTY": "1"},
                      {"DUSK_REGRESS_MAC_CONFIGURE": ""},
                      {"DAF_REV": "main\n"}):
        daf_rev = overrides.pop("DAF_REV", "2" * 40 + "\n")
        (root / "DAF_REV").write_text(daf_rev)
        result, calls = configured_run(overrides)
        (root / "DAF_REV").write_text("2" * 40 + "\n")
        assert result.returncode != 0, result.stdout
        assert not any(call["args"][:1] == ["push"] for call in calls), calls
        assert not any(call["command"] == configured_helper.name for call in calls), calls

    linux_root = root / "linux-missing-app"
    linux_runners = linux_root / "scripts/regress"
    linux_runners.mkdir(parents=True)
    for name in ("linux.sh", "common.sh", "xvfb.sh", "scenarios.sh", "null-audio.sh", "capture.sh"):
        shutil.copyfile(repo / "scripts/regress" / name, linux_runners / name)
    required_tools = linux_root / "bin"
    required_tools.mkdir()
    for name in ("cmake", "ctest", "Xvfb", "timeout", "flock", "pgrep"):
        tool = required_tools / name
        tool.write_text("#!/bin/sh\necho 'unexpected tool invocation' >&2\nexit 1\n")
        tool.chmod(0o755)
    for arguments in (["--scenarios-only"], ["--scenarios-only", "--gui-scenarios"]):
        result = subprocess.run(
            ["/bin/bash", str(linux_runners / "linux.sh"), *arguments],
            env=dict(child_environment, PATH=f"{required_tools}:{os.environ['PATH']}"),
            capture_output=True, text=True, timeout=5,
        )
        assert result.returncode != 0, result.stdout
        assert "app binary missing:" in result.stdout + result.stderr, result
        assert "regress linux: OK" not in result.stdout, result.stdout

    win_root = root / "windows-source"
    win_runners = win_root / "scripts/regress"
    (win_runners / "windows").mkdir(parents=True)
    shutil.copyfile(repo / "scripts/regress.sh", win_root / "scripts/regress.sh")
    for name in ("windows-source.sh", "common.sh"):
        shutil.copyfile(repo / "scripts/regress" / name, win_runners / name)
    shutil.copytree(repo / "scripts/regress/windows/source", win_runners / "windows/source")
    (win_root / ".github/workflows").mkdir(parents=True)
    snapshot_step = (
        "      - name: Check regression profile snapshots\n"
        "        shell: powershell\n"
        "        run: |\n"
        "          foreach ($snapshotTemp in @($env:TEMP, $env:TEMP.ToLowerInvariant())) {\n"
        "            $env:TEMP = $snapshotTemp\n"
        "            .\\tests\\windows_regress_snapshot.ps1\n"
        "          }\n"
        "\n"
        "      - name: Clone JUCE 8.0.4\n"
        "        shell: bash\n")
    workflow = win_root / ".github/workflows/windows-tests.yml"
    workflow.write_text(
        snapshot_step +
        "          for required in \\\n"
        "            \"cross-platform-validator-contract\"; do\n"
        "            grep -Fq \"$required\" <<< \"$inventory\" || exit 1\n"
        "          for required in \\\n"
        "            \"FileImporter: a matching 32-bit source is not truncated to 24-bit\"; do\n"
        "          ctest --test-dir build-tests --output-on-failure\n")
    for platform in ("linux", "mac"):
        (win_runners / f"{platform}.sh").write_text("#!/usr/bin/env bash\nexit 0\n")
    git_environment = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                           GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                           GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")

    def git(*args):
        return subprocess.run(["git", "-C", str(win_root), *args], env=git_environment,
                              check=True, capture_output=True, text=True).stdout.strip()

    git("init", "-q")
    git("add", "-A")
    git("commit", "-q", "-m", "base")
    base_sha = git("rev-parse", "HEAD")
    git("update-ref", "refs/remotes/origin/main", base_sha)
    (win_root / "local.txt").write_text("local\n")
    git("add", "-A")
    git("commit", "-q", "-m", "local")
    head_sha = git("rev-parse", "HEAD")

    win_mocks = win_root / "bin"
    win_mocks.mkdir()
    win_trace = win_root / "ssh-trace"
    copied = win_root / "copied"
    copied.mkdir()
    required = ["cross-platform-validator-contract",
                "FileImporter: a matching 32-bit source is not truncated to 24-bit"]
    (win_mocks / "ssh").write_text(f'''#!/usr/bin/python3
import base64, os, re, sys, time
command = sys.argv[-1]
encoded = re.search(r"-EncodedCommand (\\S+);", command)
if encoded:
    script = base64.b64decode(encoded.group(1)).decode("utf-16-le")
    kind = re.search(r"^\\$rgScript = '([a-z]+)'", script, re.M).group(1)
    assert "function Remove-RegressTree" in script, "inline-lib.ps1 did not travel with " + kind
    assert not re.search(r"^\\s*#", script, re.M), "comment lines travelled with " + kind
    command = "; ".join(line for line in script.splitlines()
                        if re.match(r"\\$(root|coldGb|warmGb) = ", line))
else:
    kind = re.search(r"\\\\regress\\\\([a-z]+)\\.cmd", command).group(1)
with open("{win_trace}", "a") as out:
    out.write(kind + "\\t" + command + "\\n")
if kind == "build" and os.environ.get("REGRESS_TEST_BUILD_HANG"):
    with open("{win_root}/build-ssh.pid", "w") as out:
        out.write(str(os.getpid()))
    time.sleep(60)
if kind == "build" and os.environ.get("REGRESS_TEST_BUILD_FAIL"):
    print("C:\\\\t\\\\src\\\\src\\\\a.cpp(1): error C2065: y: undeclared identifier\\r")
    print("FAILED: a.obj")
    sys.exit(1)
if kind == "ctest" and " list" in command:
    print("Total Tests: 2")
    for name in os.environ.get("REGRESS_TEST_INVENTORY", "").split("|"):
        print("  Test #1: " + name + "\\r")
elif kind == "ctest":
    print("100% tests passed, 0 tests failed out of 2")
elif kind == "snapshot":
    for needle in ("Set-Location -LiteralPath (Join-Path $root 'src')", "$env:TEMP = $tmp",
                   "foreach ($snapshotTemp in", ".\\\\tests\\\\windows_regress_snapshot.ps1",
                   "$ErrorActionPreference = 'Stop'", "Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass",
                   "try {{", "}} catch {{"):
        assert needle in script, ("the snapshot step is not the workflow's", needle, script)
    assert script.index("$env:TEMP = $tmp") < script.index("foreach ($snapshotTemp in"), script
    if os.environ.get("REGRESS_TEST_SNAPSHOT_FAIL"):
        print("The snapshot missed owned fixture entries\\r")
        sys.exit(1)
elif kind == "tidy":
    print("removed C:\\\\t\\\\regress\\\\head.bundle (0.0 GB)\\r")
    print("regress:part src 2147483648\\r")
    print("regress:part src\\\\build-tests 1610612736\\r")
    print("regress:footprint 3221225472 10737418240 C:\\\\\\r")
elif kind == "clean":
    if os.environ.get("REGRESS_TEST_CLEAN_REFUSE"):
        print("error: C:\\\\t has no .dusk-regress-tree marker, so it is not removed.\\r")
        print("regress:free 1073741824 C:\\\\\\r")
        sys.exit(1)
    print("removed the source tree C:\\\\t (3.0 GB)\\r")
    print("removed the package run folder C:\\\\l\\\\dusk-regress-1 (0.1 GB)\\r")
    print("warn: left alone, no .dusk-regress-run marker: C:\\\\l\\\\dusk-regress-2\\r")
    print("regress:free 21474836480 C:\\\\\\r")
''')
    (win_mocks / "scp").write_text(f'''#!/usr/bin/python3
import shutil, sys
for arg in sys.argv[1:-1]:
    if arg.endswith((".cmd", ".ps1", ".bundle")):
        shutil.copy(arg, "{copied}")
''')
    for name in ("ssh", "scp"):
        (win_mocks / name).chmod(0o755)
    win_environment = dict(child_environment, PATH=f"{win_mocks}:{os.environ['PATH']}",
                           REGRESS_TEST_INVENTORY="|".join(required + ["Other test"]))
    win_environment.pop("DUSK_REGRESS_RECORD_FILE")

    def windows_source(overrides):
        win_trace.write_text("")
        result = subprocess.run(["/bin/bash", str(win_runners / "windows-source.sh")],
                                env=dict(win_environment, **overrides), capture_output=True,
                                text=True, timeout=20)
        calls = [line.split("\t", 1) for line in win_trace.read_text().splitlines()]
        return result, calls

    result, calls = windows_source({})
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert [kind for kind, _ in calls] == ["preflight", "update", "snapshot", "build", "mesa", "ctest", "ctest", "tidy"], calls
    assert "[PASS] snapshot-check" in result.stdout, result.stdout
    assert calls[0][1] == "$root = 'C:\\Users\\marc\\dusk-audit-win'; $coldGb = '11'; $warmGb = '6'", calls[0]
    assert "[PASS] footprint" in result.stdout, result.stdout
    assert "warm tree C:\\Users\\marc\\dusk-audit-win 3.0 GB, C:\\ 10.0 GB free" in result.stdout, result.stdout
    assert "tree parts: src 2.0 GB; src\\build-tests 1.5 GB" in result.stdout, result.stdout
    update = calls[1][1]
    assert f"update.cmd {head_sha} {base_sha} C:\\Users\\marc\\dusk-audit-win\\regress\\head.bundle; exit $LASTEXITCODE" in update, update
    assert (copied / "head.bundle").exists()
    for script in ("update.cmd", "build.cmd", "env.cmd", "verify-mesa.ps1"):
        text = (copied / script).read_bytes()
        assert b"\r\n" in text and b"\n" not in text.replace(b"\r\n", b""), script
    for leg in ("vm-preflight", "update", "build", "mesa", "ctest-inventory", "ctest"):
        assert f"[PASS] {leg} " in result.stdout, (leg, result.stdout)

    result, calls = windows_source({"REGRESS_TEST_INVENTORY": required[0]})
    assert result.returncode != 0, result.stdout
    assert "missing from CTest: " + required[1] in result.stderr, result.stderr
    assert [kind for kind, _ in calls][-2:] == ["ctest", "tidy"], calls

    result, calls = windows_source({"REGRESS_TEST_BUILD_FAIL": "1"})
    assert result.returncode != 0, result.stdout
    assert [kind for kind, _ in calls] == ["preflight", "update", "snapshot", "build", "tidy"], calls
    assert "FAIL   build" in result.stdout and "1 errors, first a.cpp(1): error C2065" in result.stdout, result.stdout
    for leg in ("mesa", "ctest-inventory", "ctest"):
        assert f"SKIP   {leg} " in result.stdout, (leg, result.stdout)

    result, calls = windows_source({"REGRESS_TEST_SNAPSHOT_FAIL": "1"})
    assert result.returncode != 0 and "FAIL   snapshot-check" in result.stdout, result.stdout
    assert [kind for kind, _ in calls][-1] == "tidy", calls
    original_workflow = workflow.read_text()
    for broken in (original_workflow.replace(snapshot_step, ""),
                   original_workflow.replace("        shell: powershell\n", "        shell: pwsh\n", 1),
                   original_workflow.replace("windows_regress_snapshot.ps1", "something_else.ps1")):
        workflow.write_text(broken)
        result, calls = windows_source({})
        assert result.returncode != 0 and "FAIL   snapshot-check" in result.stdout, result.stdout
        assert "snapshot" not in [kind for kind, _ in calls], ("a step that changed shape still ran", calls)
    workflow.write_text(original_workflow)

    result, calls = windows_source({"DUSK_REGRESS_WIN_ROOT": "C:\\a b"})
    assert result.returncode != 0 and not calls, (result.stdout, calls)
    result, calls = windows_source({"DUSK_REGRESS_WIN_FREE_WARM_GB": "lots"})
    assert result.returncode != 0 and not calls, (result.stdout, calls)
    result, calls = windows_source({"DUSK_REGRESS_WIN_FREE_COLD_GB": "45", "DUSK_REGRESS_WIN_FREE_WARM_GB": "12"})
    assert calls[0][1].endswith("$coldGb = '45'; $warmGb = '12'"), calls[0]

    shutil.copyfile(repo / "scripts/regress/clean.sh", win_runners / "clean.sh")

    def windows_clean(*arguments, **overrides):
        win_trace.write_text("")
        result = subprocess.run(["/bin/bash", str(win_root / "scripts/regress.sh"), "clean", *arguments],
                                env=dict(win_environment, **overrides), capture_output=True,
                                text=True, timeout=20)
        return result, [line.split("\t", 1)[0] for line in win_trace.read_text().splitlines()]

    result, kinds = windows_clean("windows")
    assert result.returncode == 0 and kinds == ["clean"], (result.stdout, result.stderr, kinds)
    assert "[WARN] clean" in result.stdout and "marc@192.168.122.85: 2 removed, C:\\ 20.0 GB free" in result.stdout, result.stdout
    assert "warn: left alone, no .dusk-regress-run marker" in result.stdout, result.stdout
    result, kinds = windows_clean("windows", REGRESS_TEST_CLEAN_REFUSE="1")
    assert result.returncode == 1 and "[FAIL] clean" in result.stdout, result.stdout
    assert "has no .dusk-regress-tree marker" in result.stdout.split("[FAIL] clean", 1)[1], result.stdout
    held = open(locks / "windows.lock", "a")
    fcntl.flock(held, fcntl.LOCK_SH)
    try:
        result, kinds = windows_clean("windows")
        assert result.returncode == 1 and not kinds, ("a clean ran beside a live run", result.stdout, kinds)
        assert "a run on this box is using the windows guest" in result.stdout, result.stdout
    finally:
        held.close()
    result, kinds = windows_clean("nowhere")
    assert result.returncode != 0 and not kinds, (result.stdout, kinds)

    git("checkout", "-q", "--detach", base_sha)
    result, calls = windows_source({})
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert f"update.cmd {base_sha} {base_sha}; exit" in calls[1][1], calls[1]
    git("checkout", "-q", "--detach", head_sha)

    for arguments in (["--msi", "x.msi"], ["--fixtures-run", "1"], ["--release-run", "1"]):
        result = subprocess.run(["/bin/bash", str(win_root / "scripts/regress.sh"), "prepush", *arguments],
                                env=dict(win_environment, DUSK_REGRESS_DRY_RUN="1"),
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 2, (arguments, result.stdout, result.stderr)
    for arguments, expected in (([], "--gui-scenarios"),
                                (["--gui-scenarios"], "--gui-scenarios"),
                                (["--no-scenarios"], "--no-scenarios --gui-scenarios")):
        result = subprocess.run(["/bin/bash", str(win_root / "scripts/regress.sh"), "prepush", *arguments],
                                env=dict(win_environment, DUSK_REGRESS_DRY_RUN="1"),
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, result.stderr
        assert f"linux:   {expected}\n" in result.stdout, result.stdout
        assert "windows: source leg" in result.stdout, result.stdout

    win_trace.write_text("")
    process = subprocess.Popen(
        ["/bin/bash", str(win_root / "scripts/regress.sh"), "prepush"],
        env=dict(win_environment, REGRESS_TEST_BUILD_HANG="1"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    hung = None
    try:
        marker = win_root / "build-ssh.pid"
        wait_for(lambda: marker.exists() and marker.read_text().strip())
        hung = int(marker.read_text())
        assert lock_is_held("windows"), "a running windows-source leg does not hold the windows host lock"
        kinds_before = len(win_trace.read_text().splitlines())
        refused = subprocess.run(["/bin/bash", str(win_root / "scripts/regress.sh"), "clean", "windows"],
                                 env=win_environment, capture_output=True, text=True, timeout=10)
        assert refused.returncode == 1 and "a run on this box is using the windows guest" in refused.stdout, \
            ("a clean ran beside a live windows-source run", refused.stdout, refused.stderr)
        assert len(win_trace.read_text().splitlines()) == kinds_before, "the refused clean reached the guest"
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=20)
        assert process.returncode == 130, (process.returncode, stdout, stderr)
        wait_for(lambda: not running(hung), "the Windows leg's ssh outlived the interrupt")
        kinds = [line.split("\t", 1)[0] for line in win_trace.read_text().splitlines()]
        assert kinds[-1] == "cancel", kinds
        log_dir = Path(stdout.split("logs in ", 1)[1].splitlines()[0])
        windows_log = (log_dir / "windows.log").read_text()
        assert "windows-source started" in windows_log, windows_log
        assert "stopping the run in" in windows_log, windows_log
        assert "could not confirm" not in windows_log, windows_log
    finally:
        for pid in (hung, process.pid):
            if pid is not None and running(pid):
                try:
                    os.killpg(os.getpgid(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        process.wait()

    capture_root = root / "capture-leg"
    (capture_root / "docs/images").mkdir(parents=True)
    capture_tmp = capture_root / "tmp"
    capture_tmp.mkdir()
    capture_trace = capture_root / "trace"
    (capture_root / "docs/capture-screenshots.sh").write_text(
        "#!/usr/bin/env bash\n"
        f"printf '%s\\n%s\\n' \"$DUSK_CAPTURE_OUT\" \"$DUSK_CAPTURE_BIN\" >'{capture_trace}'\n"
        'mkdir -p "$DUSK_CAPTURE_OUT" && : >"$DUSK_CAPTURE_OUT/a.png"\n'
        'case "$CAPTURE_TEST_MODE" in\n'
        '  pass) echo "All 3 \u2705 figures are in $DUSK_CAPTURE_OUT." >&2 ;;\n'
        '  missing) printf "error: missing figure: %s\\n" b.png c.png >&2\n'
        '           echo "error: 2 of the 3 \u2705 figures are missing" >&2; exit 1 ;;\n'
        'esac\n'
    )
    capture_tools = capture_root / "bin"
    capture_tools.mkdir()
    for name in ("magick", "pipewire"):
        tool = capture_tools / name
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
    capture_records = capture_root / "records.tsv"
    capture_harness = r'''
set -euo pipefail
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/common.sh"
source "$DUSK_REGRESS_TEST_ROOT/scripts/regress/capture.sh"
REPO_ROOT="$CAPTURE_TEST_ROOT"
REGRESS_PLATFORM=linux
trap regress_run_exit_hooks EXIT
regress_capture_leg /opt/under-test/DuskStudio
'''

    def capture_leg(mode, **extra):
        capture_records.write_text("")
        capture_trace.unlink(missing_ok=True)
        result = subprocess.run(
            ["bash", "-c", capture_harness],
            env=dict(child_environment, DUSK_REGRESS_TEST_ROOT=str(repo),
                     DUSK_REGRESS_RECORD_FILE=str(capture_records),
                     CAPTURE_TEST_ROOT=str(capture_root), CAPTURE_TEST_MODE=mode,
                     TMPDIR=str(capture_tmp), PATH=f"{capture_tools}:{os.environ['PATH']}",
                     **extra),
            capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, (result.stdout, result.stderr)
        rows = [line.split("\t") for line in capture_records.read_text().splitlines()]
        assert len(rows) == 1 and rows[0][2] == "capture-screenshots", rows
        out = Path(capture_trace.read_text().splitlines()[0]) if capture_trace.exists() else None
        return rows[0][1], rows[0][4], out

    status, note, out = capture_leg("pass")
    assert status == "PASS" and note == "3 figures", (status, note)
    assert out.parent.parent == capture_tmp, out
    assert capture_trace.read_text().splitlines()[1] == "/opt/under-test/DuskStudio"
    assert not out.parent.exists(), "a passing capture leg kept its scratch folder"

    status, note, out = capture_leg("missing")
    assert status == "FAIL" and "missing b.png, c.png" in note, (status, note)
    assert out.parent.is_dir() and f"output in {out.parent}" in note, note
    assert (out.parent / "capture.log").is_file()

    status, note, out = capture_leg("silent")
    assert status == "FAIL" and "summary line" in note, (status, note)

    status, note, out = capture_leg("pass", DUSK_REGRESS_REAL_AUDIO="1")
    assert status == "SKIP" and out is None, (status, note)
    assert not any((capture_root / "docs/images").iterdir()), "the capture leg wrote to docs/images"

    # The Mac goes back to its previous commit after a run. A submodule only the
    # tested commit has must not stay behind as an untracked checkout.
    host = root / "submodule-host"
    git_env = dict(child_environment, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1",
                   GIT_AUTHOR_NAME="regress", GIT_AUTHOR_EMAIL="regress@example.invalid",
                   GIT_COMMITTER_NAME="regress", GIT_COMMITTER_EMAIL="regress@example.invalid")

    def git(cwd, *args):
        return subprocess.run(["git", "-c", "protocol.file.allow=always", "-c", "init.defaultBranch=main",
                               *args], cwd=cwd, env=git_env, capture_output=True, text=True,
                              check=True).stdout.strip()

    for name in ("kept", "added"):
        origin = root / f"submodule-{name}"
        origin.mkdir()
        git(origin, "init", "-q")
        (origin / "file").write_text(name)
        git(origin, "add", "file")
        git(origin, "commit", "-q", "-m", name)
    host.mkdir()
    git(host, "init", "-q")
    git(host, "submodule", "add", "-q", str(root / "submodule-kept"), "external/kept")
    git(host, "commit", "-q", "-m", "previous")
    previous = git(host, "rev-parse", "HEAD")
    # git quotes this path in its default ls-tree output.
    added = "external/added n\u00e4me"
    git(host, "submodule", "add", "-q", str(root / "submodule-added"), added)
    git(host, "commit", "-q", "-m", "tested")
    tested = git(host, "rev-parse", "HEAD")
    git(host, "checkout", "-q", previous)
    assert (host / added / ".git").is_file(), "switching back left no checkout to clean up"
    assert git(host, "status", "--porcelain", "--ignore-submodules=all"), "the leftover is not untracked"
    drop = 'set -euo pipefail; source "$1/scripts/regress/common.sh"; regress_drop_added_submodules "$2"'
    for _ in range(2):
        result = subprocess.run(["bash", "-c", drop, "drop", str(repo), tested], cwd=host, env=git_env,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, (result.stdout, result.stderr)
    assert not (host / added).exists(), "the added submodule's checkout stayed"
    assert (host / "external/kept/file").is_file(), "a submodule the previous commit has was removed"
    assert (host / ".git/modules" / added).is_dir(), "the added submodule's clone was removed"
    assert git(host, "status", "--porcelain", "--ignore-submodules=all") == "", "the tree is not clean"

    # The Mac clean, against a node home with one of everything it must
    # remove and everything it must leave.
    node = Path(tempfile.mkdtemp(prefix="dusk-node-", dir=root))
    slots = node / ".cache/dusk-regress"
    for name in ("regress-dead0001-111", "regress-nopid-222", "regress-live-333", "other"):
        (slots / name / "t").mkdir(parents=True)
    finished = subprocess.Popen(["true"])
    finished.wait()
    (slots / "regress-dead0001-111/pid").write_text(f"{finished.pid}\n")
    # Every live fixture outlives the test until its teardown ends it.
    live = subprocess.Popen(["bash", "-c", "sleep 3600; exit 0", "dusk-regress", "regress-live-333"],
                            start_new_session=True, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (slots / "regress-live-333/pid").write_text(f"{live.pid}\n")
    (slots / "configure-016-selection.sh").write_text("not the runner's\n")
    for name, marked in (("20261021-223044", False), ("20261115-123300", True), ("notes", False),
                         ("20261116-000001", True), ("20261116-000002", True), ("20261116-000003", True)):
        (node / "dusk-regress-pkg" / name).mkdir(parents=True)
        if marked:
            (node / "dusk-regress-pkg" / name / ".dusk-regress-run").write_text("run\n")
    checkout = node / "src/dusk-studio"
    (checkout / "build").mkdir(parents=True)
    git(checkout, "init", "-q")
    (checkout / "file").write_text("x\n")
    git(checkout, "add", "file")
    git(checkout, "commit", "-q", "-m", "base")
    for branch in ("regress-0123abcd", "regress-pkg-0123456789ab", "regress-feature", "regress-deadbeef"):
        git(checkout, "branch", branch)
    git(checkout, "worktree", "add", "-q", str(node / "src/dusk-studio-regress"), "regress-deadbeef")
    git(checkout, "checkout", "-q", "-b", "regress-a772befe")
    clean_root = root / "mac-clean"
    clean_runners = clean_root / "scripts/regress"
    (clean_runners / "mac").mkdir(parents=True)
    shutil.copyfile(repo / "scripts/regress.sh", clean_root / "scripts/regress.sh")
    for name in ("clean.sh", "common.sh", "mac/clean-node.sh"):
        shutil.copyfile(repo / "scripts/regress" / name, clean_runners / name)
    clean_bin = clean_root / "bin"
    clean_bin.mkdir()
    (clean_bin / "ssh").write_text("#!/usr/bin/python3\nimport subprocess,sys\n"
                                   "sys.exit(subprocess.run(['/bin/bash','-c',sys.argv[-1]]).returncode)\n")
    mounts = clean_root / "mounts.txt"
    (clean_bin / "mount").write_text(f"#!/bin/sh\ncat '{mounts}' 2>/dev/null\nexit 0\n")
    for name in ("ssh", "mount"):
        (clean_bin / name).chmod(0o755)
    shutil.copyfile(shutil.which("sleep"), clean_bin / "caffeinate")
    (clean_bin / "caffeinate").chmod(0o755)
    quiet = dict(start_new_session=True, stdin=subprocess.DEVNULL,
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # A group leader alive under the slot's pid, but not this run's; a live
    # caffeinate; a live pid a run listed; a disk image still mounted.
    recycled = subprocess.Popen(["sleep", "3600"], **quiet)
    (slots / "regress-recycled-444" / "t").mkdir(parents=True)
    (slots / "regress-recycled-444/pid").write_text(f"{recycled.pid}\n")
    caffeinate = subprocess.Popen([str(clean_bin / "caffeinate"), "3600"], **quiet)

    def process_text(pid, name):
        try:
            return Path(f"/proc/{pid}/{name}").read_bytes().replace(b"\0", b" ").decode().strip()
        except (FileNotFoundError, ProcessLookupError):
            return ""

    wait_for(lambda: process_text(live.pid, "cmdline").endswith(" dusk-regress regress-live-333")
             and process_text(recycled.pid, "comm") == "sleep"
             and process_text(caffeinate.pid, "comm") == "caffeinate", "the live fixtures did not come up")
    pkg = node / "dusk-regress-pkg"
    (pkg / "20261116-000001/caffeinate.pid").write_text(f"{caffeinate.pid}\n")
    (pkg / "20261116-000002/pids").write_text(f"{recycled.pid}\n")
    (pkg / "20261116-000003/mnt").mkdir()
    mounts.write_text(f"/dev/disk9s1 on {pkg}/20261116-000003/mnt (hfs, local, read-only)\n")
    clean_environment = dict(git_env, HOME=str(node), PATH=f"{clean_bin}:{os.environ['PATH']}")

    def clean_mac():
        return subprocess.run(["/bin/bash", str(clean_root / "scripts/regress.sh"), "clean", "mac"],
                              env=clean_environment, capture_output=True, text=True, timeout=30)

    try:
        result = clean_mac()
        assert result.returncode == 1 and "[FAIL] clean" in result.stdout, (result.stdout, result.stderr)
        assert "6 removed" in result.stdout, result.stdout
        assert "dusk-studio-regress is checked out on regress-deadbeef" in result.stdout, result.stdout
        assert "warn: left alone, process group" in result.stdout and "no longer this run's" in result.stdout, result.stdout
        for why in ("its caffeinate", "which it started, is alive", "still mounted"):
            assert f"error: left alone, " in result.stdout and why in result.stdout, (why, result.stdout)
        assert running(caffeinate.pid) and running(recycled.pid), "the clean signalled a process"
        assert sorted(p.name for p in slots.iterdir()) == \
            ["configure-016-selection.sh", "other", "regress-live-333", "regress-recycled-444"]
        assert sorted(p.name for p in pkg.iterdir()) == \
            ["20261116-000001", "20261116-000002", "20261116-000003", "notes"]
        branches = git(checkout, "for-each-ref", "--format=%(refname:short)", "refs/heads").split()
        assert sorted(branches) == ["main", "regress-a772befe", "regress-deadbeef", "regress-feature"], branches
        assert (checkout / "build").is_dir() and "dusk-studio/build" in result.stdout, result.stdout

        for process in (caffeinate, recycled):
            process.kill()
            process.wait()
        mounts.unlink()
        result = clean_mac()
        assert result.returncode == 0 and "[WARN] clean" in result.stdout, (result.stdout, result.stderr)
        assert "4 removed" in result.stdout and "checked out on regress-a772befe" in result.stdout, result.stdout
        assert sorted(p.name for p in slots.iterdir()) == ["configure-016-selection.sh", "other", "regress-live-333"]
        assert sorted(p.name for p in pkg.iterdir()) == ["notes"]
    finally:
        for process in (live, caffeinate, recycled):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            process.wait()

    # The ownership checks are what keeps a clean off someone else's folder, so
    # each script that must carry one is held to it.
    source = repo / "scripts/regress/windows/source"
    preflight = (source / "preflight.ps1").read_text()
    for needle in ("'.dusk-regress-tree'", "'stamps'", "'regress\\env.cmd'", "Set-Content -LiteralPath $rgMarker",
                   "Test-RegressReparse $root"):
        assert needle in preflight, ("preflight.ps1 lost its ownership check", needle)
    assert preflight.index("Remove-RegressTree $leftover") < preflight.index("Get-RegressFree $root"), \
        "preflight measures the free space before it tidies"
    clean_ps = (source / "clean.ps1").read_text()
    for needle in ("Test-Path -LiteralPath $rgMarker -PathType Leaf", "Remove-RegressTree $root '.dusk-regress-tree'",
                   "'.dusk-regress-run'", "Remove-RegressTree $run.FullName '.dusk-regress-run'"):
        assert needle in clean_ps, ("clean.ps1 lost its marker check", needle)
    lib = "\n".join(line for line in (source / "inline-lib.ps1").read_text().splitlines()
                    if not line.lstrip().startswith("#"))
    assert "rd /s /q" in lib and "Remove-Item" not in lib and "/MIR" not in lib, \
        "inline-lib.ps1 removes through links again"
    cleanup = (repo / "scripts/regress/windows/cleanup.ps1").read_text()
    assert "Remove-RegressTree $rgRoot '.dusk-regress-run'" in cleanup and "Remove-Item -Recurse" not in cleanup, \
        "cleanup.ps1 lost its marker-last removal"
    for name in ("install.ps1", "installed.ps1"):
        assert "Set-Content -Path \"$rgRoot\\.dusk-regress-run\"" in (repo / "scripts/regress/windows" / name).read_text(), \
            (name, "writes no run marker")
    assert "touch \"$HOME/$RUN/pids\"" in (repo / "scripts/regress/mac-package.sh").read_text() and \
        "/.dusk-regress-run" in (repo / "scripts/regress/mac-package.sh").read_text(), "mac-package.sh writes no run marker"
    assert "kill" not in (repo / "scripts/regress/mac/clean-node.sh").read_text().replace("killed", ""), \
        "clean-node.sh signals a process"

    # When this box has PowerShell, every guest script must parse.
    pwsh = shutil.which("pwsh")
    if pwsh:
        parse = root / "parse.ps1"
        parse.write_text(
            "$bad = 0\n"
            "foreach ($f in $args) {\n"
            "  $t = (Get-Content -Raw -LiteralPath $f) -replace '@@[A-Z]+@@', '1'\n"
            "  $e = $null\n"
            "  [void][System.Management.Automation.Language.Parser]::ParseInput($t, [ref]$null, [ref]$e)\n"
            "  foreach ($x in $e) { \"${f}: line $($x.Extent.StartLineNumber): $($x.Message)\"; $bad++ }\n"
            "}\n"
            "exit $bad\n")
        scripts_ps1 = sorted(str(p) for p in (repo / "scripts/regress/windows").rglob("*.ps1"))
        result = subprocess.run([pwsh, "-NoProfile", "-File", str(parse), *scripts_ps1],
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, ("a guest script does not parse", result.stdout, result.stderr)

print("regression runner: interrupt cleanup, runtime dirs under a long TMPDIR, prepush log folders, "
      "capture leg, timing cache, Mac paths, "
      "missing app, Windows source leg, added submodule checkouts, the host locks and the host clean pass"
      + ("" if shutil.which("pwsh") else "; no pwsh here, so the guest scripts were not parsed"))
