# macOS and Windows window-activation smoke test (manual)

Run this checklist against the packaged application on both macOS and Windows
before a release. It covers behavior that CI can compile and source-check but
cannot reliably assert without controlling the foreground desktop session.

Use two disposable sessions, with a harmless unsaved edit in the first one for
the handoff case. On Windows, foreground activation is deliberately subject to
the operating system's focus policy: when Windows denies focus, a restored
window plus a flashing taskbar button is the expected result. The implementation
must not force focus by joining another process's input queue.

| Case | Action | macOS result | Windows result |
|---|---|---|---|
| Initial launch | Put another app in front, then launch Dusk Studio from Finder / Explorer. | Dusk Studio becomes active and its main window is key and frontmost. | The main window is visible and foreground when policy permits; otherwise its taskbar button flashes. |
| Session open | Open the second session from Dusk Studio while another window partly occludes the main window. | The rebuilt main window is key and frontmost; any load alert stays above it. | The rebuilt main window is foreground when permitted; any load alert remains visible. |
| Minimized | Minimize Dusk Studio, then open a session through the OS file association. | The existing window leaves the Dock and comes to the front. | The existing window is restored; it receives focus or its taskbar button flashes. |
| Second-process handoff | With an unsaved edit open, double-click the second session so the running instance receives the request. | The existing window and save prompt come to the front; no second main window remains. | The existing window is restored and the save prompt is visible; focus is granted or the taskbar flashes, and no second main window remains. |

Second-process enforcement on macOS and Windows is implemented separately by
issue #368. Run the handoff rows after that milestone item has landed; keep this
checklist as the release regression check for both changes.
