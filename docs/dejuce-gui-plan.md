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
The focus half is since fixed and validated on the prepared framework branches
(§4.1).

The GO is conditional on three things being treated as tower work rather than
discovered late: the decoration gap in §3, the frame-cost ceiling in §2, and the
accessibility bridge in §3. None of them is a research problem.

## 1. Framework revision reconciliation

**Landed.** The framework side is merged and the app is on it: `dusk-audio/DAF`
`main` is `92c3d1a7` — the reconciled Wayland revision plus the
application-contract fixes — with its pugl submodule at `43d8e349`, and
`dusk-audio/DAF-Widgets` `main` is `77daa22`. G0 moved `DAF_REV` and
`DAF_WIDGETS_REV` onto both. One action is left and it is Marc's: **delete the
branch `fix/wayland-review-findings` in `dusk-audio/DAF`**, so no third line can
accumulate. The rest of this section is the record of what was reconciled and
why.

### 1.1 Where the two lines stand

The app pinned `DAF_REV=f9fbc62a` (`.github/actions/clone-dpf-stack/action.yml`),
the tip of the framework branch `fix/wayland-review-findings`. That commit is
**not** an ancestor of the framework's `main`, which is the line the
Dusk plug-ins follow. Both descend from `a9b033c2`.

| | Commits since `a9b033c2` | Carries |
|---|---|---|
| Pinned branch `f9fbc62a` | 3 | `Window::setEmbeddedOffset`, the Wayland lifecycle and scaling fixes, the pugl mirror pointer |
| `main` | 31 | The DPF to DAF rename, a second round of Wayland lifetime fixes, AU and CLAP fixes, CI hardening, the pugl mirror pointer |

Before the reconciliation the pugl submodule was pinned to `5e2621d7` on
**both** sides, so the Wayland backend's own history was not divergent; all
divergence was in the framework repository itself. (`43d8e349`, the landed pin
recorded in §1, is where the `dusk/302-app-contract` work later moved the
submodule.)

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

### 1.2 The reconciled revision

Merged as three commits on top of `main`:

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

`dusk_notepad_ui` and the whole `DuskStudio` target build clean against the
result, and the spike runs on it.

Stacked on top of it was `dusk/302-app-contract`, four commits closing §3 rows
5, 6 and 8 and the framework half of §4.1 (issue #302). One of those commits
only moves the pugl submodule, whose own two commits merged in
`dusk-audio/pugl`. The ImGui bridge half of §4.1 and the fix for §4.4 came the
same way in `dusk-audio/DAF-Widgets`. Everything named here is now on the
respective `main`.

### 1.3 What the app has to change to consume `main`

The rename is not cosmetic at the build interface. Three things move:

| Was | Is | Where it bites |
|---|---|---|
| `DPF_LIBRARIES` / `DPF_EXAMPLES` | `DAF_LIBRARIES` / `DAF_EXAMPLES` | root `CMakeLists.txt` |
| `dpf__add_dgl_opengl3()` | `daf__add_dgl_opengl3()` | root `CMakeLists.txt` |
| `namespace dpf_resources` | `namespace daf_resources` | `src/ui/NativeNotepadWindow.cpp` |

Commit `39d19a6` made all three accept either spelling so the tree configured
against both live revisions. G0 retired that: the build speaks only DAF and the
`DUSKSTUDIO_DGL_RESOURCES` macro is gone. `DPF_PATH` / `DPF_WIDGETS_PATH` and
the `../DPF` search rungs stay until the mechanical rename pass (§7), because
the workflows still name them.

### 1.4 Choosing the backend

`daf__add_dgl_system_libs` selects X11 whenever the X11 development files are
installed, and Wayland only when they are absent. Before `DGL_BACKEND` there was
no way to ask for Wayland. On a dual-stack box or CI image that is silent: the
build succeeds and the app runs, just against XWayland — which is precisely what
the zero-XWayland policy for Dusk-owned surfaces forbids.

Commit 3 added `DGL_BACKEND` with values `auto` (the old behaviour, and still
the default), `x11` and `wayland`, and fails the configure when the requested
backend's libraries are missing.

Note the consequence for build layout: **the backend is a per-configure choice**,
because one `dgl-opengl3` target serves every consumer in the tree. A build
directory is X11 or Wayland, not both. The spike therefore lives in its own
build directory rather than beside the app.

**The app's own configures cannot take `wayland` yet, and G0 did not give it to
them.** The vehicle every phase up to G5 uses is a framework child window placed
inside the JUCE main window, and Wayland has no window embedding: pugl says so
(`wayland.c`, "Wayland has no window embedding, ignoring parent"),
`puglSetWindowPosition` answers `PUGL_UNSUPPORTED` there, and
`NativeNotepadWindow` deliberately refuses the backend rather than let the
notepad escape into a separate top-level. A `-DDGL_BACKEND=wayland` app build
therefore ships a notepad that cannot open, which G0 is not allowed to do. The
flag belongs to the spike's build directory today; the app's Linux configures
take it at G5, in the same change that stops embedding and gives the shell its
own decorations. `BUILDING-LINUX.md` documents the rule.

### 1.5 What was agreed, and what is left

Done, in this order, because each step is what made the next one build:

1. `dusk/302-app-contract` in `dusk-audio/pugl`, or the submodule pointer the
   DAF branch carries would dangle for every clone including CI.
2. `dusk/301-reconcile` in `dusk-audio/DAF` (the three commits above), then the
   `dusk/302-app-contract` branch stacked on it.
3. `dusk/302-app-contract` in `dusk-audio/DAF-Widgets`, which needs DAF's widget
   focus callback to compile.
4. `DAF_REV` and `DAF_WIDGETS_REV` re-pinned in
   `.github/actions/clone-dpf-stack/action.yml`, the single source of truth for
   every workflow, and the dual-spelling handling retired from the app's
   `CMakeLists.txt` and `NativeNotepadWindow.cpp` in the same change.
5. `LICENSES.txt` regenerated against the new revisions: both provenance stamps,
   the pugl submodule rev, and the Wayland notice inventory, which gained the
   xdg-foreign-unstable-v2 protocol pair.

Left, and it is Marc's: **delete the branch `fix/wayland-review-findings` in
`dusk-audio/DAF`**, so no third line can accumulate.

## 2. What the gate spike measured

Source: `tools/gui-spike/`. Off by default behind `DUSKSTUDIO_BUILD_GUI_SPIKE`.
Zero JUCE. Since G1 it is the widget kit's harness rather than a standalone
sketch: it draws its strip through `DuskWidgets` and links exactly one app file,
the JUCE-free `src/ui/imgui/DuskTheme.cpp`, so what it measures and captures is
the palette the app ships.

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

Headless is not the same as the desktop. Its `wl_seat` exists but advertises
zero capabilities, so no pointer, keyboard or keyboard-focus events ever arrive;
it does not run the desktop's compositing or effects; and its idea of output
scale is fixed. **The live-desktop pass is Marc's bench and is owed** (§6).

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

**Settled in G1, with numbers.** Lever 3 was measured first and it decided the
question. The dome is now three tinted quads read out of the font atlas
(`DuskWidgets::KnobAtlas`): a body multiplied by the knob's own colour, with its
rim and drop shadow baked in as black because black survives that multiplication,
a white sheen, and the pointer ticks. Only the pointer is still drawn per frame,
because it turns. That took the full console from 616,756 vertices a frame to
70,420, and from 45.6% of a core to 7.5%. Lever 2 followed as a rule in the
window's idle tick rather than a per-strip one - an immediate-mode frame is
rebuilt whole, so the granularity actually available is the window, and the
console is a 30 Hz picture unless a control is under the pointer - taking 24
strips to 5.2%. **Lever 1 was not implemented.** Caching a strip's draw list
buys a memcpy over a rebuild of 2,500 vertices, and costs a retained list per
strip plus an invalidation rule per parameter; the budget it would defend is
already an eighth of what the gate measured. The kit keeps the shape that makes
it possible later - `meterBackground()` and `meterBar()` are separate calls, so a
view that does cache can keep the well and its segment grid and redraw only the
bar - and G3 revisits it only if a view turns out heavier than the console.

Same machine, same method, ten second runs:

| Strips | gate baseline | baked domes, 60 Hz | baked domes + 30 Hz idle | verts/frame |
|---|---|---|---|---|
| 1 | 4.7% | 3.0% | 2.7% | 25,690 -> 2,930 |
| 8 | 18.3% | 4.4% | 3.0% | 205,492 -> 23,412 |
| 24 | 45.6% | 7.5% | 5.2% | 616,756 -> 70,420 |

A 24-strip window is 5,088 px wide, so that row needs a display to match:
`DUSK_HEADLESS_MONITOR=5120x1200`. Taken on the headless compositor's default
1,920 px monitor it reads 606,096 -> 59,868 instead, because the strips past the
edge draw no text - the spike now prints the window it was granted beside the one
it asked for, and warns when they differ, since a clipped frame is not the
console. The 1- and 8-strip rows fit either way, and the corrected 24-strip
figures are what their per-strip costs project to.

`--vector-knobs` is the control: it puts the strip back on the drawn dome and
reproduces the gate's own numbers (46.0% at 24 strips), so what the table shows is
the dome and not the port. The worst frame fell with the mean - 33.8 ms to 19.9 ms
at 24 strips - because the spikes were the vertex buffer growing.

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
| Modal blocks the strip underneath | Pass, once the spike opened the popup in the right id scope — see §4.1 |
| Key held across a focus change is released | Pass, on the prepared framework branches (§4.1) |

The interaction results come from `--selftest`, which injects at the ImGui event
queue. The headless compositor's seat advertises no capabilities, so the run
reports `pointer=0 key=0 text=0 focusIn=0 focusOut=0` through the real path: the
spike proves the widget layer, and deliberately proves nothing about pugl's seat
handling. Real focus delivery was proven separately, on the X11 backend under
Xvfb with two spike windows swapping focus.

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
| 5 | Portal parenting (xdg-foreign) | Fixed on `dusk/302-app-contract`, lands with the G0 repin | `wayland.c` export, `Window::getPortalParentHandle()` |
| 6 | Keyboard focus | Fixed on `dusk/302-app-contract`, lands with the G0 repin | `wayland.c:1555`, `Widget::onFocusChanged` |
| 7 | Clipboard | Partial (text only, no primary) | `wayland.c:4093`, `:4109` |
| 8 | Cursors | Hide fixed on `dusk/302-app-contract`, lands with the G0 repin; still no custom image | `wayland.c:2519`, `kMouseCursorNone` |
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
- **Focus (6).** DGL had no focus callback on `Widget` and never surfaced pugl's
  `PUGL_FOCUS_IN`/`OUT`, and the widgets library's ImGui bridge never called
  `io.AddFocusEvent()`. Worse than the audit read: on X11 pugl itself swallowed
  the events, because its dispatch loop updated the input context in the
  `FocusIn`/`FocusOut` cases and returned without dispatching. Both halves are
  fixed on the prepared branches — pugl dispatches, `Widget::onFocusChanged`
  carries it to every widget, and the bridge forwards it — with grab-mode
  crossings filtered out, since ImGui clears mouse buttons on focus loss and a
  menu's grab would cancel a live drag. §4.1 covers what that did and did not
  explain.
- **Cursors (8).** There was no hidden-cursor path and no custom cursor image,
  so the hide-pointer idiom every DAW knob drag uses was unavailable.
  `PUGL_CURSOR_NONE` now exists on all four backends (a null cursor surface on
  Wayland, an empty pixmap on X11, a transparent image on macOS, a null
  `HCURSOR` on Windows) and reaches DGL as `kMouseCursorNone`. A custom cursor
  image is still absent and nothing in the tower needs one.
- **Portal parenting (5).** Every file chooser opened through
  xdg-desktop-portal would have been unparented, appearing behind the main
  window or on another workspace. The prepared branches vendor
  xdg-foreign-unstable-v2, export the toplevel while realizing it, and hand the
  application the string the portal wants through
  `Window::getPortalParentHandle()`: `x11:` and a window id, or `wayland:` and
  an xdg-foreign handle, empty where there is none. Verified on both backends.
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

### 4.1 Window focus never reached the ImGui layer

`DGL::Widget` had no focus callback and the widgets library's `DearImGui.cpp`
never called `io.AddFocusEvent()`, so ImGui was never told whether the window
had the keyboard. On X11 it was worse: pugl's own dispatch loop handled
`FocusIn`/`FocusOut` by updating the input context and returning, so the event
never left the backend either.

**Two of the three consequences this section originally claimed were not
consequences of that at all.** The `modal blocks the strip underneath` selftest
case failed because of a defect in the spike, not in the framework: it called
`ImGui::OpenPopup` before `ImGui::Begin("##console")`, and a popup id is derived
from the id stack of the window that is current, so it opened a different popup
from the one `BeginPopupModal` draws inside the console. Instrumenting the case
showed `GetTopMostPopupModal()` returning null while `IsPopupOpen()` returned
true: the modal was never on screen, which is why nothing was blocked. With the
popup opened in the right scope and no framework change at all, the case passes
and `Escape` closes a popup too. ImGui's modal blocking runs through
`UpdateHoveredWindowAndCaptureFlags`, which never needed a platform focus event.

What was real is the third: key state is never cleared on focus loss, so a
modifier held while switching windows stays down. That needed the framework fix,
and has it on the prepared branches — `Widget::onFocusChanged` surfaced through
DGL (§3, row 6), `io.AddFocusEvent()` in the bridge, and the pugl X11 dispatch
corrected — adopted by the app at the G0 repin. The spike carries a ninth
selftest case for it, and the original eight still pass.

Owner: already prepared; G1 consumes it.

### 4.2 `WantCaptureKeyboard` is not a usable gate

`DearImGui.cpp` unconditionally sets `ImGuiConfigFlags_NavEnableKeyboard`, which
pins `io.WantCaptureKeyboard` true once navigation is active. Combined with
§4.1 it says nothing about whether the application may take a key. A DAW's
global shortcut layer needs its own routing rule — the spike uses "no text field
open and no item active" — and the tower should settle that rule once, in the
widget kit, rather than per view.

**Settled in G1**, as `DuskWidgets::shortcutsAvailable()`: an application may take a
key when no text field is open, no item is active, and no modal is up. The first
clause is the kit's own - a view that submits a `textField()` raises a flag on the
frame's context, so the rule already holds on the frame the field appears, before
ImGui has made it the active item. The third is what `IsAnyItemActive()` misses on
its own: a modal that is up owns the keyboard even when nothing inside it is being
edited. Views ask the kit rather than deciding for themselves; the spike's selftest
carries a case for a shortcut withheld while the strip name is being typed.

### 4.3 The font atlas is a fixed glyph set

ImGui bakes glyphs at atlas build time. The default range is Latin, which
silently dropped the fader's infinity mark; the JUCE UI gets it free from the
system font. Every non-Latin mark the UI uses has to be declared. Track name
entry is worse: a user typing in any non-Latin script gets nothing. Decide in
G1 whether to bake a wide range, or to load glyphs on demand.

**Settled in G1: named marks on the faces a view draws with, one wide face for text
entry.** `DuskWidgets::consoleGlyphRanges()` names what the widgets actually draw -
Latin-1, which carries the phase and degree marks, plus the infinity mark, the
dashes and quotes, arrows, triangles and the accidentals - and every drawing face is
baked from it. `textEntryGlyphRanges()` adds Greek, Cyrillic, Hebrew, Arabic,
Devanagari, currency, CJK punctuation and kana, and is baked once, for the single
face `textField()` uses. A new mark goes into the first list, where every face picks
it up. Nothing loads glyphs on demand: rebuilding an atlas mid-run means
re-uploading the texture from inside a frame, for a cost the table below does not
justify.

Measured with `dusk-gui-spike --glyph-report` across the eight faces a console
needs, from DejaVu Sans, the framework's embedded fallback:

| Policy | atlas at scale 1 | at scale 2 | glyphs | build |
|---|---|---|---|---|
| named marks, every face | 512x1024, 2 MB | 1024x2048, 8 MB | 1,848 | 9 ms |
| named marks + wide text entry (shipped) | 1024x1024, 4 MB | 2048x2048, 16 MB | 3,723 | 19 ms |
| wide on every face | 2048x2048, 16 MB | 4096x4096, 64 MB | 16,848 | 92 ms |
| whole Unicode blocks, every face | 2048x2048, 16 MB | 4096x4096, 64 MB | 11,704 | 63 ms |

The wide half of the shipped policy is what makes a non-Latin track name typable at
all, and it doubles a 2 MB atlas. Baking it on every face quadruples that again, for
glyphs no drawing face can ever show. The last row is what the spike's first cut
cost by naming whole Unicode blocks instead of marks: the same texture as the wide
bake, for a third fewer glyphs.

### 4.4 `ImGuiWidget<StandaloneWindow>` did not call `done()`

`StandaloneWindow` documents that `done()` must be called at the end of the
constructor to release the scoped graphics context. The widgets library's
standalone specialisation did not, so the final subclass had to. Fixed upstream
on the prepared branches: both standalone constructors call it, and `done()` is
idempotent so a subclass that still calls it is unaffected. The one thing that
changes for subclasses is that their own constructor body now runs with no
graphics context — the spike had to move its `glGetString` reporting to the
first frame — and `reinit()` is the way to ask for one back.

## 5. Phases

`src/ui/` is **119 files carrying 7,675 of the tree's 9,055 JUCE uses** (85%),
across roughly 54,000 lines of `.cpp`. It cannot land in one PR. The
incremental vehicle is the pattern the notepad already ships: a framework child
window embedded over the JUCE main window through the native parent handle and
`Window::setEmbeddedOffset`, with the JUCE side reduced to a placeholder. That
is why `setEmbeddedOffset` must survive the reconciliation.

Each phase ends with the app shipping.

### G0 — Framework alignment

**Landed.** No user-visible change. `DAF_REV` and `DAF_WIDGETS_REV` moved to the
reconciled revisions (§1), the dual-spelling handling is gone, `LICENSES.txt`
carries the new provenance, and the spike's two validation commits are in the
tree. `DGL_BACKEND` is documented in `BUILDING-LINUX.md` but **not** passed on
any app configure: §1.4 says why, and G5 is where the app takes it.

Owned: `CMakeLists.txt`, `.github/actions/clone-dpf-stack/action.yml`,
`src/ui/NativeNotepadWindow.cpp`, `LICENSES.txt`, `BUILDING-LINUX.md`. The
leftover mechanical renames in §7 stayed out: the checkout directories and
`-DDPF_PATH` flags are named by 16 references across 8 workflow files, which is
a pass of its own.
Gate movement: none. Verified: full build, 787 tests, notepad opens under Xvfb,
spike selftest 9 of 9 under headless `mutter`.

### G1 — Widget kit and theme

**Landed.** No user-visible change. The spike's drawing layer is a module in
DAF-Widgets that the app and the first-party plug-in UIs both consume:
`opengl/DuskWidgets.{hpp,cpp}` carries the SSL knob and its baked dome, the
full-travel fader with its dB gutter, the segmented meter, the gain-reduction
column, the module pill, buttons, the drag value bubble and the text field, plus
the pieces a view needs around them - the skewed `Range`, the meter ballistics,
the shared value formatting, the font builder and the glyph sets. It depends on
Dear ImGui alone, not on DGL, and builds under C++11 so a plug-in UI can take it.
Values go in and come back out: no widget writes through a pointer, so the caller
decides whether a parameter lives in an atomic, a host parameter or a plain
float. `tests/duskwidgets` in that repo is a gallery of the whole set.

App-side, all JUCE-free:

- `src/ui/imgui/DuskTheme.{h,cpp}` — the console palette as the kit's theme
  tokens plus the accents the kit has no opinion about. The values are
  `DuskStudioLookAndFeel`'s, written out rather than read from it: reading the
  look-and-feel would couple a new file to JUCE for a table of colours, and the
  gate would gain a file. Each value names the constant it came from.
- `src/ui/imgui/DuskImGuiHost.{h,cpp}` — the embedded-window lifecycle lifted out
  of `NativeNotepadWindow`: create the child over the host window, refuse a
  display that cannot carry it, pump the framework's loop on a message-thread
  timer, survive a driver that fails inside that pump, and tear the child down
  over the two ticks the platform needs to unmap it. A view is now the widget the
  caller builds in `createWidget`; everything around it belongs to the host.
- `src/ui/imgui/FirstFrameProbe.h` — the first-frame guard, moved out of
  `NotepadFirstFrameProbe.h` because it guards a window rather than the notepad.
  Its marker format string is unchanged: an installed build's marker has to stay
  readable by the next one.

`NativeNotepadWindow` is the first consumer, rebased onto the host and 271 lines
shorter, with the same user-facing failure text and the same deferred close.

Landing order: the widget set has to be on `dusk-audio/DAF-Widgets` `main` before
the app change merges, and `DAF_WIDGETS_REV` in
`.github/actions/clone-dpf-stack/action.yml` has to name that commit rather than a
branch tip. Every workflow clones that revision; a branch that is deleted after
its merge would strand all of them.

One trap in the API: `StandaloneWindow` inherits a `setCursor` from both of its
bases, so app code has to name the window's, `Window::setCursor(...)`.

**The shared widget kit's home is `dusk-audio/DAF-Widgets`, not the plug-ins
repo** (Marc, 2026-08-24). The knob, fader, meter, module pill and theme tokens
go there, beside the Dear ImGui layer they build on, and both Dusk Studio and the
plug-ins consume them from that one place. The plug-ins repo's
`shared-dpf/ui/DuskImGuiWidgets.hpp` is the previous home: anything in it worth
keeping migrates into DAF-Widgets, and it is not extended in place. Do not fork a
second knob in `src/ui/`, and do not add to `shared-dpf/ui/` — a widget that only
Dusk Studio needs still belongs in DAF-Widgets if a plug-in could ever want it.

Gate movement: none — 178 files, 9,050 uses, unchanged. Verified: full app build
with no new warnings; 807 Catch2 cases green, eight of them a new headless
`imgui_widget_kit` suite that draws a whole strip with no window, no GL and no
compositor and asserts what the draw list contains, the baked dome against the
drawn one included; the notepad opens under Xvfb through the new host; spike
selftest 11 of 11 under headless `mutter` (the gate's nine plus the value bubble
and the withheld shortcut); the §2.2 and §4.3 measurements above.

Golden frames: `tools/gui-spike/golden-frames.sh capture|compare <dir>` captures
five variants - a strip, that strip at scale 2, a bank of eight, the modal, the
context menu - under a private headless compositor and diffs a later run against
them channel by channel; `--static` freezes the meters so the frame is the same
every run. On one machine it is exact: repeat runs differ by zero in every
channel. That is why the goldens are captured on the spot rather than committed -
a different GPU or driver renders the same draw list to slightly different pixels,
so a committed set would be a per-machine expectation dressed up as a repository
one. The invariant that does belong in CI went into the headless suite instead.

### G2 — Dialogs and panels

First shipping phase. Move the self-contained modal family, one file per commit,
each becoming an embedded framework window: `AudioSettingsPanel`,
`StartupDialog`, `ChannelCompEditor`, `MasteringEqEditor`,
`MasteringLimiterEditor`, `VirtualKeyboardComponent`. Re-point the
`EmbeddedModal`, `DuskComboBox` and `showContextMenu` seams at ImGui
implementations. Portal parenting is no longer new work here: the first file
dialog reads `Window::getPortalParentHandle()` from the reconciled framework
(§3, row 5) and passes it as the portal's parent window.

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
  the same view. `tools/gui-spike/golden-frames.sh` is the mechanism from G1
  onwards: capture before the change, compare after, and read the per-machine
  caveat in G1 before treating a mismatch as a defect.
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

Moved with the G0 pin, because a stale provenance record is a defect rather than
a naming preference: `LICENSES.txt` now reads `dusk-audio/DAF rev <sha>`, and
the `dpf__add_dgl_opengl3` and `dpf_resources` fallbacks are gone.

Still mechanical, and deliberately left for a pass of its own rather than
smuggled into a tower:

- `.github/actions/clone-dpf-stack/` is still the action's directory name, and
  its `RUNNER_TEMP/DPF` checkout directories and the `-DDPF_PATH=` flags in the
  workflows still use the old spelling. All three are named by 16 references
  across 8 workflow files, which is why they did not move here.
- The `DPF_PATH` / `DPF_WIDGETS_PATH` variables and the `../DPF` search rungs in
  the app's CMake stay until those workflow flags move, since they are what the
  flags land on.

Deliberately left saying DPF: the shipped changelog entries and the per-release
handoff documents, which are records of what was true at a revision rather than
instructions for today.

## 8. Resume phrase

"GUI tower, phase G<n>" — read this file, then
[dejuce-campaign.md](dejuce-campaign.md) for the ritual. The gate evidence is
§2, the framework work is §1 and §4, and the phase you are on owns exactly the
files listed under it.
