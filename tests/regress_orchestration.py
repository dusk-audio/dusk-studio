#!/usr/bin/env python3
import fcntl
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
    runner_tmp = root / "tmp"
    runner_tmp.mkdir()
    child_environment = dict(os.environ, DUSK_REGRESS_RECORD_FILE=str(root / "results.tsv"),
                             TMPDIR=str(runner_tmp))
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
                            env=dict(child_environment, DUSK_REGRESS_TEST_ROOT=str(repo)),
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert not any(runner_tmp.iterdir()), "interrupted scenario scratch escaped its sandbox"

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
                   for platform in stub_runners
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
        log_dir = Path(stdout.split("logs in ", 1)[1].splitlines()[0])
        assert log_dir.is_dir(), "an interrupted prepush removed its logs"
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
    for runner, platform in (("linux", "linux"), ("mac", "mac"), ("windows-source", "windows")):
        (logs_runners / f"{runner}.sh").write_text(
            "#!/usr/bin/env bash\n"
            f'status="${{REGRESS_TEST_{platform.upper()}:-PASS}}"\n'
            f'printf "{platform}\\t%s\\tstub\\t0\\t\\n" "$status" >>"$DUSK_REGRESS_RECORD_FILE"\n'
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
    asked_to_keep = log_folder("duskstudio-gnome-drop.stale")
    owner_lock = open(owned / ".lock", "w")
    fcntl.flock(owner_lock, fcntl.LOCK_EX)
    for folder in (stale, owned, other, capture, activation, asked_to_keep):
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
        for folder in (capture, activation):
            assert not folder.exists(), f"a week-old {folder.name} kept by a failed leg was not pruned"
        for folder in (fresh, owned, other, asked_to_keep):
            assert folder.is_dir(), f"pruning removed {folder.name}"

        result, log_dir = prepush_logs(REGRESS_TEST_MAC="FAIL")
        assert result.returncode == 1, (result.stdout, result.stderr)
        verdict = result.stdout.split("regress prepush: FAILED", 1)[1]
        assert log_dir.is_dir() and (log_dir / "mac.log").is_file(), "a failing prepush lost its logs"
        assert str(log_dir) in verdict, result.stdout
    finally:
        owner_lock.close()
    assert owned.is_dir()

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
                       DUSK_REGRESS_MAC_TEST_BUILD_DIR="test build")

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
    (win_root / ".github/workflows/windows-tests.yml").write_text(
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
    kind = "cancel" if "taskkill" in script else "preflight"
    command = next(line for line in script.splitlines() if line.startswith("$root"))
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
    assert [kind for kind, _ in calls] == ["preflight", "update", "build", "mesa", "ctest", "ctest"], calls
    assert calls[0][1] == "$root = 'C:\\Users\\marc\\dusk-audit-win'", calls[0]
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
    assert [kind for kind, _ in calls][-1] == "ctest", calls

    result, calls = windows_source({"REGRESS_TEST_BUILD_FAIL": "1"})
    assert result.returncode != 0, result.stdout
    assert [kind for kind, _ in calls] == ["preflight", "update", "build"], calls
    assert "FAIL   build" in result.stdout and "1 errors, first a.cpp(1): error C2065" in result.stdout, result.stdout
    for leg in ("mesa", "ctest-inventory", "ctest"):
        assert f"SKIP   {leg} " in result.stdout, (leg, result.stdout)

    result, calls = windows_source({"DUSK_REGRESS_WIN_ROOT": "C:\\a b"})
    assert result.returncode != 0 and not calls, (result.stdout, calls)

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
    result = subprocess.run(["/bin/bash", str(win_root / "scripts/regress.sh"), "prepush", "--no-scenarios"],
                            env=dict(win_environment, DUSK_REGRESS_DRY_RUN="1"),
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert "linux:   --no-scenarios\n" in result.stdout, result.stdout
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
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=20)
        assert process.returncode == 130, (process.returncode, stdout, stderr)
        assert not running(hung), "the Windows leg's ssh outlived the interrupt"
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

print("regression runner: interrupt cleanup, prepush log folders, capture leg, timing cache, Mac paths, missing app and Windows source leg pass")
