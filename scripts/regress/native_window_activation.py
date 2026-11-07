"""Actual-app activation and keyboard delivery in a private Wayland-only Mutter."""

import contextlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gnome_file_drop import Desktop, Failure, wait_for


def text(path):
    return path.read_text(errors="replace") if path.exists() else ""


def last_active(path):
    values = re.findall(r"^ACTIVE ([01])$", text(path), re.MULTILINE)
    return values[-1] if values else None


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def cover(binary, token_file):
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gio, Gtk

    def activate(app):
        window = Gtk.ApplicationWindow(application=app, title="Dusk native launcher")
        window.set_decorated(False)
        window.set_child(Gtk.Label(label="Launch Dusk Studio"))
        window.connect("notify::is-active", lambda w, _p: print("ACTIVE %d" % w.is_active(), flush=True))
        click = Gtk.GestureClick()

        def pressed(*_args):
            info = Gio.AppInfo.create_from_commandline(shlex.quote(binary), "Dusk Studio", Gio.AppInfoCreateFlags.NONE)
            context = window.get_display().get_app_launch_context()
            token = context.get_startup_notify_id(info, [])
            if not token:
                raise Failure("GTK did not obtain an activation token")
            token_file.write_text(token)

        click.connect("pressed", pressed)
        window.add_controller(click)
        window.maximize()
        window.present()
        # present() returns before Mutter shows the window, so MAPPED waits for
        # the surface to enter the monitor.
        window.get_surface().connect("enter-monitor", lambda *_args: print("MAPPED", flush=True))

    app = Gtk.Application(application_id="audio.dusk.studio.NativeLauncher",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)
    app.connect("activate", activate)
    app.run([sys.argv[0]])


def markers(path):
    try:
        return json.loads(path.read_text()).get("markers", [])
    except (OSError, ValueError):
        return []


def drive(binary, work):
    from gi.repository import Gio
    if os.environ.get("DISPLAY"):
        raise Failure("native activation must run without an X11 display")
    desktop = Desktop()
    desktop.tap(0xFFE1)
    config, music = work / "config", work / "music"
    config.mkdir()
    music.mkdir()
    (config / "app-config.properties").write_text("scan_plugins_on_startup=0\nautosave_interval_sec=10\n")
    sessions = []
    for name in "abcd":
        folder = work / name
        folder.mkdir()
        session = folder / ("native-" + name + ".json")
        session.write_text('{"version":6}\n')
        (folder / "session.json").write_text(session.read_text())
        sessions.append(session)
    env = dict(os.environ, GDK_BACKEND="wayland", GTK_A11Y="none", WAYLAND_DEBUG="client",
               DUSKSTUDIO_NATIVE_WAYLAND="1",
               DUSKSTUDIO_CONFIG_DIR=str(config), DUSKSTUDIO_MUSIC_DIR=str(music))
    app_log = work / "app.err"
    app = None

    def focused():
        wire = text(app_log)
        surfaces = re.findall(r"get_xdg_surface\(new id xdg_surface[#@]\d+, wl_surface[#@](\d+)\)", wire)
        events = re.findall(r"wl_keyboard[#@]\d+\.(enter|leave)\(\d+, wl_surface[#@](\d+)", wire)
        return bool(surfaces and events and events[-1] == ("enter", surfaces[0]))

    def loaded(session):
        return "[Dusk Studio/Load] " + session.name + ":" in text(app_log)

    def require_event(predicate, reason, seconds=20):
        def check():
            if app is not None and app.poll() is not None:
                raise Failure("app exited while waiting for " + reason)
            return predicate()
        if not wait_for(check, seconds, step=0.05):
            raise Failure(reason)

    with contextlib.ExitStack() as resources:
        for index, case in enumerate(("launch", "covered", "minimized", "handoff")):
            if case == "minimized":
                if "<Super>h" not in Gio.Settings.new("org.gnome.desktop.wm.keybindings").get_strv("minimize"):
                    raise Failure("private Mutter minimize shortcut differs from Super+h")
                desktop.tap(ord("h"), 0xFFEB)
                require_event(lambda: not focused(), "minimize did not remove app focus")
            if case == "handoff":
                desktop.tap(ord("m"))
                desktop.tap(0xFF1B)
                autosave = sessions[2].parent / "session.json.autosave"
                require_event(lambda: len(markers(autosave)) == 2, "unsaved marker did not reach autosave", 30)

            token_file = work / (case + ".token")
            cover_log = work / (case + ".cover")
            launcher = subprocess.Popen([sys.executable, "-I", __file__, "cover", binary, str(token_file)],
                env=env, stdout=resources.enter_context(cover_log.open("w")),
                stderr=resources.enter_context((work / (case + ".cover.err")).open("w")))
            resources.callback(stop, launcher)
            require_event(lambda: "MAPPED" in text(cover_log), "launcher did not map")
            # Mutter scales a new window up from its centre, so for its first frames
            # a corner can still be the app's. Only the centre is sure to be the cover.
            desktop.click(960, 540)
            require_event(lambda: last_active(cover_log) == "1" and not focused(), "cover did not take app focus")
            token_file.unlink(missing_ok=True)
            desktop.click(960, 540)
            require_event(token_file.exists, "launcher token missing")
            launch_env = dict(env, XDG_ACTIVATION_TOKEN=token_file.read_text())
            if app is None:
                app = subprocess.Popen([binary, str(sessions[index])], env=launch_env,
                    stdout=resources.enter_context((work / "app.out").open("w")),
                    stderr=resources.enter_context(app_log.open("w")))
                resources.callback(stop, app)
            else:
                with (work / (case + ".handoff")).open("w") as output:
                    result = subprocess.run([binary, str(sessions[index])], env=launch_env,
                                            stdout=output, stderr=output, timeout=60)
                if result.returncode != 0:
                    raise Failure(case + " second launch failed")
            require_event(lambda: focused() and last_active(cover_log) == "0", case + " did not transfer native focus", 60)
            if case == "handoff":
                time.sleep(2)
                if loaded(sessions[index]):
                    raise Failure("handoff bypassed the unsaved-changes prompt")
                desktop.tap(0xFF09)
                desktop.tap(0xFF09)
                desktop.tap(0xFF0D)
            require_event(lambda: loaded(sessions[index]), case + " session did not load", 60)
            if case != "handoff":
                desktop.tap(ord("m"))
                desktop.tap(0xFF1B)
                desktop.tap(ord("s"), 0xFFE3)
                require_event(lambda: len(markers(sessions[index].parent / "session.json")) == 1,
                              case + " did not deliver edit/save keys")
            elif len(markers(sessions[2].parent / "session.json")) != 1:
                raise Failure("Don't Save wrote the unsaved marker")
            print("PASS native %s: focus, keyboard delivery and session handoff, pid %d" % (case, app.pid), flush=True)
            stop(launcher)

        wire = text(app_log)
        if len(re.findall(r"\.get_toplevel\(new id xdg_toplevel[#@]\d+\)", wire)) != 1:
            raise Failure("app did not retain one native toplevel")
        if len(re.findall(r"xdg_activation_v1[#@]\d+\.activate\(", wire)) != 4:
            raise Failure("each launch did not consume one native activation token")
        desktop.tap(ord("q"), 0xFFE3)
        if app.wait(timeout=10) != 0:
            raise Failure("app failed to shut down")


if __name__ == "__main__":
    try:
        if len(sys.argv) == 4 and sys.argv[1] == "cover":
            cover(sys.argv[2], Path(sys.argv[3]))
        elif len(sys.argv) == 4 and sys.argv[1] == "drive":
            drive(sys.argv[2], Path(sys.argv[3]))
        else:
            raise Failure("usage: native_window_activation.py drive <app> <work>")
    except (Failure, OSError, subprocess.TimeoutExpired) as error:
        print("FAIL native activation: " + str(error), flush=True)
        sys.exit(1)
