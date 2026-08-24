# De-JUCE — GUI tower (campaign plan)

The last tower. It removes `src/ui/` as a JUCE surface and, with it, every
remaining JUCE module. This document is the execution spec: the gate evidence,
the framework revision to build on, the backend gaps that have to be closed, the
phase order, and what each phase must prove before it lands.

Gate issue: #301. Campaign map: [dejuce-campaign.md](dejuce-campaign.md).

## Recommendation: GO

The locked direction holds. A native Wayland application window driven by the
Dusk Audio Framework's DGL/pugl stack, rendering a real Dusk Studio channel
strip in Dear ImGui, runs at a locked 60 Hz with p50 16.5 ms and a worst frame
of 19.9 ms, costs 4.7% of one core for one strip, survives repeated window
teardown and recreation without losing its renderer, and reproduces the current
look closely enough that the theme port reads as a port rather than a redesign.
A full bank of eight strips holds 60 Hz at 18.3% of a core, and all twenty-four
console strips hold it at 45.6%. Nothing in the spike argued for a different
framework, and the two defects it did surface (window focus is never delivered
to the ImGui layer, and window decorations are absent under GNOME) are bounded
work inside a framework the project already owns, not reasons to change course.

The GO is conditional on three things being treated as tower work rather than
discovered late: the focus/decoration gaps in §4, the frame-cost ceiling in §2,
and the accessibility bridge in §4. None of them is a research problem.

## 1. Framework revision reconciliation

### 1.1 Where the two lines stand

The app pins `DAF_REV=f9fbc62a` (`.github/actions/clone-dpf-stack/action.yml`),
which is the tip of the framework branch `fix/wayland-review-findings`. That
commit is **not** an ancestor of the framework's `main`, which is the line the
Dusk plug-ins follow. Both descend from `a9b033c2`.

| | Commits since `a9b033c2` | Carries |
|---|---|---|
| Pinned branch `f9fbc62a` | 3 | `Window::setEmbeddedOffset`, the Wayland lifecycle and scaling fixes, the pugl mirror pointer |
| `main` | 31 | The DPF to DAF rename, a second round of Wayland lifetime fixes, AU and CLAP fixes, CI hardening, the pugl mirror pointer |

The pugl submodule is pinned to `5e2621d7` on **both** sides, so the Wayland
backend's own history is not divergent. All divergence is in the framework
repository itself.

**Nothing on the pinned branch is superseded by main, and nothing on main is
superseded by the branch.** They are two independent rounds of work on the same
files:

- `213336ad` (branch, "point the pugl submodule at the dusk-audio mirror") has an
  exact equivalent on main in `d8e5bc69` ("take pugl from our own fork"). The
  resulting `.gitmodules` is byte-identical. Nothing to carry.
- `f9fbc62a` (`Window::setEmbeddedOffset`) is **absent from main**. The notepad
  refuses to open without it, so main cannot be adopted as-is.
- `7b33d9f6` ("Fix Wayland lifecycle and scaling issues") is **absent from
  main**. Main's `36a7347a` is a different fix set on the same file (unclaimed
  clipboard offers, per-view timer purge on destroy, sub-`BTN_LEFT` button
  codes, 64-bit shm pool sizing, `_GNU_SOURCE`), not a rework of it.

There is exactly one place where the two sides made **contradictory** decisions:
`getDesktopScaleFactor()` on Wayland. Main returns `1.0` with a comment arguing
it is harmless; the branch returns the `0.0` "unknown" sentinel. The branch is
the later decision and it is the correct one — `Window::PrivateData` derives
`followsPuglScaleFactor` from `d_isZero(scale)`, so returning `1.0` pins the
window to scale 1 for its whole life and the matching `onPuglConfigure` hook
that propagates a live scale change never runs. Main is missing both halves of
that mechanism.

### 1.2 The proposed reconciled revision

Prepared and validated locally; the push and merge are Marc's. Three commits on
top of `main`:

1. Cherry-pick `f9fbc62a` (`setEmbeddedOffset`). Applies cleanly except for the
   two `DISTRHO_SAFE_ASSERT_RETURN` uses, which become `DAF_SAFE_ASSERT_RETURN`.
2. Cherry-pick `7b33d9f6` (Wayland lifecycle and scaling). Five conflict hunks,
   all mechanical once the intent is clear:
   - `wayland.c`, timer removal: both sides added a `puglWaylandRemoveViewTimers`.
     Take the branch's pair (`puglWaylandRemoveTimer` + the view sweep), because
     the helper is also used by the branch's `puglStopTimer`, and wrap it in the
     `PUGL_WAYLAND_HAVE_TIMERFD` guard main added. Drop the branch's duplicate
     unguarded call in `puglFreeViewInternals`.
   - `wayland.c`, timer dispatch: take the branch side, which adds
     `impl->timers[i].fd != timer.fd` to the staleness check. Strict superset.
   - `WindowPrivateData.cpp`: take the branch's `HAVE_WAYLAND` scale-follow block
     in `onPuglConfigure`, keeping main's `DAF_TEST_WINDOW_CPP` spelling.
   - `DafUI.cpp` and `README.wayland`: take the branch's `0.0` sentinel and its
     prose, under DAF naming. This is the one semantic call in the set; §1.1
     explains why.
3. A new commit adding `DGL_BACKEND` (§1.4).

Validated on this box: `dusk_notepad_ui` and the whole `DuskStudio` target build
clean against the result, and the spike runs on it. The prepared branch is
`dusk/301-reconcile` in the framework clone at `/home/marc/projects/DAF`; it has
never been pushed, so it has to be recreated or transplanted before the PR.

### 1.3 What the app has to change to consume `main`

The rename is not cosmetic at the build interface. Three things move:

| Was | Is | Where it bites |
|---|---|---|
| `DPF_LIBRARIES` / `DPF_EXAMPLES` | `DAF_LIBRARIES` / `DAF_EXAMPLES` | root `CMakeLists.txt` |
| `dpf__add_dgl_opengl3()` | `daf__add_dgl_opengl3()` | root `CMakeLists.txt` |
| `namespace dpf_resources` | `namespace daf_resources` | `src/ui/NativeNotepadWindow.cpp` |

Commit `39d19a6` on this branch makes all three accept either spelling, so the
tree configures against the current pin and against the reconciled revision.
When the pin moves, that dual handling collapses to the DAF spelling and the
`DUSKSTUDIO_DGL_RESOURCES` macro goes away; it exists only to span the two live
revisions.

### 1.4 The backend is chosen by accident today

`daf__add_dgl_system_libs` selects X11 whenever the X11 development files are
installed, and Wayland only when they are absent. There is no way to ask for
Wayland. On a dual-stack developer box or CI image the failure is silent: the
build succeeds and the app runs, just against XWayland — which is precisely what
the zero-XWayland policy for Dusk-owned surfaces forbids.

Proposed commit 3 adds `DGL_BACKEND` with values `auto` (the old behaviour, and
still the default), `x11` and `wayland`, and fails the configure when the
requested backend's libraries are missing. Linux app and CI configures then pass
`-DDGL_BACKEND=wayland` explicitly.

Note the consequence for build layout: **the backend is a per-configure choice**,
because one `dgl-opengl3` target serves every consumer in the tree. A build
directory is X11 or Wayland, not both. The spike therefore lives in its own
build directory rather than beside the app.

### 1.5 Proposal to Marc

- Merge the three commits above into `main` of `dusk-audio/DAF`, as one reviewed
  PR.
- Re-pin `DAF_REV` in `.github/actions/clone-dpf-stack/action.yml` to the merge
  commit; that action is the single source of truth for every workflow, and it
  already clones from `https://github.com/dusk-audio/DAF.git`.
- Re-pin `DAF_WIDGETS_REV` to `main` of `dusk-audio/DAF-Widgets`, which is two
  commits ahead of the current pin and carries the matching rename. There is no
  divergence there.
- Delete the branch `fix/wayland-review-findings` once merged, so no third line
  can accumulate.
- Retire the dual-spelling handling in the app's `CMakeLists.txt` and
  `NativeNotepadWindow.cpp` in the same PR that moves the pin.

## 2. What the gate spike measured

Source: `tools/gui-spike/`. Off by default behind `DUSKSTUDIO_BUILD_GUI_SPIKE`.
Zero JUCE, links no part of the app.

```
cmake -S . -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDUSKSTUDIO_BUILD_GUI_SPIKE=ON -DDGL_BACKEND=wayland \
      -DJUCE_PATH=... -DDUSK_PLUGINS_PATH=... -DDAF_PATH=... -DDAF_WIDGETS_PATH=...
cmake --build build-spike --target dusk-gui-spike -j6

tools/gui-spike/headless-compositor.sh &
XDG_RUNTIME_DIR=/tmp/dusk-headless WAYLAND_DISPLAY=dusk-headless \
  env -u DISPLAY ./build-spike/gui-spike/dusk-gui-spike --seconds 10 --strips 24
```

`headless-compositor.sh` starts a `mutter` with its own runtime directory, dbus
session and display name. Nothing Dusk-owned goes on the live session's socket:
a crash in one of these surfaces takes the desktop with it.

It draws the strip from `ChannelStripComponent::resized()`'s real geometry, with
the colours, fonts, knob dome, fader cap and segmented meter ported from
`DuskStudioLookAndFeel`, against stub parameters at the shipping ranges and
meters fed by a 30 Hz source thread with the UI-side ballistics the shipping
timer callback applies.

### 2.1 Environment

Every number below is from a **private headless `mutter` 48.4** session
(`--headless --no-x11 --virtual-monitor 1920x1080`, its own `XDG_RUNTIME_DIR`,
its own dbus session, its own Wayland display name), never the live desktop.
GL is **hardware**, not llvmpipe: `AMD Radeon RX Vega M GH (radeonsi, vegam,
Mesa 24.3.3)`, GL 4.6 compatibility profile.

Headless is not the same as the desktop. It has no seat, so no real pointer or
keyboard events arrive; it does not run the desktop's compositing or effects;
and its idea of output scale is fixed. **The live-desktop pass is Marc's bench
and is owed** (§6).

### 2.2 Frame pacing and cost

Ten second runs, present-to-present measured at the top of each ImGui frame.
The frame rate is capped at 60 by the framework's idle callback, so the number
that carries information is CPU, not fps.

| Strips | fps | p50 | p95 | worst | CPU (one core) | verts/frame |
|---|---|---|---|---|---|---|
| 1 | 60.0 | 16.53 ms | 19.49 ms | 19.85 ms | 4.7% | 25,690 |
| 8 (one bank) | 59.9 | 15.91 ms | 18.76 ms | 26.16 ms | 18.3% | 205,492 |
| 24 (full console) | 59.8 | 16.90 ms | 17.66 ms | 33.78 ms | 45.6% | 606,096 |

Read the CPU column as the tower's central engineering constraint. Cost is
linear in strips because immediate mode rebuilds every vertex every frame,
whereas the JUCE strip today repaints only `inputMeterArea` on its 30 Hz tick.
**45.6% of a core for a static console is not shippable as-is** on the machines
Dusk Studio targets, and it is a headroom problem, not a frame-rate one: the
budget is being spent whether or not anything moved.

Three levers, in the order they should be tried:

1. Cache the draw list for strips whose parameters did not change and only
   rebuild the meter rectangles. ImGui supports this through a retained
   `ImDrawList` per strip; the meters are a handful of rectangles.
2. Drop the redraw rate to 30 Hz for strips that are not under the pointer.
   Meters are already a 30 Hz signal; only the control being dragged needs 60.
3. Reduce vertex count in the knob dome. It is 12 concentric circles at 32
   segments each because `ImDrawList` has no radial gradient; a small
   pre-rendered dome texture tinted per band would collapse it to four vertices.

Lever 3 alone is worth measuring first: a strip carries 21 knobs at roughly 600
vertices each, so the domes are about half of its 25,690.

### 2.3 Everything else the spike exercised

| Property | Result |
|---|---|
| Native Wayland, no X11 in the loop | Confirmed. Links `wayland-client`/`-egl`/`-cursor`; `DGL_BACKEND=wayland` reports "Wayland (requested)" |
| Resize | 19 programmatic resizes over 12 s at 8 strips: 60 fps held, worst frame 23.3 ms, no stall or crash |
| Scale | Rendering at an application scale of 2.0 reflows correctly and costs the same 5.0% |
| Renderer and context lifetime | Four full window create/destroy cycles then a measured run: renderer identical each time, no leak, no failed realize |
| Knob drag, wheel, double-click reset | Pass |
| Text entry on the strip name | Pass (`InputText`, commit on Enter) |
| Shell keyboard shortcut | Pass |
| Modal opens and dims | Pass |
| Context menu opens | Pass |
| Modal blocks the strip underneath | **Fail** — see §4.1 |

The interaction results come from `--selftest`, which injects at the ImGui event
queue. A headless compositor has no seat, so the run reports
`pointer=0 key=0 text=0` through the real path: the spike proves the widget
layer, and deliberately proves nothing about pugl's seat handling.

## 3. Framework gap audit for an application

Audited against `dgl/src/pugl-extra/wayland.c` and the DGL sources on the
reconciled revision. "Plugin UI" needs are already met; this is the delta for a
desktop application.

| # | Capability | Status | Reference |
|---|---|---|---|
| 1 | IME / text input | Partial | `wayland.c:1430`, `:2970` |
| 2 | Drag and drop | Absent (offers actively declined) | `wayland.c:1909` |
| 3 | Multiple top-level windows | Present | `wayland.c:186`, `wayland.h:151` |
| 4 | Transient parent / modal | Partial (parent yes, modal no) | `wayland.c:3070`, `:3813` |
| 5 | Portal parenting (xdg-foreign) | Absent | no protocol vendored |
| 6 | Keyboard focus | Partial (pugl emits, DGL drops) | `wayland.c:1555`, `:3851` |
| 7 | Clipboard | Partial (text only, no primary) | `wayland.c:4093`, `:4109` |
| 8 | Cursors | Partial (10 shapes, no hide, no custom) | `wayland.c:2519`, `:2568` |
| 9 | Accessibility | Absent (nothing in the tree) | — |
| 10 | Headless automation | Absent in the backend | `wayland.c:2911` |
| 11 | HiDPI / fractional scaling | Present | `wayland.c:312`, `:355` |
| 12 | Timers and event loop | Present, real blocking wait with timeout | `wayland.c:3454`, `:3612` |
| 13 | Window decorations | Partial, and the worst gap | `wayland.c:3193` |
| 14 | EGL context | Present, survives close/reopen | `wayland_gl.c:61`, `:146` |

Detail on the ones that change the plan:

- **Decorations (13).** Server-side only: the backend asks for
  `SERVER_SIDE` and the configure listener is a stub whose own comment says an
  answer of `CLIENT_SIDE` leaves the window undecorated. GNOME has never
  implemented `xdg-decoration`, so on Marc's own desktop the manager is NULL and
  that branch never runs. Worse, `xdg_toplevel.move`, `.resize` and
  `.show_window_menu` appear nowhere in the backend. Net result on GNOME: a
  borderless window with no titlebar, no close or maximise control, no
  drag-to-move and no resize borders. **This blocks the shell flip (G5) and
  nothing before it.** It needs either client-side decorations drawn in ImGui —
  which the portastudio aesthetic arguably wants anyway — or hit-tested edges
  wired to `xdg_toplevel.move`/`.resize`. Prefer CSD: it is one place, it is
  themeable, and it removes the compositor dependency entirely.
- **Focus (6).** pugl dispatches `PUGL_FOCUS_IN`/`OUT`, but DGL has no focus
  callback on `Widget` and never surfaces them, and the widgets library's ImGui
  bridge never calls `io.AddFocusEvent()`. §4.1 covers the consequences.
- **Cursors (8).** There is no hidden-cursor path and no custom cursor image.
  The hide-pointer-and-warp idiom that every DAW knob drag uses is therefore
  unavailable. Add a `PUGL_CURSOR_NONE` shape; it is a one-line
  `wl_pointer_set_cursor(..., NULL, ...)` in the backend.
- **Portal parenting (5).** Every file chooser opened through
  xdg-desktop-portal will be unparented, so it can appear behind the main window
  or on another workspace. Needed by G2, which is where the first file dialog
  moves.
- **Accessibility (9).** Nothing exists, in either the framework or ImGui, and
  `src/ui/` already carries JUCE accessibility labelling that would regress
  silently. This needs a deliberate decision from Marc before G3, not after:
  either an AT-SPI bridge fed from the ImGui widget kit, or a written,
  documented decision to drop screen-reader support in the native UI.
- **Headless automation (10).** The backend hard-fails without a compositor and
  has no screenshot hook. This is workable and already solved twice over: run
  under a private headless `mutter` as this gate did, and let the application
  read its own frame back (the spike's `--capture` does `glReadPixels` after the
  base `onDisplay` and before the swap). The existing Xvfb screenshot harness
  has to move to that model with the tower.

## 4. Defects the spike found

### 4.1 Window focus never reaches the ImGui layer

`DGL::Widget` has no focus callback and the widgets library's `DearImGui.cpp`
never calls `io.AddFocusEvent()`, so ImGui always believes the window is
unfocused. Three consequences, all observed:

- ImGui's modal never takes navigation focus, so its input blocking does not
  engage. The spike's modal dims the console correctly and still lets a drag
  reach a knob behind it. The `EmbeddedModal` seam depends on that blocking.
- `Escape` does not close a popup, because that routing runs through navigation.
- Key state is never cleared on focus loss, so a modifier held while switching
  windows stays down.

Fix in the framework, not the app: surface pugl's `PUGL_FOCUS_IN`/`OUT` as a DGL
widget callback and have the ImGui bridge forward it. Owner: G1.

### 4.2 `WantCaptureKeyboard` is not a usable gate

`DearImGui.cpp` unconditionally sets `ImGuiConfigFlags_NavEnableKeyboard`, which
pins `io.WantCaptureKeyboard` true once navigation is active. Combined with
§4.1 it says nothing about whether the application may take a key. A DAW's
global shortcut layer needs its own routing rule — the spike uses "no text field
open and no item active" — and the tower should settle that rule once, in the
widget kit, rather than per view.

### 4.3 The font atlas is a fixed glyph set

ImGui bakes glyphs at atlas build time. The default range is Latin, which
silently dropped the fader's infinity mark; the JUCE UI gets it free from the
system font. Every non-Latin mark the UI uses has to be declared. Track name
entry is worse: a user typing in any non-Latin script gets nothing. Decide in
G1 whether to bake a wide range, or to load glyphs on demand.

### 4.4 `ImGuiWidget<StandaloneWindow>` does not call `done()`

`StandaloneWindow` documents that `done()` must be called at the end of the
constructor to release the scoped graphics context. The widgets library's
standalone specialisation does not, so the final subclass has to. Either fix it
upstream or write it down; the spike calls `done()` itself.

## 5. Phases

`src/ui/` is **119 files carrying 7,675 of the tree's 9,055 JUCE uses** (85%),
across roughly 54,000 lines of `.cpp`. It cannot land in one PR. The
incremental vehicle is the pattern the notepad already ships: a framework child
window embedded over the JUCE main window through the native parent handle and
`Window::setEmbeddedOffset`, with the JUCE side reduced to a placeholder. That
is why `setEmbeddedOffset` must survive the reconciliation.

Each phase ends with the app shipping.

### G0 — Framework alignment

No user-visible change. Land §1: reconcile the framework revision, move
`DAF_REV` and `DAF_WIDGETS_REV`, pass `-DDGL_BACKEND=wayland` on Linux app and
CI configures, drop the dual-spelling handling.

Owns: `CMakeLists.txt`, `.github/actions/clone-dpf-stack/action.yml`, the
Linux workflows, `src/ui/NativeNotepadWindow.cpp`. Take the leftover mechanical
renames in §7 here too if they are cheap.
Gate movement: none. Verify: full build, ctest, notepad opens under Xvfb and
under headless `mutter`.

### G1 — Widget kit and theme

No user-visible change. Promote the spike's drawing layer into a real module in
DAF-Widgets that the app and the first-party plug-in UIs both consume: theme
tokens from `DuskStudioLookAndFeel`, the SSL knob, the fader, the segmented
meter, the GR strip, the module pill, buttons, the value bubble and the text
field. Settle the
shortcut routing rule (§4.2), the glyph strategy (§4.3), and the draw-list
caching from §2.2 here, before anything depends on them. Fix §4.1 and §4.4 in
the framework.

New app-side files, all JUCE-free: `src/ui/imgui/DuskImGuiHost.{h,cpp}`, the
embedded-window lifecycle lifted out of `NativeNotepadWindow`.

**The shared widget kit's home is `dusk-audio/DAF-Widgets`, not the plug-ins
repo** (Marc, 2026-08-24). The knob, fader, meter, module pill and theme tokens
go there, beside the Dear ImGui layer they build on, and both Dusk Studio and the
plug-ins consume them from that one place. The plug-ins repo's
`shared-dpf/ui/DuskImGuiWidgets.hpp` is the previous home: anything in it worth
keeping migrates into DAF-Widgets, and it is not extended in place. Do not fork a
second knob in `src/ui/`, and do not add to `shared-dpf/ui/` — a widget that only
Dusk Studio needs still belongs in DAF-Widgets if a plug-in could ever want it.

Gate movement: none. Verify: golden-image tests against captured frames, plus
`NativeNotepadWindow` rebased onto `DuskImGuiHost` as the first consumer.

### G2 — Dialogs and panels

First shipping phase. Move the self-contained modal family, one file per commit,
each becoming an embedded framework window: `AudioSettingsPanel`,
`StartupDialog`, `ChannelCompEditor`, `MasteringEqEditor`,
`MasteringLimiterEditor`, `VirtualKeyboardComponent`. Re-point the
`EmbeddedModal`, `DuskComboBox` and `showContextMenu` seams at ImGui
implementations. Needs portal parenting (§3, row 5) for the first file dialog.

Owns: roughly 12 files, ~6,000 lines. Gate: those files leave the allowlist.
Verify: each panel captured headlessly and diffed against its JUCE screenshot;
ctest; live-Wayland pass per panel.

### G3 — The console

The largest and riskiest phase, and the one the spike measured.
`ChannelStripComponent` (6,340 lines), `MasterStripComponent`, `BusComponent`,
`AuxLaneComponent`, `ConsoleView`, `AnalogVuMeter`, `CompMeterStrip`,
`SplitModuleButton` become one embedded framework window covering the console
area. Do not start it until §2.2's caching work is landed and measured at 24
strips.

Owns: roughly 15 files, ~16,000 lines. Verify: 24-strip CPU under the budget
agreed in G1; golden images per strip variant including the compact and 8-up
density tiers; the automation, take and metering paths exercised live.

### G4 — The arrangement

`TapeStrip`, `AudioRegionEditor`, `PianoRollComponent`, `TransportBar`,
`MasteringView` and their helpers. Mechanically similar to G3 but with real
scrolling, selection and drag semantics, so drag and drop (§3, row 2) is decided
here.

Owns: roughly 20 files, ~15,000 lines.

### G5 — The shell flip

`MainComponent` stops being a `juce::Component` and the top-level window becomes
a framework window. Client-side decorations (§3, row 13) land here, because this is
the first Dusk-owned top-level. Third-party plugin editors move to their own
XWayland child windows, which is already the design. `juce_gui_extra`,
`juce_gui_basics` and `juce_graphics` unlink.

Owns: `MainComponent.{h,cpp}` (6,000 lines), `DuskStudioApp.{h,cpp}`,
`ScreenshotCapture`, the screenshot harness.

### G6 — Residue

What is left once no UI is JUCE: `MessageThread.cpp` rewritten on the pugl
world's timers and event loop, `juce_data_structures`, `AudioThumbnail` off
`juce_audio_utils`, and the last `juce::String`/`File`/`Colour` holdouts in
`src/session` and `src/engine` that were anchored by UI types.

Module unlink order, each step its own commit so a bisect lands on one module:

```
juce_gui_extra -> juce_gui_basics -> juce_graphics
  -> juce_audio_utils -> juce_audio_formats
  -> juce_data_structures -> juce_events -> juce_core
```

`juce_core` last, as the campaign requires.

## 6. Verification and bench debts

Per phase, non-negotiable:

- Full app build, zero new warnings; `dusk-studio-tests` green.
- `tools/juce-gate.sh` passes, and every file the phase claims is gone from
  `tools/juce-allowlist.txt`. Files leave and never rejoin.
- Golden-image capture under headless `mutter`, diffed against the JUCE render of
  the same view. The spike's `--capture` shows the mechanism; G1 should
  generalise it.
- A live-Wayland pass on Marc's desktop for every view the phase touches. The
  standing rule that the app binary only runs under Xvfb applies to the JUCE
  build; a framework-native build has to be run on a real compositor, and
  `tools/gui-spike/headless-compositor.sh` is the safe way to do that from an
  agent session.

Owed to Marc's bench, and not inferable from this gate:

- **Live-desktop frame pacing.** Every number in §2 is headless. Compositing,
  vsync to a real output, fractional scaling and a loaded desktop are not
  represented.
- **Real input.** The headless compositor has no seat, so pointer, keyboard,
  scroll, text and cursor behaviour reached the application zero times. Drag
  feel, wheel resolution, double-click timing and modifier handling are all
  unverified through the real path.
- **HiDPI from the compositor.** The scale test drove an application-level
  factor. `wp_fractional_scale_v1` negotiation with a real output is untested.
- **Decoration behaviour on GNOME.** Row 13 above is read from the source; confirm on
  the desktop before committing to CSD.
- **Second GPU / Intel path.** All numbers are from the discrete Vega M.
- **macOS and Windows compiles of the spike.** Only Linux was available. The
  spike has no platform code beyond a `getrusage` block that compiles out
  elsewhere, and its CMake carries the same MSVC GL 3.x loader the notepad
  needs, but neither has been built. If the shell's portability is in question
  before G5, build `dusk-gui-spike` on both.
- **The accessibility decision** (§3, row 9), which is Marc's call and gates G3.

## 7. Naming

The fork stack is **DAF, the Dusk Audio Framework**. The rename is real, not
prospective: `dusk-audio/DPF` is now `dusk-audio/DAF` and `dusk-audio/DPF-Widgets`
is now `dusk-audio/DAF-Widgets`, the old URLs redirect, and the framework's `main`
already renamed its own public surface. **All future development is on DAF**; a
DPF spelling anywhere in this tree is transitional, not a second supported name.

Already moved: the repositories themselves, the framework's public API, the app's
preferred CMake variables (`DAF_PATH`, `DAF_WIDGETS_PATH`), the sibling
auto-detect (`../DAF` and `../DAF-Widgets` are tried first), the CI action's
clone URLs and pin variables, and the contributor build instructions.

Still mechanical, and deliberately left for a pass of its own rather than
smuggled into a tower — do it in G0 if it is cheap, otherwise as a standalone PR:

- `.github/actions/clone-dpf-stack/` is still the action's directory name, and
  its `RUNNER_TEMP/DPF` checkout directories and the `-DDPF_PATH=` flags in the
  workflows still use the old spelling. All three are named by 16 references
  across 8 workflow files, which is why they did not move here.
- `LICENSES.txt` records provenance as `dusk-audio/DPF rev <sha>`. It is
  hand-maintained release-audit surface and has to be regenerated when the pin
  moves anyway, so the repo name moves with the pin, not before it.
- The `DPF_PATH` / `DPF_WIDGETS_PATH` fallbacks in the app's CMake, and the
  `dpf__add_dgl_opengl3` and `dpf_resources` fallbacks, retire when the pin
  lands on the reconciled revision (§1.5).

## 8. Resume phrase

"GUI tower, phase G<n>" — read this file, then
[dejuce-campaign.md](dejuce-campaign.md) for the ritual. The gate evidence is
§2, the framework work is §1 and §4, and the phase you are on owns exactly the
files listed under it.
