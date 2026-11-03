"""XDND drops and a late pointer device against Dusk Studio on a bare X server.

Runs inside the private Xvfb display x11-drop-input.sh starts. The drag source
and the added pointer come from x11_drop_input.c, which the shell script builds.

    x11_drop_input.py drive <app> <helper> <work> <files>   prints PASS/FAIL lines

The checks:
  - leave before data: the source sends XdndDrop and XdndLeave before it answers
    the conversion; the drop still lands and the source gets XdndFinished.
  - never answer: the source sends the same and never answers; the app still
    finishes the drop, refused, within its deadline.
  - uri-list forms: a comment line, an https link, a file://localhost/ WAV and a
    file:/// WAV in one list; only the two WAVs import.
  - link only: an https link that ends in .wav imports nothing and opens nothing.
  - late pointer: a master pointer added after the app started clicks the
    import picker's button, and the import happens.
The imports are judged from the session state the app autosaves.
"""

import json
import os
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gnome_file_drop import (SINGLE_IMPORT, MULTI_AUTO_ASSIGN, MULTI_IMPORT_DISABLED, TRACK_COLOURS,  # noqa: E402
                             Failure, find_button, find_rows, read_lines, wait_for)

LINK = "https://example.com/take.wav"
# x11-drop-input.sh writes each fixture with its own length in frames, because an
# import is saved under a name of the app's own.
FIXTURES = {24000: "early", 28800: "local host", 33600: "plain+plus", 38400: "late"}
LATE_POINTER = "dusk-late"
# Off the app window, which the app centres on the 1920x1080 screen.
PARKED = (1910, 1070)


def xdotool(*args):
    return subprocess.run(["xdotool", *args], capture_output=True, text=True, timeout=10).stdout


def file_uri(path, host=""):
    return "file://" + host + urllib.parse.quote(path)


class App:
    def __init__(self, binary, work, session_file):
        self.work = work
        self.session_file = session_file
        self.err = os.path.join(work, "app.err")
        env = dict(os.environ)
        env.update({
            "DUSKSTUDIO_CONFIG_DIR": os.path.join(work, "config"),
            "DUSKSTUDIO_MUSIC_DIR": os.path.join(work, "music"),
            "DUSKSTUDIO_LOAD_SESSION": session_file,
        })
        self.process = subprocess.Popen([binary], env=env, stdout=open(os.path.join(work, "app.out"), "w"),
                                        stderr=open(self.err, "w"))
        with open(os.path.join(work, "app.pid"), "w") as f:
            f.write("%d\n" % self.process.pid)
        if not wait_for(lambda: any("Dusk Studio/Load" in l for l in read_lines(self.err)), 60):
            self.stop()
            raise Failure("the app did not load its session")
        found = wait_for(lambda: xdotool("search", "--onlyvisible", "--pid", str(self.process.pid)).split(), 20)
        if not found:
            self.stop()
            raise Failure("no X11 window for the app")
        self.windows = found

    def window(self):
        """The largest visible window, which is the main window, and its rectangle in root coordinates."""
        best = None
        for w in self.windows:
            geometry = dict(line.split("=", 1) for line in
                            xdotool("getwindowgeometry", "--shell", w).split() if "=" in line)
            try:
                rect = tuple(int(geometry[k]) for k in ("X", "Y", "WIDTH", "HEIGHT"))
            except (KeyError, ValueError):
                continue
            if best is None or rect[2] * rect[3] > best[1][2] * best[1][3]:
                best = (w, rect)
        if best is None:
            raise Failure("cannot read the app window's geometry")
        return best

    def grab(self, name):
        from PIL import Image
        window, rect = self.window()
        path = os.path.join(self.work, name + ".png")
        subprocess.run(["import", "-window", window, path], capture_output=True, timeout=30)
        return Image.open(path).convert("RGB"), rect

    def autosave(self):
        try:
            with open(self.session_file + ".autosave", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def stop(self):
        self.process.kill()
        self.process.wait()


def imported(state):
    return sorted(FIXTURES.get(r.get("length"), "a region of %s frames" % r.get("length"))
                  for t in (state["tracks"] if state else []) for r in t.get("regions", []))


def changed_pixels(a, b):
    import numpy
    if a.size != b.size:
        return a.size[0] * a.size[1]
    diff = numpy.abs(numpy.asarray(a).astype(int) - numpy.asarray(b).astype(int)).max(axis=2)
    return int((diff > 24).sum())


class Test:
    def __init__(self, binary, helper, work, files):
        self.binary = binary
        self.helper = helper
        self.work = work
        self.files = files
        self.failures = []
        self.drags = 0
        self.last_click = 0.0

    def result(self, name, ok, detail):
        print("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
        if not ok:
            self.failures.append(name)

    def run_helper(self, *args):
        done = subprocess.run([self.helper, *map(str, args)], capture_output=True, text=True, timeout=60)
        return done.returncode, done.stdout.splitlines()

    def drag(self, lines, at, order):
        self.drags += 1
        uri_file = os.path.join(self.work, "drag%d.uri" % self.drags)
        with open(uri_file, "w", newline="") as f:
            f.write("".join(line + "\r\n" for line in lines))
        rc, out = self.run_helper("drag", int(self.window, 0), at[0], at[1], uri_file, order)
        print("  drag %d (%s): %s" % (self.drags, order, " | ".join(out)), flush=True)
        if rc != 0:
            raise Failure("the drag source failed (status %d): %s" % (rc, " ".join(out)))
        return next((l for l in out if l.startswith("FINISHED") or l == "NO-FINISHED"), "NO-FINISHED")

    def picker_button(self, colour, seconds=8):
        """The button's root position once two grabs agree: the modal moves while it opens."""
        last = [None]

        def settled():
            found = find_button(self.app.grab("picker")[0], colour)
            stable = found if found is not None and found == last[0] else None
            last[0] = found
            return stable

        found = wait_for(settled, seconds, step=0.4)
        if not found:
            return None
        _, rect = self.app.window()
        return rect[0] + found[0], rect[1] + found[1]

    def click(self, at):
        xdotool("mousemove", "--sync", str(at[0]), str(at[1]))
        time.sleep(0.2)
        xdotool("click", "1")
        self.last_click = time.time()
        time.sleep(0.5)
        # A pointer left on a button paints it in its hover colour, and the next
        # picker's button, opening in the same place, would not be found by colour.
        xdotool("mousemove", "--sync", *map(str, PARKED))

    def dismiss(self):
        xdotool("key", "Escape")
        time.sleep(0.5)

    def leave_before_data(self):
        finished = self.drag([file_uri(os.path.join(self.files, "early.wav"))], self.row(2), "leave-before-data")
        button = self.picker_button(SINGLE_IMPORT)
        if button:
            self.click(button)
        self.result("drop then leave before the data", finished == "FINISHED accepted=1" and button is not None,
                    "the source got %s; %s" % (finished, "the import picker opened" if button else "no import picker"))

    def never_answer(self):
        started = time.time()
        finished = self.drag([file_uri(os.path.join(self.files, "early.wav"))], self.row(2), "never-answer")
        seconds = time.time() - started
        self.result("source that never answers", finished == "FINISHED accepted=0" and seconds < 7,
                    "the source got %s after %.1f s" % (finished, seconds))

    def uri_list_forms(self):
        lines = ["# a comment line, which RFC 2483 allows", LINK,
                 file_uri(os.path.join(self.files, "local host.wav"), "localhost"),
                 file_uri(os.path.join(self.files, "plain+plus.wav"))]
        finished = self.drag(lines, self.row(1), "data-first")
        import_at = self.picker_button(MULTI_IMPORT_DISABLED)
        auto = find_button(self.app.grab("multi")[0], MULTI_AUTO_ASSIGN) if import_at else None
        if auto:
            _, rect = self.app.window()
            self.click((rect[0] + auto[0], rect[1] + auto[1]))
            self.click(import_at)
        elif import_at:
            self.dismiss()
        self.result("uri-list forms", finished == "FINISHED accepted=1" and auto is not None,
                    "the source got %s; %s" % (finished, "the multi-import picker opened" if auto
                                               else "no multi-import picker with Auto-assign"))

    def settled_screen(self, name):
        """A grab once the window has stopped changing, so an import still painting is not taken for the drop."""
        last = [self.app.grab(name)[0]]

        def same():
            image = self.app.grab(name)[0]
            stable = image if changed_pixels(last[0], image) == 0 else None
            last[0] = image
            return stable

        return wait_for(same, 10, step=0.5) or last[0]

    def link_only(self):
        before = self.settled_screen("before-link")
        finished = self.drag([LINK], self.row(3), "data-first")
        time.sleep(3.0)
        after, _ = self.app.grab("after-link")
        changed = changed_pixels(before, after)
        if changed > 2000:
            self.dismiss()
        self.result("link only", finished.startswith("FINISHED") and changed <= 2000,
                    "the source got %s; %d pixels changed after the drop" % (finished, changed))

    def late_pointer(self):
        rc, out = self.run_helper("add-pointer", LATE_POINTER)
        print("  add-pointer: %s" % " | ".join(out), flush=True)
        if rc != 0:
            raise Failure("could not add a pointer device: %s" % " ".join(out))
        time.sleep(1.0)
        finished = self.drag([file_uri(os.path.join(self.files, "late.wav"))], self.row(3), "data-first")
        button = self.picker_button(SINGLE_IMPORT)
        if button is None:
            self.result("late pointer", False, "the source got %s; no import picker to click" % finished)
            return
        rc, out = self.run_helper("click", LATE_POINTER, button[0], button[1], *PARKED)
        self.last_click = time.time()
        print("  click: %s" % " | ".join(out), flush=True)
        closed = wait_for(lambda: find_button(self.app.grab("after-click")[0], SINGLE_IMPORT) is None, 5, step=0.5)
        if not closed:
            self.dismiss()
        self.result("late pointer", rc == 0 and bool(closed),
                    "the added pointer's click %s the import picker" % ("closed" if closed else "did not close"))

    def run(self):
        session_dir = os.path.join(self.work, "session")
        os.makedirs(session_dir)
        for d in ("config", "music"):
            os.makedirs(os.path.join(self.work, d))
        with open(os.path.join(self.work, "config", "app-config.properties"), "w") as f:
            f.write("tape_strip_expanded_default=1\nautosave_interval_sec=10\nscan_plugins_on_startup=0\n")
        session_file = os.path.join(session_dir, "session.json")
        with open(session_file, "w") as f:
            json.dump({"version": 6, "tracks": [{"colour": "ff%02x%02x%02x" % TRACK_COLOURS[t]} for t in (1, 2, 3)]}, f)

        self.app = App(self.binary, self.work, session_file)
        try:
            xdotool("mousemove", "--sync", *map(str, PARKED))
            time.sleep(2.0)
            self.window, rect = self.app.window()
            image, rect = self.app.grab("timeline")
            rows = find_rows(image)
            print("  window %s at %s, label stripes %s" % (self.window, rect, rows), flush=True)
            x = rect[0] + rect[2] * 2 // 3
            self.row = lambda track: (x, rect[1] + rows[track][1])

            # Track 3 is still empty for the late pointer's import; the multi-import
            # auto-assigns past it.
            for check in (self.leave_before_data, self.never_answer, self.late_pointer, self.uri_list_forms,
                          self.link_only):
                try:
                    check()
                except Failure as failure:
                    self.result(check.__name__.replace("_", " "), False, str(failure))

            # The verdict reads an autosave written after the last click that could import.
            def fresh_autosave():
                try:
                    fresh = os.path.getmtime(session_file + ".autosave") > self.last_click + 1.0
                except OSError:
                    return None
                return self.app.autosave() if fresh else None

            # A click that imported nothing leaves the session clean, and then no autosave follows it.
            state = (wait_for(fresh_autosave, 15, 1.0) if self.last_click else None) or self.app.autosave()
            files = imported(state)
            print("  imported: %s" % (files or "nothing"), flush=True)
            self.result("leave before data imported", files.count("early") == 1,
                        "early.wav %s" % ("landed" if "early" in files else "missing"))
            local = files.count("local host") == 1 and files.count("plain+plus") == 1
            self.result("uri-list imported the local files only", local,
                        "%s and %s" % ("local host.wav landed" if "local host" in files else "local host.wav missing",
                                       "plain+plus.wav landed" if "plain+plus" in files else "plain+plus.wav missing"))
            self.result("late pointer imported", files.count("late") == 1,
                        "late.wav %s" % ("landed" if "late" in files else "missing"))
            self.result("nothing else imported", len(files) == len(set(files)) and set(files) <= set(FIXTURES.values()),
                        "imported %s" % (files or "nothing"))
        finally:
            self.app.stop()


def main():
    if len(sys.argv) != 6 or sys.argv[1] != "drive":
        sys.exit(__doc__)
    test = Test(*sys.argv[2:6])
    try:
        test.run()
    except Failure as failure:
        test.result("drop", False, str(failure))
    except Exception as error:  # a crash in the driver is a failed leg, not a hang
        test.result("driver", False, repr(error))
    return 1 if test.failures else 0


if __name__ == "__main__":
    sys.exit(main())
