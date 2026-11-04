"""Whether Dusk Studio's window comes forward in a private GNOME session.

Runs as the session command of a headless Mutter (gnome-window-activation.sh starts it), so
the app takes the production path: an Xwayland client whose window Mutter manages. The
window put in front of it is a GTK 4 window on Wayland, as most GNOME applications are,
maximized over the whole monitor. It reports its keyboard focus and every pointer event it
gets.

    gnome_window_activation.py cover               the window put in front
    gnome_window_activation.py drive <app> <work>  the test; prints PASS/FAIL lines

The app is in front when all of these hold: Mutter's _NET_ACTIVE_WINDOW and the X input
focus are its window, the only one of its windows Mutter manages; that window is shown,
not minimized; the cover window says it lost the keyboard; and the pointer held still at
the centre of the app's window reaches the app and not the cover. Before every handoff the
same checks have to show the cover in front, so a check that cannot fail does not pass a
case. One app instance runs the four cases in turn:

    launch     the app started while the cover is in front
    covered    a second launch with session B while the cover is in front
    minimized  the main window minimized, the cover in front, then session C
    handoff    M drops a marker (an unsaved edit), the cover in front, then session D:
               the save prompt holds D back and Don't Save lets it through

Each case leaves a grab of the app's window, <case>.png, in the work directory.
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gnome_file_drop import Desktop, Failure, read_lines, wait_for  # noqa: E402

KEY_M, KEY_SHIFT = 0x006D, 0xFFE1
# The save prompt is a flat panel of this colour, about 440 by 200, centred on
# the window's content, with Don't Save this far from its centre.
PROMPT_FILL = (0x1A, 0x1A, 0x20)
DONT_SAVE = (23, 58)
# Seconds an X tool gets to answer.
TOOL_TIMEOUT = 20


def run_cover():
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gio, Gtk

    def say(text):
        print(text, flush=True)

    def activate(app):
        window = Gtk.ApplicationWindow(application=app, title="dusk-cover")
        window.set_default_size(900, 700)
        motion = Gtk.EventControllerMotion()
        motion.connect("enter", lambda _c, x, y: say("POINTER enter %d %d" % (x, y)))
        motion.connect("motion", lambda _c, x, y: say("POINTER motion %d %d" % (x, y)))
        window.add_controller(motion)
        window.connect("notify::is-active", lambda w, _p: say("ACTIVE %d" % w.is_active()))
        window.maximize()
        window.present()
        say("MAPPED")

    app = Gtk.Application(application_id="audio.dusk.studio.ActivationCover",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)
    app.connect("activate", activate)
    app.run([sys.argv[0]])


def run(*args):
    """A tool's output. One that hangs fails the case that called it, not the whole leg."""
    try:
        return subprocess.run(list(args), capture_output=True, text=True, timeout=TOOL_TIMEOUT).stdout
    except subprocess.TimeoutExpired:
        raise Failure("%s did not answer within %d s" % (args[0], TOOL_TIMEOUT))


def xprop_root(name):
    return run("xprop", "-root", "-notype", name)


def window_ids(text):
    return [int(w, 16) for w in re.findall(r"0x[0-9a-fA-F]+", text.split(":", 1)[-1])]


class Cover:
    def __init__(self, work, name):
        self.log = os.path.join(work, "cover-%s.log" % name)
        self.process = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "cover"],
                                        env=dict(os.environ, GTK_A11Y="none"),
                                        stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        if not wait_for(lambda: "MAPPED" in read_lines(self.log), 20):
            self.close()
            raise Failure("the cover window did not map")
        if not wait_for(lambda: self.active() is True, 10):
            self.close()
            raise Failure("the cover window did not get the keyboard when it opened")
        time.sleep(1.0)

    def active(self):
        states = [l for l in read_lines(self.log) if l.startswith("ACTIVE ")]
        return states[-1] == "ACTIVE 1" if states else None

    def pointer_events(self):
        return sum(1 for l in read_lines(self.log) if l.startswith("POINTER "))

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=10)
        time.sleep(0.5)


class App:
    def __init__(self, binary, work, config, music):
        self.binary = binary
        self.work = work
        self.err = os.path.join(work, "app.err")
        self.env = dict(os.environ)
        self.env.update({"DUSKSTUDIO_CONFIG_DIR": config, "DUSKSTUDIO_MUSIC_DIR": music})
        self.process = None
        self.launches = 0

    def start(self, session):
        env = dict(self.env, DUSKSTUDIO_LOAD_SESSION=session)
        self.process = subprocess.Popen([self.binary], env=env, stdout=open(os.path.join(self.work, "app.out"), "w"),
                                        stderr=open(self.err, "w"))

    def hand_over(self, session):
        """A second launch naming the session; its exit status, or why it has none."""
        self.launches += 1
        name = "second-%d" % self.launches
        try:
            done = subprocess.run([self.binary, session], env=self.env, timeout=60,
                                  stdout=open(os.path.join(self.work, name + ".out"), "w"),
                                  stderr=open(os.path.join(self.work, name + ".err"), "w"))
        except subprocess.TimeoutExpired:
            return "still running after 60 s"
        return done.returncode

    def loads(self, name):
        return sum(1 for l in read_lines(self.err) if l.startswith("[Dusk Studio/Load] %s:" % name))

    def alive(self):
        return self.process is not None and self.process.poll() is None

    def processes(self):
        """This session's app processes. A copy of the same binary running on the desktop has another runtime dir."""
        out = run("pgrep", "-x", "-f", re.escape(self.binary) + "( .*)?")
        runtime = ("XDG_RUNTIME_DIR=" + os.environ["XDG_RUNTIME_DIR"]).encode()
        mine = []
        for pid in out.split():
            try:
                with open("/proc/%s/environ" % pid, "rb") as f:
                    if runtime in f.read().split(b"\0"):
                        mine.append(int(pid))
            except OSError:
                pass
        return mine

    def main_windows(self):
        """The app's windows that Mutter manages."""
        found = []
        for w in window_ids(xprop_root("_NET_CLIENT_LIST")):
            pid = re.search(r"= (\d+)", run("xprop", "-id", hex(w), "-notype", "_NET_WM_PID"))
            if pid and int(pid.group(1)) == self.process.pid:
                found.append(w)
        return found

    def geometry(self, window):
        shell = dict(line.split("=", 1) for line in
                     run("xdotool", "getwindowgeometry", "--shell", str(window)).split() if "=" in line)
        return tuple(int(shell[k]) for k in ("X", "Y", "WIDTH", "HEIGHT"))

    def grab(self, window, name):
        from PIL import Image
        path = os.path.join(self.work, name + ".png")
        try:
            subprocess.run(["import", "-window", hex(window), path], capture_output=True, timeout=TOOL_TIMEOUT)
        except subprocess.TimeoutExpired:
            return None, "%s: import did not answer within %d s" % (path, TOOL_TIMEOUT)
        try:
            return Image.open(path).convert("RGB"), path
        except OSError:
            return None, path

    def stop(self):
        if self.alive():
            self.process.kill()
            self.process.wait()


def wm_state(window):
    text = run("xprop", "-id", hex(window), "-notype", "_NET_WM_STATE", "WM_STATE")
    hidden = "_NET_WM_STATE_HIDDEN" in text
    state = re.search(r"window state: (\w+)", text)
    return hidden, state.group(1) if state else "none"


def markers(autosave):
    try:
        with open(autosave, encoding="utf-8") as f:
            return json.load(f).get("markers")
    except (OSError, ValueError):
        return None


def find_prompt(image):
    """The save prompt's (left, top, width, height) in the window grab, or None."""
    import numpy
    pixels = numpy.asarray(image).astype(int)
    mask = (numpy.abs(pixels - numpy.array(PROMPT_FILL)) <= 3).all(axis=2)
    h, w = mask.shape
    cx, cy = w // 2, h // 2
    # Inside the panel's left edge, clear of its text.
    column = mask[:, cx - 205]
    if not column[cy]:
        return None
    top, bottom = cy, cy
    while top > 0 and column[top - 1]:
        top -= 1
    while bottom < h - 1 and column[bottom + 1]:
        bottom += 1
    row = mask[top + 8]
    left, right = cx, cx
    while left > 0 and row[left - 1]:
        left -= 1
    while right < w - 1 and row[right + 1]:
        right += 1
    if not (420 <= right - left + 1 <= 460 and 180 <= bottom - top + 1 <= 220):
        return None
    return left, top, right - left + 1, bottom - top + 1


class Test:
    def __init__(self, binary, work):
        self.binary = binary
        self.work = work
        self.desktop = Desktop()
        # The first key on a fresh virtual keyboard is dropped.
        self.desktop.tap(KEY_SHIFT)
        self.failures = []
        self.started = time.time()
        self.cover = None
        self.app = None
        self.window = None

    def result(self, name, problems, detail):
        ok = not problems
        print("%s %s [%ds]: %s" % ("PASS" if ok else "FAIL", name, time.time() - self.started,
                                   detail if ok else "; ".join(problems)), flush=True)
        if not ok:
            self.failures.append(name)

    def session(self, name):
        directory = os.path.join(self.work, "sessions", name)
        os.makedirs(directory)
        path = os.path.join(directory, name + ".json")
        with open(path, "w") as f:
            f.write('{"version": 6}\n')
        return path

    def open_cover(self, name):
        self.close_cover()
        self.cover = Cover(self.work, name)

    def close_cover(self):
        if self.cover is not None:
            self.cover.close()
            self.cover = None

    def pointer_on_app(self):
        """Where the pointer, held at the centre of the app window, lands: 'app', 'cover' or why neither."""
        x, y, w, h = self.app.geometry(self.window)
        cx, cy = x + w // 2, y + h // 2
        self.desktop.at(cx, cy)
        time.sleep(0.5)
        before = self.cover.pointer_events()
        for dx, dy in ((1, 0), (0, 1), (-1, 0), (0, -1)):
            self.desktop.move(dx, dy)
            time.sleep(0.1)
        time.sleep(0.5)
        if self.cover.pointer_events() > before:
            return "cover"
        location = dict(line.split("=", 1) for line in
                        run("xdotool", "getmouselocation", "--shell").split() if "=" in line)
        under = int(location.get("WINDOW", "0"))
        at = (int(location.get("X", "-1")), int(location.get("Y", "-1")))
        if abs(at[0] - cx) > 2 or abs(at[1] - cy) > 2:
            return "neither: X has the pointer at %s, not %s" % (at, (cx, cy))
        if under != self.window:
            return "neither: X has window %s under the pointer, not %s" % (hex(under), hex(self.window))
        return "app"

    def in_front(self):
        """Why the app's window is not in front; empty when it is."""
        problems = []
        windows = self.app.main_windows()
        if windows != [self.window]:
            problems.append("the app's managed windows are %s, not just %s" % (
                [hex(w) for w in windows], hex(self.window)))
        active = window_ids(xprop_root("_NET_ACTIVE_WINDOW"))
        if active != [self.window]:
            problems.append("_NET_ACTIVE_WINDOW is %s" % ([hex(w) for w in active] or "unset"))
        focus = run("xdotool", "getwindowfocus").strip()
        if focus != str(self.window):
            problems.append("the X input focus is on %s" % (hex(int(focus)) if focus.isdigit() else "nothing"))
        hidden, state = wm_state(self.window)
        if hidden or state != "Normal":
            problems.append("the window is %s (WM_STATE %s)" % ("hidden" if hidden else "shown", state))
        if self.cover is not None:
            if self.cover.active() is not False:
                problems.append("the cover window still has the keyboard")
            landed = self.pointer_on_app()
            if landed != "app":
                problems.append("the pointer at the window's centre reached %s" % landed)
        return problems

    def cover_in_front(self):
        """Why the cover is not in front of the app before a handoff; empty when it is."""
        problems = []
        if self.cover.active() is not True:
            problems.append("the cover window does not have the keyboard")
        active = window_ids(xprop_root("_NET_ACTIVE_WINDOW"))
        if self.window in active:
            problems.append("_NET_ACTIVE_WINDOW is still the app")
        landed = self.pointer_on_app()
        if landed != "cover":
            problems.append("the pointer at the app window's centre reached %s, not the cover" % landed)
        return ["setup: " + p for p in problems]

    def common(self, problems, name):
        processes = self.app.processes()
        if processes != [self.app.process.pid]:
            problems.append("Dusk Studio processes %s, not just %d" % (processes, self.app.process.pid))
        image, shot = self.app.grab(self.window, name)
        if image is None:
            problems.append("no grab of the window (%s)" % shot)
        elif problems:
            problems.append("grab %s" % shot)
        return problems

    def handover(self, session, problems):
        rc = self.app.hand_over(session)
        if rc != 0:
            problems.append("the second launch exited %s" % rc)
        if not self.app.alive():
            raise Failure("the running instance exited on the handoff")
        return problems

    def case_launch(self, a):
        self.open_cover("launch")
        self.app.start(a)
        if not wait_for(lambda: self.app.loads("wa-a.json"), 90):
            raise Failure("the app did not load session A")
        if not any("using the X11 display" in l for l in read_lines(self.app.err)):
            raise Failure("the app did not take the Xwayland path")
        windows = wait_for(self.app.main_windows, 20)
        if not windows:
            raise Failure("Mutter manages no window of the app")
        self.window = windows[0]
        time.sleep(2.0)
        problems = self.common(self.in_front(), "launch")
        self.result("launch", problems, "session A loaded; " + self.note())

    @staticmethod
    def note():
        return "active, focused and shown; the cover lost the keyboard and the pointer"

    def case_covered(self, b):
        self.open_cover("covered")
        problems = self.cover_in_front()
        if not problems:
            problems = self.handover(b, problems)
            if not wait_for(lambda: self.app.loads("wa-b.json"), 30):
                problems.append("session B did not load")
            time.sleep(1.0)
            problems += self.in_front()
        problems = self.common(problems, "covered")
        self.result("covered", problems, "session B handed over and loaded; " + self.note())

    def case_minimized(self, c):
        self.close_cover()
        run("xdotool", "windowminimize", str(self.window))
        if not wait_for(lambda: wm_state(self.window) == (True, "Iconic"), 10):
            raise Failure("the main window did not minimize (%s)" % (wm_state(self.window),))
        self.open_cover("minimized")
        problems = self.cover_in_front()
        if not problems:
            problems = self.handover(c, problems)
            if not wait_for(lambda: self.app.loads("wa-c.json"), 30):
                problems.append("session C did not load")
            time.sleep(1.0)
            problems += self.in_front()
        problems = self.common(problems, "minimized")
        self.result("minimized", problems, "the same window came back from minimized with session C; " + self.note())

    def case_handoff(self, c, d):
        self.close_cover()
        problems = []
        not_in_front = self.in_front()
        if not_in_front:
            problems.append("setup: the app is not in front to take the M key (%s)" % "; ".join(not_in_front))
        else:
            # The autosave shows the edit landed; a key the app missed would
            # leave nothing for the prompt to guard.
            autosave = os.path.join(os.path.dirname(c), "session.json.autosave")
            for attempt in (1, 2):
                self.desktop.tap(KEY_M)
                if wait_for(lambda: markers(autosave), 25, step=1.0):
                    print("  M dropped a marker on press %d" % attempt, flush=True)
                    break
            else:
                raise Failure("M dropped no marker: the session C autosave holds none")
            self.open_cover("handoff")
            problems = self.cover_in_front()
        if not problems:
            problems = self.handover(d, problems)
            time.sleep(3.0)
            if self.app.loads("wa-d.json"):
                problems.append("session D opened without a save prompt: the marker edit did not land, or the app did not ask")
            problems += self.in_front()
            self.desktop.at(0, 0)
            time.sleep(0.5)
            image, shot = self.app.grab(self.window, "handoff-prompt")
            prompt = find_prompt(image) if image is not None else None
            if prompt is None:
                problems.append("no save prompt in the window (grab %s)" % shot)
            else:
                print("  save prompt %dx%d at %d,%d in the window" % (prompt[2], prompt[3], prompt[0], prompt[1]), flush=True)
                x, y, _, _ = self.app.geometry(self.window)
                left, top, width, height = prompt
                self.desktop.click(x + left + width // 2 + DONT_SAVE[0], y + top + height // 2 + DONT_SAVE[1])
                if not wait_for(lambda: self.app.loads("wa-d.json"), 30):
                    problems.append("Don't Save did not let session D open")
                else:
                    time.sleep(1.0)
                    problems += ["after the open: " + p for p in self.in_front()]
                    self.desktop.at(0, 0)
                    time.sleep(0.5)
                    image, shot = self.app.grab(self.window, "handoff-after")
                    if image is None or find_prompt(image) is not None:
                        problems.append("the save prompt is still up after session D opened (grab %s)" % shot)
        problems = self.common(problems, "handoff")
        self.result("handoff", problems,
                    "the save prompt held session D back in front, Don't Save opened it; " + self.note())

    def run(self):
        for d in ("config", "music"):
            os.makedirs(os.path.join(self.work, d))
        with open(os.path.join(self.work, "config", "app-config.properties"), "w") as f:
            f.write("scan_plugins_on_startup=0\nautosave_interval_sec=10\n")
        a, b, c, d = (self.session("wa-" + n) for n in "abcd")
        self.app = App(self.binary, self.work, os.path.join(self.work, "config"), os.path.join(self.work, "music"))
        cases = (("launch", lambda: self.case_launch(a)), ("covered", lambda: self.case_covered(b)),
                 ("minimized", lambda: self.case_minimized(c)), ("handoff", lambda: self.case_handoff(c, d)))
        try:
            for i, (name, case) in enumerate(cases):
                self.started = time.time()
                try:
                    case()
                except Failure as failure:
                    if self.window is not None and self.app.alive():
                        _, shot = self.app.grab(self.window, name)
                        failure = "%s; grab %s" % (failure, shot)
                    self.result(name, [str(failure)], "")
                    if not self.app.alive() or self.window is None:
                        for rest, _ in cases[i + 1:]:
                            self.result(rest, ["not run: the app is not up"], "")
                        return
        finally:
            self.close_cover()
            self.app.stop()


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "cover":
        run_cover()
        return 0
    if len(sys.argv) != 4 or sys.argv[1] != "drive":
        sys.exit(__doc__)
    try:
        test = Test(*sys.argv[2:4])
    except Failure as failure:
        print("FAIL setup [0s]: %s" % failure, flush=True)
        return 1
    try:
        test.run()
    except Failure as failure:
        test.result("setup", [str(failure)], "")
    except Exception as error:  # a crash in the driver is a failed leg, not a hang
        test.result("driver", [repr(error)], "")
    return 1 if test.failures else 0


if __name__ == "__main__":
    sys.exit(main())
