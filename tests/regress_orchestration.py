#!/usr/bin/env python3
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


repo = Path(sys.argv[1]).resolve()


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("regression runner did not reach the expected state")


def running(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0] != "Z"
    except FileNotFoundError:
        return False


with tempfile.TemporaryDirectory(prefix="dusk-regress-contract-") as scratch:
    root = Path(scratch)
    scripts = root / "scripts"
    runners = scripts / "regress"
    runners.mkdir(parents=True)
    shutil.copyfile(repo / "scripts/regress.sh", scripts / "regress.sh")
    for platform in ("linux", "mac"):
        (runners / f"{platform}.sh").write_text(
            "#!/usr/bin/env bash\n"
            "set -eu\n"
            f"echo $$ > '{root}/{platform}.pid'\n"
            "timeout --kill-after=2 60 bash -c '\n"
            f'echo $$ > "{root}/{platform}.managed"\n'
            "sleep 60 &\n"
            f'echo $! > "{root}/{platform}.child"\n'
            "wait\n' &\n"
            f"echo $! > '{root}/{platform}.timeout'\n"
            "wait\n"
        )
    process = subprocess.Popen(
        ["bash", str(scripts / "regress.sh"), "prepush"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    groups = {process.pid}
    try:
        markers = [root / f"{platform}.{kind}"
                   for platform in ("linux", "mac")
                   for kind in ("pid", "timeout", "managed", "child")]
        wait_for(lambda: all(marker.exists() and marker.read_text().strip()
                             for marker in markers))
        children = [int(marker.read_text()) for marker in markers]
        for pid in children:
            group = os.getpgid(pid)
            groups.add(group)
            assert group != process.pid, "runner shares the caller's process group"
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 130, (process.returncode, stdout, stderr)
        wait_for(lambda: not any(running(pid) for pid in children))
    finally:
        for group in groups:
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()

    shutil.copyfile(repo / "scripts/regress/mac.sh", runners / "mac.sh")
    shutil.copyfile(repo / "scripts/regress/common.sh", runners / "common.sh")
    (root / "DONOR_REV").write_text("0" * 40 + "\n")
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
    environment = dict(os.environ, HOME=str(remote_home),
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
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=20)
        assert process.returncode == 130, (process.returncode, stdout, stderr)
        log_dir = Path(stdout.split("logs in ", 1)[1].splitlines()[0])
        mac_log = (log_dir / "mac.log").read_text()
        assert "not acknowledged" not in mac_log, mac_log
        assert not running(remote_pid), "prepush returned before remote cancellation finished"
    finally:
        for pid in (remote_pid, process.pid):
            if pid is not None and running(pid):
                try:
                    os.killpg(os.getpgid(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        process.wait()

    cache = root / "cache" / "gui-case-ms.tsv"
    environment = dict(os.environ, DUSK_REGRESS_TEST_ROOT=str(repo),
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

print("regression runner: local/remote interrupt cleanup and fresh GUI timing cache pass")
