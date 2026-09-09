> **PARKED until 1.0.** See [docs/decisions/0001-ship-1.0-on-juce.md](decisions/0001-ship-1.0-on-juce.md).

# GUI accessibility bridge — issue #305

**Decision, 2026-09-08:** preserve screen-reader support with a platform
accessibility bridge. G3 must preserve the existing semantic controls and pass
desktop accessibility checks before the JUCE console is replaced. This decision
does not waive the G2 desktop checks in [the GUI plan](dejuce-gui-plan.md).

**First phase:** inventory the current support floor and correct the channel
fader's accessible mute value and infinity-text parsing. No native bridge is implemented by this phase;
#305 remains open. The next implementation must connect real native controls
to a platform adapter and test their actions, not stop at a detached tree model.

## What exists today

This is a source audit of main `64efac2` and its fader correction, not a report
from a running screen reader. Explicit accessibility annotations occur in six
UI files: MainComponent, ChannelStripComponent, AuxLaneComponent, TransportBar,
DuskComboBox and SplitModuleButton. Other JUCE widgets still inherit default
handlers, so absence of explicit annotations is not absence of semantics.

The Linux JUCE-wayland input supplies the shared handlers but no Linux native
accessibility implementation. Its `modules/juce_gui_basics/juce_gui_basics.cpp`
includes native accessibility implementations for macOS, Windows, iOS and
Android; `native/accessibility/juce_Accessibility.cpp` supplies an empty native
implementation otherwise. Therefore the labels below are existing application
semantics to preserve, not evidence of working Linux AT-SPI or Orca support.
Linux exposure is added capability; macOS and Windows require preservation of
their existing native accessibility paths. Neither platform is signed off by
this source audit.

| Surface and source | Existing names, roles, values and actions | Limits to carry into the bridge work |
|---|---|---|
| [MainComponent](../src/ui/MainComponent.cpp), constructor | Title `Dusk Studio mixer`; description `16-channel portastudio-style mixer`; default component role is unspecified. The root requests keyboard focus. | The description is stale: the session has 24 tracks. The comment promising per-strip focus setup is not supported by a `setWantsKeyboardFocus` call in ChannelStripComponent or ConsoleView. Do not claim a verified strip traversal order. |
| [ChannelStripComponent](../src/ui/ChannelStripComponent.cpp), constructor | Container title `Track N`, description `Channel strip for track N`. Track-qualified titles identify fader, pan, mute, solo, record arm, input monitor, print effects, HPF/LPF, EQ, insert slot and aux sends. | Compressor helper titles use their control label; the constructor's claim that every control is named is broader than the track-qualified title list. Preserve default button text and tooltip help too. |
| Channel fader | Slider role, range −100 to +12 dB, step 0.1, reset to 0 dB on double-click. Value text is `-INF dB` at or below `ChannelStripParams::kFaderInfThreshDb` (−90), otherwise one decimal plus ` dB`. Case-insensitive `-INF` / `-INF dB` input selects the muted minimum. | Before this phase the formatter used −99.95, misreporting 100 valid hard-muted slider values. DSP uses ≤ −90; the standalone visible label uses ≤ −89.95, equivalent for the 0.1 dB slider grid. The default text parser interpreted the infinity label as zero; this phase keeps an accessible string round-trip muted and retains the default finite-number parsing. |
| Channel pan and filter knobs | Slider roles. Pan speaks `C`, `L<pct>` or `R<pct>`. HPF/LPF speak `OFF` at their bypass ends, otherwise formatted frequency. Compressor helpers supply titles, suffixes and formatted values, including FET ratio. | Preserve units and bypass words. Pan and FET ratio still lack matching text parsers: `L50`/`R50` parse to centre, and `4:1`/`8:1`/`12:1`/`20:1` parse or clamp to index 4 (`All`). Their editable readouts and accessible string setters need separate corrections before native parity; this phase changes only the fader. |
| Channel mute/solo/arm/monitor and [TransportBar](../src/ui/TransportBar.cpp) | JUCE button handler supplies press, checkable/checked state and `On`/`Off` for toggleable buttons. Transport has `Transport bar` plus `Play`, `Stop`, `Record`, `Rewind`, `Fast forward` titles. | Actions must use the existing callback path. A transport press invokes engine behavior; mute callbacks also handle automation. Writing only a boolean would omit these effects. |
| Channel print/freeze control | `refreshPrintButtonForMode()` updates title and help between `Print effects on record`, `Freeze track` and `Unfreeze track`. | The dynamic title loses the constructor's track prefix. Preserve the mode-dependent meaning; track disambiguation is a follow-up, not established parity. |
| [AuxLaneComponent](../src/ui/AuxLaneComponent.cpp), constructor | `Aux N`, description `Aux send/return lane N`; `Aux N return fader`, `Aux N mute`, `Aux N plugin slot M`. JUCE slider/button defaults supply value and actions. | Bus and master controls also inherit JUCE defaults; this audit found no matching explicit title pass there. They need a per-control inventory before their port. |
| [SplitModuleButton](../src/ui/SplitModuleButton.h) | Explicit group role; two real child buttons named `<label> enabled` and `Open <label> editor`. The indicator is toggleable, and `refresh()` synchronizes external model state before repaint. | Keep both children actionable. A single painted region with one press loses the bypass/editor distinction. Child titles do not inherit a custom track-qualified group title. |
| [DuskComboBox](../src/ui/DuskComboBox.cpp), MenuPanel | Explicit popup-menu role with press action. Painted rows are represented by the panel's active-row title; navigation emits `titleChanged`. Empty grid search announces no matching presets. | This is active-row announcement, not a tree of selectable row children. Disabled entries and headings cannot be activated. A richer row tree would improve this floor, not describe existing behavior. |
| [EmbeddedModal](../src/ui/EmbeddedModal.h) | Opening requests body keyboard focus. A modal stack restores focus to the newest remaining body, or the registered shell target. Deferred restoration is invalidated when a newer modal opens. | There is no explicit accessible dialog/modal role or accessibility-tree exclusion in this seam. Visual dimming and keyboard handling do not establish screen-reader modality. |

The JUCE behavior above is grounded in `juce_Slider.cpp`,
`detail/juce_ButtonAccessibilityHandler.h`, `juce_Button.cpp` and
`juce_Component.cpp` under `modules/juce_gui_basics` in the selected JUCE input.
Slider accessible set-value uses `ScopedDragNotification`, synchronous value
notification and the same min/max/interval as the widget. Its string setter uses
the widget parser. Slider help comes from its tooltip, even when the app also
sets help text. Buttons similarly use tooltip help; their press action triggers
the button callback, and their toggle action updates the toggle state with
notification.

## Keyboard and focus contract

These are source-supported handlers, not proof that every control is reachable
from Tab in the shipped layout:

- With shell keyboard focus in Recording/Mixing, unmodified Left/Right calls
  `ConsoleView::moveFocus`, moving the painted strip ring and the A/S/X shortcut
  target. This is separate from native accessibility focus and Tab traversal.
- A focused JUCE slider accepts unmodified Up/Right to increment one accessible
  interval and Down/Left to decrement it, with synchronous notification.
  Accessible set-value additionally brackets the change with drag start/end;
  those hooks matter for fader Touch automation and grouping.
- A focused JUCE button accepts Return to trigger its click. Do not promise
  Space for every button: Space is also a shell transport shortcut.
- The flat combo menu uses Up/Down, Page Up/Down and Home/End to move among
  selectable rows; Return or accessible press activates the active row. Escape
  dismisses. The grid has its own navigation; Escape first clears an active
  filter and only then closes. Every active-row change must remain announceable.
- EmbeddedModal handles Escape unless dismissal is disabled. Its shortcut
  forwarding admits selected transport/navigation keys, excludes edit and
  destructive keys, and respects a body's claimed keys. Text entry and nested
  popups must retain their keys. Focus restoration must not jump past a still
  open modal or steal focus after another modal opens.

## Native seams and the first bridge slice

[DuskPanelView and DuskPanelWindow](../src/ui/imgui/DuskPanelWindow.h) already
separate view drawing from window lifecycle, modal dismissal and shortcut
ownership. [DuskImGuiHost](../src/ui/imgui/DuskImGuiHost.h) owns the native window,
geometry and deferred teardown. Neither exposes an accessibility tree.
[PanelControls](../src/ui/imgui/PanelControls.cpp) owns form combos, buttons,
checkboxes and sliders, but its `##` IDs, separate painted labels, tooltips and
ImGui focus do not currently publish platform semantics. DAF/DAF-Widgets remain
shared read-only inputs during this application phase.

The first implementation candidate is the **General section of the existing
[AudioSettingsView](../src/ui/imgui/AudioSettingsView.cpp)**: `Expand tape strip
by default` (checkbox), `Autosave every` (combo with five named choices), and
`UI scale` (0.50–2.00 slider with `%.2fx` value). These exercise state, selection,
numeric value, clipping, help and modal focus using real controls. UI scale is
particularly useful proof: it previews while changing and persists on release.
The accessible action must take the same apply/release route. Tests must inject
settings effects rather than touch the user's settings or audio devices.

Proposed ownership, subject to the next phase's reviewed implementation scope:

1. App-owned semantic submission beside these control calls supplies explicit
   name, role, stable identity, bounds, enabled state, current value and actions.
   A panel instance owns identities; reopening must invalidate old actions.
   An ImGui label hash alone must not identify a control across native windows.
2. The panel window publishes a completed snapshot after layout. Focus and
   selected value changes must be observable without a pointer event. Controls
   outside a clipped scroll view must not claim visible bounds; focus actions
   must scroll a target into view before reporting it focused.
3. A platform adapter consumes that snapshot. AT-SPI actions arriving off the UI
   thread queue commands for the UI owner; they do not dereference a view or
   mutate settings directly. The current panel generation, visibility, enabled
   state and modal ownership are rechecked when executing each action.
4. The same command path handles pointer, keyboard and accessibility activation.
   Closing first invalidates the action target, then destroys the view/adapter
   in the native host's deferred lifecycle. Late queries use owned snapshots,
   never pointers into frame-local strings or ImGui draw data.

Evaluate **AccessKit's C API** before writing three platform adapters. Its
documented adapters target Linux AT-SPI, macOS NSAccessibility and Windows UI
Automation. The C bindings provide CMake integration and require Rust when built
from source. This is a candidate dependency, not an adopted or pinned one.
([Adapter overview](https://accesskit.dev/how-it-works/),
[C integration and build requirements](https://github.com/AccessKit/accesskit-c/blob/main/README.md))
The dependency decision must resolve embedded-child ownership alongside the
JUCE shell, native handles and focus events, callback threading, clean builds
on all three platforms and licensing notices. Do not substitute synthetic
click coordinates for semantic actions.

The next phase must be split into reviewed batches of at most five files;
structural changes to the large host/view files require their own cleanup
commit first. It should produce a working, limited native slice and deterministic
tests together. Completing those three controls alone does not make the whole
settings panel accessible or unblock the console port.

## Support floor and proof required before replacement

| Platform | Required bridge outcome | Proof still owed |
|---|---|---|
| Linux, native Wayland and supported X11 path | AT-SPI exposes named controls, roles, values, actions, focus and modal ownership. | Query the live tree and invoke actions on a private accessibility bus; then verify actual Orca reading and keyboard use on Marc's desktop. This is new Linux platform exposure. |
| macOS | NSAccessibility retains the current app semantics and connects native child panels to the host window. | Build and inspect the tree, invoke value/press actions, and test VoiceOver across child open/close and focus restoration. |
| Windows | UI Automation retains the current app semantics and connects native child panels to the host window. | Build and inspect control patterns, invoke actions, and test Narrator or NVDA across child open/close and focus restoration. |

Deterministic semantic/action tests must cover the actual first-slice producer:

- Stable IDs across redraw/scale; correct parentage, names, roles, help, ranges,
  selected labels and checked states; missing controls removed on the next
  snapshot; no duplicate or dangling children after reopen.
- Checkbox press, combo selection and numeric set-value take the same model
  path as direct interaction. Numeric input rejects non-finite values, respects
  the real range/step, and preserves begin/change/end semantics and release-only
  persistence. Disabled controls and stale IDs cannot mutate the model.
- Focus follows keyboard/accessibility changes, reveals clipped controls and
  moves into a modal; nested popup dismissal restores the panel before the shell.
  Late actions after close and actions behind the active modal are rejected.
- No unchanged snapshot or meter polling produces repeated announcements. No
  accessibility callback or tree work runs on the audio thread.

Source-bound fader boundary checks, unit tests, a headless semantic tree and
successful builds prove different layers. None establishes physical screen-reader
usability. Preserve the JUCE console until its full control inventory, native
parity, platform integration and outstanding desktop checks have passed.

## Resume

Issue #305 first phase is the fader correction and this inventory. Next: review
the adapter/embedded-host dependency decision, then implement the three real
Audio Settings controls with semantic/action tests and isolated platform proof.
The GUI tower's G3 replacement remains gated.
