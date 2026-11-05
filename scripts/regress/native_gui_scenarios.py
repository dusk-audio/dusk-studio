"""Run editor scenarios in the real application's native Wayland window."""

import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gnome_file_drop import Desktop, Failure


def drive(binary, work):
    desktop = Desktop()
    desktop.tap(0xFFE1)
    root = Path(__file__).resolve().parents[2]
    cases = ["gui.native_wayland_foreign_editors"]
    cache = Path(binary).resolve().parents[2] / "CMakeCache.txt"
    if os.environ.get("DISPLAY") and "DUSKSTUDIO_NATIVE_LV2:BOOL=ON" in cache.read_text():
        cases.append("gui.lv2_editor_reflects_state")
    config, music = work / "config", work / "music"
    config.mkdir()
    music.mkdir()
    (config / "app-config.properties").write_text("scan_plugins_on_startup=0\n")
    env = dict(os.environ, DUSKSTUDIO_NATIVE_WAYLAND="1",
               DUSKSTUDIO_CONFIG_DIR=str(config), DUSKSTUDIO_MUSIC_DIR=str(music),
               DUSKSTUDIO_RUN_SCENARIOS="gui:" + ",".join(cases),
               DUSKSTUDIO_FIXTURE_DIR=str(root / "build-tests") + ":" + str(root / "tests/fixtures"))
    with (work / "app.out").open("w") as output, (work / "app.err").open("w") as errors:
        result = subprocess.run([binary], env=env, stdout=output, stderr=errors, timeout=150)
    output = (work / "app.out").read_text(errors="replace")
    errors = (work / "app.err").read_text(errors="replace")
    print(output, end="")
    if result.returncode != 0:
        raise Failure("native GUI app exited with status " + str(result.returncode))
    if "using the native Wayland display" not in errors:
        raise Failure("app did not use native Wayland")
    for case in cases:
        if "[PASS] " + case not in output:
            raise Failure(case + " did not pass")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 4 or sys.argv[1] != "drive":
            raise Failure("usage: native_gui_scenarios.py drive <app> <work>")
        drive(sys.argv[2], Path(sys.argv[3]))
    except (Failure, OSError, subprocess.TimeoutExpired) as error:
        print("FAIL native GUI: " + str(error), flush=True)
        sys.exit(1)
