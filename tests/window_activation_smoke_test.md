# macOS and Windows window-activation smoke test

Four cases decide whether Dusk Studio comes to the front when it should on
macOS and Windows: the initial launch, a session opened from inside the app,
a session opened from outside while the window is minimized, and a session
opened from outside over an unsaved edit. CI can compile and source-check this
code but cannot own a foreground desktop session, so the cases run as legs of
the regression runner against an installed package, and a person covers what
the legs cannot.

## How a session reaches a running instance

The installed 0.16.0 packages register no file association for `session.json`
on macOS (the app's `Info.plist` declares no `CFBundleDocumentTypes`) or on
Windows (the MSI registers none). Only Linux installs one, as MANUAL.md says
under Install and first launch. So "opening a session from outside" takes a
different route on each platform:

- **macOS**: LaunchServices. **Open With**, a drop on the Dock icon and
  `open -a DuskStudio <session.json>` all hand the document to the running
  instance as an Apple event. No second process starts.
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
docs/MAINTAINER-GUIDE.md, Part 9b.

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

## What still needs a person

Run these by hand on both platforms before a release, with two disposable
sessions and a harmless unsaved edit in the first one for the last row.

| Check | macOS | Windows |
|---|---|---|
| The literal gestures | Finder's **Open With > Dusk Studio** on a `session.json`, and a `session.json` dropped on the Dock icon, each with Dusk Studio running and minimized: the window comes out of the Dock, frontmost, and an unsaved edit raises the save prompt above it. | Explorer's **Open with** on a `session.json`, choosing `DuskStudio.exe`, with Dusk Studio running and minimized: the window is restored and comes forward or its taskbar button flashes, no second window remains, and an unsaved edit raises the save prompt. |
| A launch from the desktop | Dusk Studio started from Finder or Launchpad with another application in front comes up frontmost and takes typing at once. | Dusk Studio started from the Start menu shortcut with another application in front comes up in front or flashes its taskbar button. |
| Another machine | A Retina display: the leg reads captures on the Air's 1x desktop only. | A physical machine with a person's real foreground history; the guest only ever has Notepad and File Explorer in front. |

A command-line second launch on macOS (`DuskStudio.app/Contents/MacOS/DuskStudio
<session.json>` while the app runs) goes through the single-instance handoff
rather than LaunchServices. No Finder route starts one, and the macOS leg does
not test it.
