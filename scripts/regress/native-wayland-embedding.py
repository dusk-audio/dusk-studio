#!/usr/bin/env python3
"""Run the native panel lifecycle probe in an isolated Wayland-only Mutter session."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def drive(binary, work):
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    with (work / "probe.log").open("w") as log:
        try:
            result = subprocess.run([binary], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=30)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 124
    (work / "result").write_text(str(code))
    return code


def cleanup(runtime):
    marker = ("XDG_RUNTIME_DIR=" + str(runtime)).encode()
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for entry in Path("/proc").iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                if entry.stat().st_uid == os.getuid() and marker in (entry / "environ").read_bytes().split(b"\0"):
                    os.kill(int(entry.name), sig)
            except OSError:
                pass
        time.sleep(0.2)


def interrupted(signum, frame):
    raise KeyboardInterrupt


def run(binary):
    binary = str(Path(binary).resolve())
    if not os.access(binary, os.X_OK):
        print("native panel probe is missing: " + binary)
        return 1
    for tool in ("mutter", "dbus-run-session"):
        if shutil.which(tool) is None:
            print("native panel probe requires " + tool)
            return 77

    work = Path(tempfile.mkdtemp(prefix="duskstudio-gnome-native-embed."))
    runtime, home, scratch = work / "runtime", work / "home", work / "tmp"
    for folder in (runtime, home, scratch):
        folder.mkdir(mode=0o700)
    env = dict(PATH="/usr/local/bin:/usr/bin:/bin", HOME=str(home), USER=os.environ.get("USER", ""),
               LANG="C.UTF-8", TMPDIR=str(scratch), XDG_RUNTIME_DIR=str(runtime),
               XDG_CONFIG_HOME=str(home / ".config"), XDG_DATA_HOME=str(home / ".local/share"),
               XDG_CACHE_HOME=str(home / ".cache"), XDG_STATE_HOME=str(home / ".local/state"),
               XDG_SESSION_TYPE="wayland", XDG_CURRENT_DESKTOP="GNOME", GSETTINGS_BACKEND="memory",
               LIBGL_ALWAYS_SOFTWARE="1")
    command = ["dbus-run-session", "--", "mutter", "--wayland", "--no-x11", "--headless",
               "--virtual-monitor", "1920x1080", "--wayland-display", "dusk-native-embed", "--",
               sys.executable, "-I", str(Path(__file__).resolve()), "drive", binary, str(work)]
    code = 1
    old_term = signal.signal(signal.SIGTERM, interrupted)
    try:
        with (work / "mutter.log").open("w") as log:
            try:
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=45)
                if result.returncode == 0 and (work / "result").read_text() == "0":
                    code = 0
            except (subprocess.TimeoutExpired, OSError):
                pass
            except KeyboardInterrupt:
                code = 130
    finally:
        cleanup(runtime)
        signal.signal(signal.SIGTERM, old_term)

    probe = (work / "probe.log").read_text(errors="replace") if (work / "probe.log").exists() else ""
    compositor = (work / "mutter.log").read_text(errors="replace")
    print(probe, end="")
    if "PASS native shared panels:" not in probe:
        code = 1
    for warning in ("committed initial non-empty content without acknowledging configuration",
                    "Client provided invalid window geometry"):
        if warning in compositor:
            print("FAIL native shell protocol: " + warning)
            code = 1
    if code:
        print("\n".join(compositor.splitlines()[-20:]))
        print("kept " + str(work))
    else:
        shutil.rmtree(work)
    return code


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "drive":
        raise SystemExit(drive(sys.argv[2], Path(sys.argv[3])))
    if len(sys.argv) != 2:
        raise SystemExit("usage: native-wayland-embedding.py <dusk-native-wayland-probe>")
    raise SystemExit(run(sys.argv[1]))
