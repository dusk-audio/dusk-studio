"""Pick from every MASTERING list with the compositor's own pointer.

The in-app case gui.mastering_real_list_picks says where to click on stderr
("[real-input] click <x> <y> <what>", in the main window's pixels, and
"[real-input] window <x> <y> <w> <h>" whenever the window moves) and checks what
each click did. This driver makes each click through the private GNOME session's
RemoteDesktop pointer, so a press reaches the app the way the user's does: under
native Wayland, and under X11 through Xwayland, one app run each. On
"[real-input] cover" a GTK window opens over the app and, clicked, takes the keyboard, and
"[real-input] uncover" closes it.
"""

import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gnome_file_drop import Desktop, Failure
from gnome_file_drop import read_lines, wait_for

ACTIVATION = Path(__file__).resolve().parent / "gnome_window_activation.py"

CASE = "gui.mastering_real_list_picks"


class Cover:
    """gnome_window_activation.py's GTK window, maximized over the app. Mutter keeps
    a window opened after the user's last click from taking the keyboard, so a click
    on it gives it the keyboard, as the user's would; the case checks the app saw
    its window lose it."""

    def __init__(self, desktop, log):
        self.log = str(log)
        self.process = subprocess.Popen([sys.executable, "-I", str(ACTIVATION), "cover"],
                                        env=dict(os.environ, GTK_A11Y="none"),
                                        stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        if not wait_for(lambda: "MAPPED" in read_lines(self.log), 20):
            self.close()
            raise Failure("the cover window did not map")
        time.sleep(1.0)
        desktop.click(960, 540)
        # A click that missed it would have landed on the app instead.
        if not wait_for(lambda: any(line.startswith("POINTER ") for line in read_lines(self.log)), 5, step=0.1):
            self.close()
            raise Failure("the click meant for the cover window did not reach it")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
        time.sleep(0.5)


def run_backend(desktop, binary, work, backend):
    root = Path(__file__).resolve().parents[2]
    base = work / backend
    config, music = base / "config", base / "music"
    config.mkdir(parents=True)
    music.mkdir()
    (config / "app-config.properties").write_text("scan_plugins_on_startup=0\n")
    env = dict(os.environ, DUSKSTUDIO_CONFIG_DIR=str(config), DUSKSTUDIO_MUSIC_DIR=str(music),
               DUSKSTUDIO_REAL_INPUT_DRIVER=backend, DUSKSTUDIO_RUN_SCENARIOS="gui:" + CASE,
               DUSKSTUDIO_FIXTURE_DIR=str(root / "build-tests") + ":" + str(root / "tests/fixtures"))
    if backend == "wayland":
        env["DUSKSTUDIO_NATIVE_WAYLAND"] = "1"
    else:
        env.pop("DUSKSTUDIO_NATIVE_WAYLAND", None)
        if not env.get("DISPLAY"):
            raise Failure("the session has no Xwayland display")
    out_path, err_path = base / "app.out", base / "app.err"
    origin = (0, 0)
    clicks = 0
    cover = None
    with out_path.open("w") as output, err_path.open("w") as errors:
        app = subprocess.Popen([binary], env=env, stdout=output, stderr=errors)
        deadline = time.monotonic() + 240
        offset = 0
        try:
            while app.poll() is None:
                if time.monotonic() > deadline:
                    raise Failure(backend + ": the app did not finish the case in time")
                with err_path.open("rb") as stream:
                    stream.seek(offset)
                    chunk = stream.read()
                complete = chunk[:chunk.rfind(b"\n") + 1]
                offset += len(complete)
                for line in complete.decode(errors="replace").splitlines():
                    if not line.startswith("[real-input] "):
                        continue
                    words = line.split()
                    if words[1] == "window":
                        origin = (int(words[2]), int(words[3])) if backend == "x11" else (0, 0)
                    elif words[1] == "move":
                        desktop.at(origin[0] + int(words[2]), origin[1] + int(words[3]))
                    elif words[1] == "shift":
                        origin = (origin[0] - int(words[2]), origin[1] - int(words[3]))
                    elif words[1] == "cover":
                        print(f"{backend}: another window takes the keyboard", flush=True)
                        cover = Cover(desktop, base / "cover.log")
                    elif words[1] == "uncover" and cover is not None:
                        cover.close()
                        cover = None
                    elif words[1] == "click":
                        x, y = origin[0] + int(words[2]), origin[1] + int(words[3])
                        print(f"{backend}: click {x},{y} to {' '.join(words[4:])}", flush=True)
                        desktop.click(x, y)
                        clicks += 1
                time.sleep(0.05)
        finally:
            if cover is not None:
                cover.close()
            if app.poll() is None:
                app.kill()
                app.wait()
    output = out_path.read_text(errors="replace")
    errors_text = err_path.read_text(errors="replace")
    for line in output.splitlines():
        if CASE in line or line.startswith("       "):
            print(f"{backend}: {line}", flush=True)
    if app.returncode != 0:
        raise Failure(f"{backend}: the app exited with status {app.returncode}")
    if backend == "wayland" and "using the native Wayland display" not in errors_text:
        raise Failure("the app did not use native Wayland")
    if "[PASS] " + CASE not in output:
        raise Failure(f"{backend}: {CASE} did not pass after {clicks} real clicks")


def drive(binary, work):
    desktop = Desktop()
    failures = []
    for backend in ("wayland", "x11"):
        try:
            run_backend(desktop, binary, work, backend)
        except Failure as error:
            failures.append(str(error))
    if failures:
        raise Failure("; ".join(failures))
    print("Real pointer picks from every MASTERING list passed under native Wayland and X11", flush=True)


if __name__ == "__main__":
    try:
        if len(sys.argv) != 4 or sys.argv[1] != "drive":
            raise Failure("usage: native_mastering_lists.py drive <app> <work>")
        drive(sys.argv[2], Path(sys.argv[3]))
    except (Failure, OSError, subprocess.TimeoutExpired) as error:
        print("FAIL native mastering lists: " + str(error), flush=True)
        sys.exit(1)
