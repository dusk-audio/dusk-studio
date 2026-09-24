# Giant-file split plan

Five UI translation units exceed the ~2000-line comfortable-read /
incremental-compile threshold. Splitting them into per-concern TUs (all
implementing the same class — legal C++) speeds incremental builds and
keeps each file readable in one pass. **No behavior change** — pure
mechanical extraction.

## When to do this

Deferred until the in-flight multisample work lands. Four of the five
files currently have uncommitted edits; splitting them now would
entangle the refactor with that feature diff. Split all five in one
phased pass (one file per commit, build between) once the tree is clean.

## Verification per file

UI has no Catch2 harness, so verification is:
1. `scripts/dev.sh app` — compiles + links (catches missing symbols /
   moved helpers).
2. Launch the binary, exercise the split component's surface (draw,
   click, drag, keyboard) — catches nothing the linker would, but
   confirms no behavioral regression from a mis-moved method.

## Split convention

Each file → up to 4 TUs implementing the same class:

| Suffix | Holds |
|---|---|
| `<Name>.cpp` | ctor/dtor, `resized()`, layout, accessors, timer, core state |
| `<Name>_Paint.cpp` | every `paint*` method + colour helpers |
| `<Name>_Mouse.cpp` | `mouseDown/Drag/Move/Up/WheelMove`, `keyPressed`, hit-testing |
| `<Name>_Edit.cpp` | edit ops + context-menu/popup builders |

Drop a suffix when a file doesn't warrant it. Add each new `.cpp` to
the `Dusk Studio` target's source list in `CMakeLists.txt`.

### Shared file-local helpers

The hazard: anonymous-namespace helpers + file-`static` functions are
TU-local. If a moved method uses one, it breaks. Two fixes:
- **Partition** — if a helper is used by only one concern group, move it
  into that group's TU. (PianoRoll's colour palettes are paint-only, so
  they move to `_Paint.cpp` with no sharing — verified below.)
- **Promote** — if a helper is genuinely shared, lift it into a
  `<Name>Internal.h` (a `namespace duskstudio::<name>_detail` with
  `inline`/`constexpr` members to avoid ODR issues) included by every
  split TU.

---

## Per-file seams

### PianoRollComponent.cpp (3430 lines, 72 methods) — ANALYSIS DONE

Symbol partition verified clean — no internal header needed:
- Anon-ns colour constants (`kBgDark`…`kNoteEdge`, `kTransportPlayhead`)
  used ONLY by paint methods → move to `_Paint.cpp`.
- `kPitchClassPalette` used only by `colourForNote` (paint) → `_Paint.cpp`.
- `kPalette`/`kPaletteCount` used only by `showRegionPropertiesPopup`
  → stays in core/edit.
- file-`static snapTick()` used only by mouse methods → `_Mouse.cpp`.
- static member `sNoteClipboard` definition stays in core.

Move to `_Paint.cpp`: `paint`, `paintToolbar`, `paintNoteGrid`,
`paintBeatRuler`, `paintKeyboard`, `paintNotes`, `paintEditCursor`,
`paintVelocityStrip`, `paintCcStrip`, `colourForNote`,
`paintTransportPlayhead`, `transportPlayheadX`, `IconButton::paintButton`.

Move to `_Mouse.cpp`: `hitTestNote`, `hitTestCcBar`, `hitTestVelocityBar`,
`mouseDown`, `mouseDrag`, `mouseMove`, `mouseUp`, `keyPressed`,
`mouseWheelMove`, `snapTick`.

Move to `_Edit.cpp`: selection ops, `applyGroupMove`,
`transposeSelected`, `quantizeSelected`, `humanizeVelocity`,
`glueSelectedNotes`, `duplicateSelectedNotes`, step-record, `nudge*`,
all `show*Popup`, `splitSelectedAtCursor`.

Core keeps the rest (ctor, `region()`, coord transforms, `resized`,
layout, scrollbar, status bar, zoom, navigate, `timer`).

### ChannelStripComponent.cpp (4511, 49 methods, 3 anon ns)
Biggest. Likely seams: `_Paint` (strip background, meters, GR),
`_Eq` / `_Comp` (the inline editor sections + their popups), `_Mouse`
(fader/knob/bus-assign handlers), core (layout, mode switching, timer).
Audit the 3 anon namespaces for cross-group helpers before moving.

### TapeStrip.cpp (3826, 52 methods, 0 anon ns)
No anon-namespace hazard — cleanest to split. Seams: `_Paint` (ruler,
region bodies, waveforms), `_Mouse` (region drag/trim/split/select),
core (layout, collapsible rows, timer, clipboard ops).

### AudioRegionEditor.cpp (3426, 57 methods, 2 anon ns)
Seams: `_Paint` (waveform, fades, gain envelope), `_Mouse` (edit
gestures), `_Edit` (region property popups, automation param menu),
core. Audit the 2 anon namespaces.

### MainComponent.cpp (3195, 51 methods, 4 anon ns)
4 anon namespaces — most helper-audit needed. Seams: `_Menu` (menu-bar
model + handlers), `_Session` (save/load/import/bounce flows),
`_Layout` (resized, stage switching), core (ctor, transport wiring,
quit handling).
