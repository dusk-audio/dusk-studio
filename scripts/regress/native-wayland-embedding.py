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
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gnome_file_drop import Desktop, Failure, wait_for

    env = dict(os.environ)
    env.pop("DISPLAY", None)
    exchange = work / "clipboard"
    exchange.mkdir()
    env["DUSK_NATIVE_PROBE_WORK"] = str(exchange)
    processes = []
    code = 1
    with (work / "probe.log").open("w") as log:
        try:
            desktop = Desktop()
            desktop.tap(ord("x"))
            probe = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
            processes.append(probe)

            def wait_value(name, value, seconds=10):
                def ready():
                    if probe.poll() is not None:
                        raise Failure("probe exited while waiting for " + name)
                    path = exchange / name
                    return path.exists() and path.read_text() == value
                if not wait_for(ready, seconds, 0.05):
                    raise Failure("probe did not report " + name + "=" + value)

            def selection():
                result = subprocess.run(["wl-paste", "--no-newline", "--type", "text/plain"], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
                if result.returncode != 0:
                    raise Failure("external Wayland clipboard read failed: " + result.stderr.decode(errors="replace"))
                return result.stdout

            def paste(value, count):
                (exchange / "expected").write_bytes(value)
                desktop.tap(ord("p"))
                wait_value("pasted", str(count))

            wait_value("ready", "ready", 20)
            desktop.click(480, 540)
            external = ("x" * 4095 + "\u65e5\u672c\u8a9e\U0001f642\n").encode() * 64
            (exchange / "external").write_bytes(external)
            with (exchange / "external").open("rb") as source:
                publisher = subprocess.run(["wl-copy", "--type", "text/plain"], env=env,
                                           stdin=source, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            if publisher.returncode != 0 or selection() != external:
                raise Failure("external clipboard publisher did not become owner")
            desktop.click(480, 540)
            paste(external, 1)

            owned = ("child \u4e2d\u6587\U0001f642\n" * 20000).encode()
            (exchange / "payload").write_bytes(owned)
            desktop.tap(ord("c"))
            wait_value("published", "1")
            actual = selection()
            if actual != owned:
                (exchange / "actual").write_bytes(actual)
                raise Failure(f"external reader received {len(actual)} bytes instead of the child's {len(owned)} bytes")
            desktop.click(480, 540)
            paste(owned, 2)

            desktop.tap(ord("e"))
            wait_value("published", "2")
            if selection() != b"":
                raise Failure("empty publication retained old clipboard bytes")
            desktop.click(480, 540)
            paste(b"", 3)

            sibling = "sibling \u6f22\u5b57".encode()
            (exchange / "payload").write_bytes(sibling)
            desktop.click(1440, 540)
            desktop.tap(ord("c"))
            wait_value("published", "3")
            (exchange / "close-first").write_text("close")
            wait_value("first-closed", "closed")
            if selection() != sibling:
                raise Failure("closing a sibling invalidated the clipboard publication")
            desktop.click(1440, 540)
            paste(sibling, 4)
            with (exchange / "external").open("rb") as source:
                publisher = subprocess.run(["wl-copy", "--type", "text/plain"], env=env,
                                           stdin=source, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            if publisher.returncode != 0 or selection() != external:
                raise Failure("external clipboard publisher did not become owner")
            desktop.click(1440, 540)
            paste(external, 5)
            (exchange / "done").write_text("done")
            code = probe.wait(timeout=10)
        except (Failure, OSError, subprocess.TimeoutExpired) as error:
            print("FAIL native clipboard driver: " + str(error), file=log, flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
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
    for tool in ("mutter", "dbus-run-session", "wl-copy", "wl-paste"):
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
                result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=90)
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
    if "PASS native shared panels:" not in probe or "PASS native clipboard:" not in probe:
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
