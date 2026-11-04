# Window-activation smoke test

Four cases decide whether Dusk Studio comes to the front when it should on
macOS and Windows: the initial launch, a session opened from inside the app,
a session opened from outside while the window is minimized, and a session
opened from outside over an unsaved edit. CI can compile and source-check this
code but cannot own a foreground desktop session, so the cases run as legs of
the regression runner against an installed package, and a person covers what
the legs cannot. Linux runs the same idea under GNOME's window manager in a
private headless session, as part of every `scripts/regress.sh linux` run, with
a second launch over another window in place of the open from inside.

## How a session reaches a running instance

The installed 0.16.0 packages register no file association for `session.json`
on macOS (the app's `Info.plist` declares no `CFBundleDocumentTypes`) or on
Windows (the MSI registers none). Only Linux installs one, as MANUAL.md says
under Install and first launch. So "opening a session from outside" takes a
different route on each platform:

- **macOS**: LaunchServices. **Open With**, a drop on the Dock icon and
  `open -a DuskStudio <session.json>` all hand the document to the running
  instance as an Apple event. No second process starts.
- **Linux**: opening a `session.json` from the file manager runs
  `DuskStudio <session.json>`. That second process finds the running instance
  through the single-instance handoff, passes the path over and exits 0, and
  the existing window must come forward.
- **Windows**: Explorer's **Open with** runs `DuskStudio.exe "<session.json>"`.
  That second process finds the running instance through the single-instance
  handoff, passes the path over and exits 0, and the existing window must come
  forward. Windows' foreground rules can refuse it the foreground; then the
  restored window with its taskbar button flashing is the accepted result, for
  the initial launch and for a handoff alike. The app must never force focus,
  for instance by attaching to another process's input queue.

## Automated legs

```bash
TMPDIR=/var/tmp scripts/regress.sh mac --window-activation-only
TMPDIR=/var/tmp scripts/regress.sh windows --installed X.Y.Z
```

The first tests the app installed in `/Applications` on the M3 Air, the second
the MSI installed in the `win11` guest, which must be version X.Y.Z. Package
mode (`mac --dmg` / `mac --release-run`, `windows --msi` /
`windows --release-run`) runs the same cases against the package it installs.
Prerequisites, durations and the evidence each run leaves are in
docs/MAINTAINER-GUIDE.md, Part 9b. The Linux cases are scenario legs, so
`TMPDIR=/var/tmp scripts/regress.sh linux` runs them against `build/`, and
`--tarball` / `--release-run` against the package it unpacks.

### macOS (`scripts/regress/mac-window-activation.sh`)

A probe built on the node reads the frontmost application and the window list
from the window server. Reading the key window needs a permission the node
does not grant, so "key and frontmost" is judged three ways at once: Dusk
Studio is the frontmost application, its one main window is the topmost normal
window, and a Screen Sharing capture shows that window's title-bar buttons in
colour, which AppKit draws only on the key window. Every case also requires a
single Dusk Studio process. Calculator is the other application put in front.
The in-window alert and save prompt are found in captures.

| Case | Action | Pass |
|---|---|---|
| `window-activation:launch` | Calculator in front, then the app started through LaunchServices (`open`) with session A named in its environment. | Frontmost and key as above; session A is the open session. |
| `window-activation:session-open` | Cmd+O, the path of session B typed into the browser, then Calculator brought in front over part of the main window and the browser's Open button clicked. Session B's mastering mix is missing, so its load raises an alert. | Session B loaded; frontmost and key; the alert on top of the main window. Two controls make the checks prove something: with Calculator active the title-bar buttons must read grey, and once Escape has dismissed the alert the panel check must find nothing. |
| `window-activation:minimized` | The main window minimized from its yellow button, Calculator in front, then `open -a` with session C. | The same window, by window id, back on screen; frontmost and key; session C loaded. |
| `window-activation:handoff` | M drops a marker (an unsaved edit), Calculator in front, then `open -a` with session D. | Frontmost and key with the save prompt on top and session D held back; Don't Save, clicked where the prompt draws it, then opens session D; still one process and the same single main window. |

### Windows (`scripts/regress/windows/phase4-window-activation.ps1`)

A probe pumps messages for the whole phase and records, on one clock, the
shell hook's taskbar flashes (`HSHELL_FLASH`), every foreground change and
every `DuskStudio.exe` start and exit with its parent process and exit code.
UI Automation finds the embedded prompts and alerts by their titles, and one
counts as visible only when it is on screen and the pixel at its centre
belongs to the main window. Input is `SendInput`. Notepad is the other window
put in front. "From Explorer" means a shortcut opened through a File Explorer
window's own `ShellExecute`, so the process is a child of `explorer.exe`, as
it is from **Open with**. Each case starts a fresh instance, and each accepts
foreground or a taskbar flash; the leg's notes say which one each case got.

| Case | Action | Pass |
|---|---|---|
| `initial-launch` | Notepad in front, then a shortcut to `DuskStudio.exe` opened from Explorer. The private config and music folders are lent to Explorer through `HKCU\Environment` for that one launch and cleared after it. | Foreground or taskbar flash; the window shown, not minimized; exactly one main window; started by `explorer.exe`; the private config folder used. |
| `session-open` | Session A loaded, its window clicked to the front, Notepad kept on top over part of it, then Ctrl+O and the path of a session with missing audio typed into the browser. | Foreground or taskbar flash; shown, not minimized; exactly one main window; the `Missing audio files` alert visible. |
| `minimized` | Session A loaded, the window minimized, Notepad in front, then a shortcut running `DuskStudio.exe "wa-b.json"` opened from Explorer. | Foreground or taskbar flash; the window restored; exactly one main window and one `DuskStudio.exe`; the second process a child of `explorer.exe` that exited 0; `wa-b.json` loaded. |
| `handoff` | Session A loaded, M drops a marker (an unsaved edit), Notepad in front over part of the window, then the same shortcut opened from Explorer. | Foreground or taskbar flash; shown; exactly one main window and one process; the second process from `explorer.exe` exited 0; the save prompt visible with Save, Don't Save and Cancel; `wa-b.json` not loaded behind it. |

Each case leaves a screenshot, `shot-phase4-<case>.png`, in the run
directory, and the probe's record goes to `posts/phase4-window-activation-events.log`
there.

### Linux (`scripts/regress/gnome-window-activation.sh`)

The leg starts a headless Mutter with Xwayland on a private session bus, from
an empty environment with its own `HOME`, XDG directories and runtime dir, and
runs `scripts/regress/gnome_window_activation.py` inside it. The app takes the
production path, an Xwayland client whose window Mutter manages. The other
window is a GTK 4 window on Wayland, as most GNOME applications are, maximized
over the whole monitor; it reports its keyboard focus and every pointer event
it gets. Input goes through `org.gnome.Mutter.RemoteDesktop` on the private
bus. One app instance runs the four cases in turn.

The app is in front when all of these hold: Mutter's `_NET_ACTIVE_WINDOW` and
the X input focus are its window, the only window of the app Mutter manages;
the window is not hidden and its `WM_STATE` is Normal; the GTK window says it
lost the keyboard; and the pointer held at the centre of the app's window
reaches the app and not the GTK window. Before each handoff the same checks
must show the GTK window in front, so a check that cannot fail does not pass a
case. Every case also requires a single Dusk Studio process and the second
launch to exit 0.

| Case | Action | Pass |
|---|---|---|
| `window-activation:launch` | The GTK window in front, then the app started with session A named in its environment. | In front as above; session A loaded. |
| `window-activation:covered` | The GTK window opened over the whole monitor, then `DuskStudio wa-b.json` run as a second process. | The second process exited 0; session B loaded; in front. |
| `window-activation:minimized` | The main window minimized (`WM_CHANGE_STATE` Iconic), the GTK window in front, then `DuskStudio wa-c.json`. | The same window back from minimized; session C loaded; in front. |
| `window-activation:handoff` | M drops a marker (an unsaved edit, read back from the autosave), the GTK window in front, then `DuskStudio wa-d.json`. | In front with the save prompt in a grab of the window and session D held back; Don't Save, clicked where the prompt draws it, opens session D, and the prompt is gone. |

Each case leaves a grab of the app window, `<case>.png`. A failed run keeps its
scratch directory under `$TMPDIR` and names it; the grabs, the app's output and
the GTK window's event log are there.

What each case can catch: Mutter honours an activation request without a
timestamp, which JUCE's own `toFront` sends, as well as the pager-sourced
request the app adds, so either one brings the window forward. Every session
load ends with a raise of its own, so `covered` and `minimized` pass even when
the handoff skips its raise; `handoff`, where the load waits behind the save
prompt, is the case that fails then. With the handoff's raise gone the X input
focus still reached the app, through a focus call made after the handoff,
while the window stayed under the GTK window, and only the pointer check saw
it. A new window gets the keyboard under Mutter's own policy, so `launch`
passes whatever the app does. The leg covers the default Xwayland path; the
native Wayland backend (`DUSKSTUDIO_NATIVE_WAYLAND=1`) is not tested.

## What still needs a person

Run these by hand on all three platforms before a release, with two
disposable sessions and a harmless unsaved edit in the first one for the last
row.

| Check | macOS | Windows | Linux |
|---|---|---|---|
| The literal gestures | Finder's **Open With > Dusk Studio** on a `session.json`, and a `session.json` dropped on the Dock icon, each with Dusk Studio running and minimized: the window comes out of the Dock, frontmost, and an unsaved edit raises the save prompt above it. | Explorer's **Open with** on a `session.json`, choosing `DuskStudio.exe`, with Dusk Studio running and minimized: the window is restored and comes forward or its taskbar button flashes, no second window remains, and an unsaved edit raises the save prompt. | A double-click on a `session.json` in GNOME Files, with Dusk Studio running and minimized: the window comes forward with the keyboard, no second window remains, and an unsaved edit raises the save prompt. |
| A launch from the desktop | Dusk Studio started from Finder or Launchpad with another application in front comes up frontmost and takes typing at once. | Dusk Studio started from the Start menu shortcut with another application in front comes up in front or flashes its taskbar button. | Dusk Studio started from the GNOME application grid with another application in front comes up in front and takes typing at once. |
| Another machine | A Retina display: the leg reads captures on the Air's 1x desktop only. | A physical machine with a person's real foreground history; the guest only ever has Notepad and File Explorer in front. | A full GNOME Shell desktop and a KDE Plasma one: the leg runs bare Mutter, with no shell and no other window manager. |

A command-line second launch on macOS (`DuskStudio.app/Contents/MacOS/DuskStudio
<session.json>` while the app runs) goes through the single-instance handoff
rather than LaunchServices. No Finder route starts one, and the macOS leg does
not test it.
