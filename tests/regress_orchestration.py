#!/usr/bin/env python3
import json
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
    child_environment = dict(os.environ, DUSK_REGRESS_RECORD_FILE=str(root / "results.tsv"))
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
        env=child_environment,
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
                          'cwd':os.getcwd(),'fixtures':os.environ.get('DUSKSTUDIO_FIXTURE_DIR')})+'\\n')
'''
    git_mock = recorder + '''
args=sys.argv[1:]
cwd=os.getcwd()
if args[:1]==['-C']:
    cwd=args[1]
    args=args[2:]
if args[:1]==['pull']:
    sys.exit('Dedicated validation must not pull a developer dependency')
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
                       DUSK_REGRESS_MAC_DAF_REV="2" * 40)

    def configured_run(overrides):
        trace.write_text("")
        result = subprocess.run(["/bin/bash", str(runners / "mac.sh")],
                                env=dict(environment, **overrides), capture_output=True,
                                text=True, timeout=30)
        calls = [json.loads(line) for line in trace.read_text().splitlines()]
        return result, calls

    result, calls = configured_run({})
    assert result.returncode == 0, (result.stdout, result.stderr)
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
                      {"DUSK_REGRESS_MAC_DAF_REV": ""}):
        result, calls = configured_run(overrides)
        assert result.returncode != 0, result.stdout
        assert not any(call["args"][:1] == ["push"] for call in calls), calls
        assert not any(call["command"] == configured_helper.name for call in calls), calls

print("regression runner: interrupt cleanup, timing cache and dedicated Mac paths pass")
