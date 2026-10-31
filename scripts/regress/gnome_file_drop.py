"""Drop files from a GTK4 file drag onto Dusk Studio's tape strip in a private GNOME session.

Runs as the session command of a headless Mutter (gnome-file-drop.sh starts it), so the
app takes the production path: WAYLAND_DISPLAY and DISPLAY are both set, and it runs
through Xwayland. The drag source offers a GdkFileList, the same content GNOME Files
offers, so Mutter relays the same XDND type list to the app, with text/plain before
text/uri-list. Input goes through org.gnome.Mutter.RemoteDesktop on the private bus.

    gnome_file_drop.py source <file>...            the drag source window
    gnome_file_drop.py drive <app> <work> <files>  the test; prints PASS/FAIL lines

The verdict is the session state the app autosaves: a WAV dropped on track 2 lands
there, a MIDI file dropped on track 3 makes it a MIDI track with the notes, and three
WAVs dropped at once each land on a track.
"""

import json
import os
import subprocess
import sys
import time

# The three fixture tracks paint their label stripes in these colours, so the
# rows can be found on screen whatever the layout.
TRACK_COLOURS = {1: (255, 0, 255), 2: (0, 255, 255), 3: (255, 255, 0)}
ROW_BACKGROUND = (20, 20, 24)
SINGLE_IMPORT = (0x2f, 0x5a, 0x3d)
MULTI_AUTO_ASSIGN = (0x2d, 0x3b, 0x48)
MULTI_IMPORT_DISABLED = (0x2d, 0x3c, 0x2a)
MONITOR = (1920, 1080)
KEY_SUPER, KEY_LEFT, KEY_RIGHT, KEY_RETURN = 0xFFEB, 0xFF51, 0xFF53, 0xFF0D


def run_source(paths):
    import gi
    gi.require_version("Gtk", "4.0")
    gi.require_version("Gdk", "4.0")
    from gi.repository import Gdk, Gio, GObject, Gtk

    def activate(app):
        window = Gtk.ApplicationWindow(application=app, title="dusk-drag-source")
        window.set_default_size(600, 600)
        label = Gtk.Label(label="\n".join(os.path.basename(p) for p in paths))
        label.set_hexpand(True)
        label.set_vexpand(True)
        source = Gtk.DragSource(actions=Gdk.DragAction.COPY)

        def prepare(_source, _x, _y):
            files = Gdk.FileList.new_from_list([Gio.File.new_for_path(p) for p in paths])
            return Gdk.ContentProvider.new_for_value(GObject.Value(Gdk.FileList, files))

        source.connect("prepare", prepare)
        source.connect("drag-begin", lambda *_: print("DRAG begin", flush=True))
        source.connect("drag-end", lambda *_: print("DRAG end", flush=True))
        label.add_controller(source)
        window.set_child(label)
        window.present()
        print("MAPPED", flush=True)

    app = Gtk.Application(application_id="audio.dusk.studio.DropTestSource",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)
    app.connect("activate", activate)
    app.run([sys.argv[0]])


class Failure(Exception):
    pass


class Desktop:
    """Pointer and keyboard through a RemoteDesktop session on the private bus."""

    def __init__(self):
        from gi.repository import Gio, GLib
        self.GLib = GLib
        self.Gio = Gio
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.session = None
        for _ in range(200):
            try:
                self.session = self.call("/org/gnome/Mutter/RemoteDesktop", "org.gnome.Mutter.RemoteDesktop",
                                         "CreateSession", None, "(o)").unpack()[0]
                break
            except GLib.Error:
                time.sleep(0.1)
        if self.session is None:
            raise Failure("no RemoteDesktop session on the private bus")
        self.call(self.session, "org.gnome.Mutter.RemoteDesktop.Session", "Start")
        # The seat gains its pointer with the first motion. The app lists its
        # pointer devices once at startup and ignores clicks from any added
        # later, so the pointer has to exist before the app starts Xwayland.
        self.move(1, 0)
        time.sleep(0.5)

    def call(self, path, interface, method, args=None, reply=None):
        return self.bus.call_sync("org.gnome.Mutter.RemoteDesktop", path, interface, method, args,
                                  self.GLib.VariantType(reply) if reply else None,
                                  self.Gio.DBusCallFlags.NONE, 5000, None)

    def notify(self, method, signature, *args):
        self.call(self.session, "org.gnome.Mutter.RemoteDesktop.Session", method,
                  self.GLib.Variant(signature, args))

    def move(self, dx, dy):
        self.notify("NotifyPointerMotionRelative", "(dd)", float(dx), float(dy))

    # Relative motion is unaccelerated for this virtual device, so a move from the
    # top-left corner lands on exact root coordinates.
    def at(self, x, y):
        self.move(-10000, -10000)
        time.sleep(0.03)
        while x > 0 or y > 0:
            step_x, step_y = min(x, 100), min(y, 100)
            self.move(step_x, step_y)
            x, y = x - step_x, y - step_y
            time.sleep(0.005)
        time.sleep(0.1)

    def button(self, pressed):
        self.notify("NotifyPointerButton", "(ib)", 272, pressed)

    def click(self, x, y):
        self.at(x, y)
        self.button(True)
        time.sleep(0.05)
        self.button(False)
        time.sleep(0.3)

    def key(self, keysym, pressed):
        self.notify("NotifyKeyboardKeysym", "(ub)", keysym, pressed)

    def tap(self, keysym, modifier=None):
        if modifier:
            self.key(modifier, True)
        self.key(keysym, True)
        time.sleep(0.03)
        self.key(keysym, False)
        if modifier:
            self.key(modifier, False)
        time.sleep(0.5)

    def drag(self, start, end):
        self.at(*start)
        self.button(True)
        time.sleep(0.2)
        steps = 40
        dx, dy = (end[0] - start[0]) // steps, (end[1] - start[1]) // steps
        for _ in range(steps):
            self.move(dx, dy)
            time.sleep(0.025)
        self.move(end[0] - start[0] - steps * dx - 2, end[1] - start[1] - steps * dy - 2)
        for _ in range(2):
            time.sleep(0.4)
            self.move(1, 1)
        time.sleep(0.4)
        self.button(False)
        time.sleep(1.0)


def wait_for(predicate, seconds, step=0.2):
    end = time.time() + seconds
    while time.time() < end:
        value = predicate()
        if value:
            return value
        time.sleep(step)
    return None


def read_lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


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
        if not wait_for(lambda: any("Dusk Studio/Load" in l for l in read_lines(self.err)), 60):
            self.stop()
            raise Failure("the app did not load its session")
        if not any("using the X11 display" in l for l in read_lines(self.err)):
            self.stop()
            raise Failure("the app did not take the Xwayland path")
        found = wait_for(lambda: self.xdotool("search", "--pid", str(self.process.pid)).split(), 20)
        if not found:
            self.stop()
            raise Failure("no X11 window for the app")
        self.windows = found

    @staticmethod
    def xdotool(*args):
        return subprocess.run(["xdotool", *args], capture_output=True, text=True).stdout

    def window(self):
        """The visible top-level window and its client rectangle in root coordinates."""
        best = None
        for w in self.windows:
            geometry = dict(line.split("=", 1) for line in
                            self.xdotool("getwindowgeometry", "--shell", w).split() if "=" in line)
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
        subprocess.run(["import", "-window", window, path], capture_output=True)
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


def audio_regions(state):
    return {(i + 1, r["file"]) for i, t in enumerate(state["tracks"] if state else []) for r in t.get("regions", [])}


def blend(colour, alpha, background):
    return tuple(round(c * alpha + b * (1 - alpha)) for c, b in zip(colour, background))


def find_rows(image):
    """Row centres of tracks 1 to 3, from their 3 px label stripes (85 % alpha)."""
    import numpy
    pixels = numpy.asarray(image).astype(int)
    rows = {}
    for track, colour in TRACK_COLOURS.items():
        target = numpy.array(blend(colour, 0.85, ROW_BACKGROUND))
        mask = (numpy.abs(pixels - target) <= 24).all(axis=2)
        # A stripe is a narrow vertical run; the console strips' colour bars are wide.
        narrow = mask & ~numpy.roll(mask, 4, axis=1) & ~numpy.roll(mask, -4, axis=1)
        ys, xs = numpy.nonzero(narrow)
        if len(ys) < 8:
            raise Failure("cannot find track %d's label stripe on screen" % track)
        top = ys.min()
        run = ys[(ys >= top) & (ys < top + 60) & (xs == xs[ys == top][0])]
        rows[track] = (int(xs[ys == top][0]), int(run.min() + run.max()) // 2)
    return rows


def find_button(image, colour, tolerance=8):
    """Centre of the lowest solid block of the colour at least 40 px wide."""
    import numpy
    pixels = numpy.asarray(image).astype(int)
    mask = (numpy.abs(pixels - numpy.array(colour)) <= tolerance).all(axis=2)
    counts = mask.sum(axis=1)
    rows = numpy.nonzero(counts >= 40)[0]
    if len(rows) == 0:
        return None
    bottom = rows.max()
    block = [y for y in rows if y > bottom - 40]
    top = bottom
    while top - 1 in block:
        top -= 1
    xs = numpy.nonzero(mask[(top + bottom) // 2])[0]
    return int((xs.min() + xs.max()) // 2), int((top + bottom) // 2)


class Test:
    def __init__(self, binary, work, files):
        self.binary = binary
        self.work = work
        self.files = files
        self.desktop = Desktop()
        self.failures = []
        self.source = None

    def result(self, name, ok, detail):
        print("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
        if not ok:
            self.failures.append(name)

    def open_source(self, paths):
        self.close_source()
        log = os.path.join(self.work, "source.log")
        self.source = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "source", *paths],
                                       stdout=open(log, "w"), stderr=subprocess.STDOUT)
        if not wait_for(lambda: "MAPPED" in read_lines(log), 20):
            raise Failure("the drag source did not map")
        time.sleep(1.0)
        self.desktop.tap(KEY_LEFT, KEY_SUPER)
        time.sleep(1.0)
        self.source_log = log

    def close_source(self):
        if self.source is not None:
            self.source.terminate()
            self.source.wait(timeout=10)
            self.source = None
            time.sleep(0.5)

    def drop(self, paths, target):
        self.open_source(paths)
        drags = sum(1 for l in read_lines(self.source_log) if l == "DRAG end")
        self.desktop.drag((MONITOR[0] // 4, MONITOR[1] // 2), target)
        if not wait_for(lambda: sum(1 for l in read_lines(self.source_log) if l == "DRAG end") > drags, 10):
            raise Failure("the drag from the source never ended")

    def wait_for_button(self, app, colour, what):
        found = wait_for(lambda: find_button(app.grab("picker")[0], colour), 10, step=0.5)
        if not found:
            raise Failure("the drop did not open the %s (DUSK_REGRESS_SCENARIO_KEEP=1 keeps picker.png)" % what)
        _, rect = app.window()
        print("  %s: button at %d,%d in the window at %s" % (what, found[0], found[1], rect), flush=True)
        return rect[0] + found[0], rect[1] + found[1]

    def wait_for_state(self, app, predicate, seconds=30):
        def landed():
            state = app.autosave()
            return state if state is not None and predicate(state) else None
        return wait_for(landed, seconds, 1.0)

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

        app = App(self.binary, self.work, session_file)
        try:
            time.sleep(2.0)
            self.desktop.tap(KEY_RIGHT, KEY_SUPER)
            time.sleep(2.0)
            image, rect = app.grab("timeline")
            rows = find_rows(image)
            print("  window %s, label stripes %s" % (rect, rows), flush=True)
            x = rect[0] + rect[2] * 2 // 3

            def row(track):
                return x, rect[1] + rows[track][1]

            one = os.path.join(self.files, "one", "solo tone.wav")
            self.drop([one], row(2))
            self.desktop.click(*self.wait_for_button(app, SINGLE_IMPORT, "import target picker"))
            state = self.wait_for_state(app, lambda s: bool(s["tracks"][1].get("regions")))
            self.result("one WAV on track 2", state is not None,
                        "track 2 holds %s" % (state["tracks"][1].get("regions") if state else "nothing"))

            midi = os.path.join(self.files, "midi", "melody.mid")
            self.drop([midi], row(3))
            self.desktop.click(*self.wait_for_button(app, SINGLE_IMPORT, "import target picker"))
            time.sleep(1.0)
            self.desktop.tap(KEY_RETURN)
            state = self.wait_for_state(app, lambda s: bool(s["tracks"][2].get("midi_regions")))
            notes = len(state["tracks"][2]["midi_regions"][0].get("notes", [])) if state else 0
            self.result("MIDI file on track 3", state is not None and state["tracks"][2].get("track_mode") == 2
                        and notes == 4, "track 3 mode %s with %d notes" % (
                            state["tracks"][2].get("track_mode") if state else "?", notes))

            # Auto-assign may put a file on a track that already has a name, so the
            # files are counted by their regions, not by the names they give tracks.
            before = audio_regions(app.autosave())
            many = [os.path.join(self.files, "many", n) for n in ("bass.wav", "drums.wav", "keys.wav")]
            self.drop(many, row(1))
            import_at = self.wait_for_button(app, MULTI_IMPORT_DISABLED, "multi-import target picker")
            image, rect = app.grab("multi")
            auto = find_button(image, MULTI_AUTO_ASSIGN)
            if auto is None:
                raise Failure("no Auto-assign button in the multi-import picker (DUSK_REGRESS_SCENARIO_KEEP=1 keeps multi.png)")
            self.desktop.click(rect[0] + auto[0], rect[1] + auto[1])
            time.sleep(0.5)
            self.desktop.click(*import_at)
            state = self.wait_for_state(app, lambda s: len(audio_regions(s) - before) == 3)
            landed = sorted(track for track, _ in audio_regions(state) - before) if state else []
            self.result("three WAVs at once", state is not None, "new regions on tracks %s" % (landed or "none"))
        finally:
            self.close_source()
            app.stop()


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "source":
        run_source(sys.argv[2:])
        return 0
    if len(sys.argv) != 5 or sys.argv[1] != "drive":
        sys.exit(__doc__)
    test = Test(*sys.argv[2:5])
    try:
        test.run()
    except Failure as failure:
        test.result("drop", False, str(failure))
    except Exception as error:  # a crash in the driver is a failed leg, not a hang
        test.result("driver", False, repr(error))
    return 1 if test.failures else 0


if __name__ == "__main__":
    sys.exit(main())
