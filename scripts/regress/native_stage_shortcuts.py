#!/usr/bin/env python3
"""Exercise shell shortcuts delivered to native children on the runner's private X display."""

import os
from pathlib import Path
import subprocess
import sys
import time


def xdo(*args, optional=False):
    result = subprocess.run(["xdotool", *map(str, args)], text=True, capture_output=True, timeout=5)
    if result.returncode and not optional:
        raise RuntimeError(f"xdotool {args}: {result.stderr.strip()}")
    return result.stdout.strip()


def wait_for(predicate, message):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        os.kill(pid, 0)
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise RuntimeError(message)


def window(class_name):
    matches = xdo("search", "--onlyvisible", "--class", f"^{class_name}$", optional=True).splitlines()
    return matches[0] if len(matches) == 1 else None


def geometry(wid):
    return {key: int(value) for key, value in
            (line.split("=", 1) for line in xdo("getwindowgeometry", "--shell", wid).splitlines())}


def focus(wid):
    bounds = geometry(wid)
    xdo("mousemove", "--window", wid, bounds["WIDTH"] // 2, bounds["HEIGHT"] // 2)
    xdo("windowfocus", "--sync", wid)
    wait_for(lambda: xdo("getwindowfocus", "-f") == wid, "native child did not receive keyboard focus")


def stage_key(wid, digit, destination):
    focus(wid)
    offset = log.stat().st_size
    xdo("key", "--clearmodifiers", f"ctrl+{digit}")
    marker = f"[MainComponent] switchToStage({destination}) enter"
    def switched():
        with log.open() as stream:
            stream.seek(offset)
            return marker in stream.read()
    wait_for(switched, f"Ctrl+{digit} from native window {wid} never reached the stage selector")


def mastering():
    stage_key(shell, 3, 3)
    return wait_for(lambda: window("dusk-studio-mastering-phase"), "Mastering Phase child did not open")


def stays_in_mastering(wid, chord):
    if xdo("getwindowfocus", "-f") != wid:
        raise RuntimeError("Phase child lost keyboard focus before capture check")
    offset = log.stat().st_size
    xdo("key", "--clearmodifiers", chord)
    time.sleep(0.5)
    with log.open() as stream:
        stream.seek(offset)
        if "switchToStage(" in stream.read():
            raise RuntimeError(f"{chord} escaped a captured or modified native key")
    if window("dusk-studio-mastering-phase") != wid:
        raise RuntimeError(f"{chord} closed the Phase child")


if __name__ == "__main__":
    pid = int(sys.argv[1])
    log = Path(sys.argv[2]) / "A.err"
    shell = wait_for(lambda: [wid for wid in xdo("search", "--onlyvisible", "--maxdepth", 1,
                                               "--pid", pid, optional=True).splitlines()
                              if xdo("getwindowname", wid, optional=True) == "Dusk Studio"],
                     "main window did not open")
    if len(shell) != 1:
        raise RuntimeError(f"expected one main window, got {shell}")
    shell = shell[0]
    xdo("windowsize", shell, 1600, 1000)

    for panel, digit, destination in (("phase", 1, 0), ("eq", 2, 1), ("limiter", 4, 2)):
        mastering()
        child = wait_for(lambda: window(f"dusk-studio-mastering-{panel}"), f"{panel} child did not open")
        stage_key(child, digit, destination)
        wait_for(lambda: not window(f"dusk-studio-mastering-{panel}"), "stage change left its native child visible")
    aux = wait_for(lambda: window("dusk-studio-aux-builtin"), "Aux Utility child did not open")
    stage_key(aux, 3, 3)
    wait_for(lambda: not window("dusk-studio-aux-builtin"), "leaving Aux left its native child visible")

    phase = wait_for(lambda: window("dusk-studio-mastering-phase"), "Phase child did not reopen")
    focus(phase)
    for chord in ("1", "ctrl+shift+1", "ctrl+alt+1", "super+1"):
        stays_in_mastering(phase, chord)
    # The Mode menu occupies the second row at the sandbox's default UI scale.
    xdo("mousemove", "--window", phase, geometry(phase)["WIDTH"] // 2, 50, "click", 1)
    time.sleep(0.3)
    stays_in_mastering(phase, "ctrl+1")
    xdo("key", "--clearmodifiers", "Escape")
    time.sleep(0.3)
    stage_key(phase, 1, 0)
    wait_for(lambda: not window("dusk-studio-mastering-phase"), "dismissed menu kept the stage shortcut")
    print("Native Mastering/Aux stage shortcuts and Phase menu capture passed", flush=True)
