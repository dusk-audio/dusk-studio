# De-JUCE — events-remainder tower (executable spec)

Status: **E0–E3 DONE 2026-07-25, committed on `dejuce/events-remainder`,
awaiting Marc's review + push.** Verified: app build zero new warnings, gate
182 (no rise), ctest 450/450 (one later rerun hit the documented
alsa_seq_midi PipeWire env flake; binary functionally unchanged from the
green run), selftest + screenshot harness green under Xvfb. One PR for the
tower. Read `docs/dejuce-campaign.md` and the memory ledger first, per ritual.

## Goal, honestly stated

Flip every migratable `juce_events` call site onto the existing seams
(`dusk::Timer`, `dusk::callAsync`, DeviceManager-style listener fanout).
**Zero gate movement, zero module unlink** — a 2026-07-25 full-allowlist scan
confirmed no file reaches zero JUCE tokens from an events-only flip (every
Timer host is a `juce::Component` or is anchored by String/AudioProcessor).
The yield is finale-prep: after this tower, the GUI tower replaces the event
loop by rewriting `src/foundation/MessageThread.cpp` alone, instead of ~90
scattered call sites. The gate count (182) must not *rise*.

## Out of scope (anchored elsewhere — do not touch)

- `src/foundation/MessageThread.cpp` — the seam backend stays JUCE-backed
  until the GUI tower brings its own event loop.
- `src/engine/ipc/PluginHostMain.cpp` `MessageManagerLock` ×5 and
  `runDispatchLoop`/`stopDispatchLoop` — the OOP host needs the JUCE message
  loop while it hosts JUCE-format plugins; plugin-hosting tower scope.
  (Its one `callAsync` at :647 DOES flip.)
- `src/engine/device/DeviceManagerJuce.cpp` `juce::ChangeListener` — listens
  to `juce::AudioDeviceManager` itself (mac/win path).
- `juce::ChangeListener` on `juce::AudioThumbnail` (AudioRegionEditor) and
  `juce::UndoManager` (MiniTimelineStrip, TapeStrip) — anchored by those JUCE
  objects; they go when the anchor goes.
- `juce::MessageManager::existsAndIsCurrentThread()` asserts
  (AudioEngine.cpp:2090, :2281) and `getInstanceWithoutCreating()` headless
  probe (BounceEngine.cpp:47) — thread-identity queries the seam does not
  model yet; GUI tower gives the seam a native `isMessageThread()`.

## E0 — seam extension (2 files: `src/foundation/MessageThread.{h,cpp}`)

Add to `dusk::Timer` (all thin forwarders to the JUCE impl, same pattern as
the existing methods):

- `static void callAfterDelay (int milliseconds, std::function<void()>)` —
  forwards to `juce::Timer::callAfterDelay`.
- `int getTimerInterval() const noexcept` — only if a call site in E1/E3
  actually needs it; check before adding. Do not add speculative surface.

`dusk::callAsync` stays as-is (raw post, callers own their lifetime guards —
that is the established pattern at all 7 current seam users).

Verify: app build + ctest (no new tests — seam is message-loop-bound, which
the test policy excludes).

## E1 — engine + app-shell flips (owns these files exclusively)

Files: `src/engine/AudioEngine.{h,cpp}`, `src/engine/PluginSlot.{h,cpp}`,
`src/engine/BounceEngine.cpp`, `src/engine/ipc/PluginHostMain.cpp` (callAsync
:647 only), `src/engine/multisample/DuskMultisampleProcessor.cpp`,
`src/DuskStudioApp.cpp`.

1. Timer flips — `juce::Timer` → `dusk::Timer`, keep intervals and
   start/stop call shapes identical:
   - `AudioEngine::PerfReporter` (AudioEngine.cpp:222)
   - `AudioEngine::NativeParamDrain` (AudioEngine.cpp:280)
   - `CallbackDiagnosticTimer` (AudioEngine.h:849)
   - `PluginSlot` private base (PluginSlot.h:31) + `ReaperTimer` (:333)
   - `DuskStudioApp::BounceTest` (DuskStudioApp.cpp:1090)
2. callAsync flips — `juce::MessageManager::callAsync` → `dusk::callAsync`,
   captures unchanged (existing SafePointer/WeakReference/shared_ptr guards
   stay exactly as they are): BounceEngine.cpp:62, PluginHostMain.cpp:647,
   DuskMultisampleProcessor.cpp:252 + :280, DuskStudioApp.cpp:168 + :2126.
3. `AudioEngine : public juce::ChangeBroadcaster` → replace with the
   DeviceManager listener-fanout pattern (copy `src/engine/device/
   DeviceManager.{h,cpp}` `addChangeCallback`-style void*-token +
   `std::function` + coalesced `dusk::callAsync` broadcast with keepAlive —
   read it first, mirror it exactly). **MUST-CHECK before implementing:**
   grep every `sendChangeMessage()` call site in AudioEngine and prove each
   runs on the message thread (hot-plug timer, device callbacks). If ANY can
   run on the audio thread, do NOT route it through `dusk::callAsync`
   (allocates); use an atomic flag drained by an existing 30 Hz timer and
   note it in the PR. Subscriber flips (AudioSettingsPanel,
   ChannelStripComponent) belong to E3 owners — E1 must keep a
   source-compatible registration API name agreed here:
   `addChangeCallback (void* token, std::function<void()>)` /
   `removeChangeCallback (void* token)`.
   E1 lands the producer + flips both subscribers' *call sites* only if the
   E3 owner has not started those files yet — coordinate by file ownership,
   never two agents in one file.
4. Drop `#include <juce_events/juce_events.h>` from PluginSlot.h if nothing
   else in the header needs it (PluginHostMain.cpp keeps its include — the
   dispatch loop stays).

Verify: app build zero new warnings, full ctest, gate script (count must
stay ≤ 182).

## E2 — folded into E1 (producer) + E3 (subscribers). No separate phase.

## E3 — UI flips (38 Timer classes, 37 callAsync sites, 2 callAfterDelay)

Mechanical per file: `juce::Timer` → `dusk::Timer` (add
`#include "../foundation/MessageThread.h"`, adjust for subdirs), method names
`startTimer/startTimerHz/stopTimer/isTimerRunning/timerCallback` are
identical so bodies do not change; `juce::MessageManager::callAsync` →
`dusk::callAsync` with captures untouched;
`juce::Timer::callAfterDelay` → `dusk::Timer::callAfterDelay`
(AuxLaneComponent.cpp:1305, MainComponent.cpp:905).

File inventory (partition disjointly across builders; a builder owns every
listed edit in its files):

- Batch A: AnalogVuMeter.h, AudioRegionEditor.h, AuxLaneComponent.{h,cpp},
  AuxView.h, BounceDialog.h, BusComponent.{h,cpp}, ChannelCompEditor.h,
  CompMeterStrip.h, ConsoleView.h
- Batch B: ChannelStripComponent.{h,cpp} (Timer + callAsync ×2 + engine
  listener re-registration), ClapPluginEditorComponent.h, DuskComboBox.cpp,
  FreezeDialog.h, HardwareInsertEditor.h, Lv2PluginEditorComponent.h,
  Vst3PluginEditorComponent.h, PluginScanModal.{h,cpp}, StartupDialog.h,
  SystemStatusBar.h, UpdateChecker.h
- Batch C: MainComponent.{h,cpp} (Timer + OneShotTimer + TunerPoller +
  ~20 callAsync + callAfterDelay :905), SelfTestPanel.cpp,
  EmbeddedModal.h (callAsync ×4), PianoRollComponent.{h,cpp}
- Batch D: MasteringEqEditor.h, MasteringLimiterEditor.h, MasteringView.h,
  MasterStripComponent.{h,cpp} (Timer + callAsync ×2),
  MiniTimelineStrip.h (Timer only — UndoManager listener stays),
  TapeMachineModalEditor.h, TapeStrip.{h,cpp} (Timer + callAsync — UndoManager
  listener stays), TransportBar.h, VirtualKeyboardComponent.h,
  multisample/DuskMultisampleEditor.h, AudioSettingsPanel.{h,cpp} (engine
  listener re-registration)

Rules for every batch:
- If a class overrides/uses JUCE Timer surface the seam lacks
  (`getTimerInterval`, `MultiTimer`) — stop, report back, do not improvise.
- Engine-listener re-registration (ChannelStripComponent, AudioSettingsPanel):
  swap `engine.addChangeListener(this)` / `changeListenerCallback` for the
  E1-agreed `addChangeCallback(this, [this]{...})` / `removeChangeCallback
  (this)` in the destructor; the callback body is the old
  `changeListenerCallback` body for the engine-broadcaster case only (the
  UndoManager/Thumbnail `changeListenerCallback` branches stay JUCE where a
  class listens to both — keep both mechanisms side by side in that case).
- Do not remove `juce::` tokens beyond the events flip; no drive-by cleanup.

Verify after all batches: app build zero new warnings, gate ≤ 182, full
ctest, `DUSKSTUDIO_RUN_SELFTEST=1` under Xvfb (private DISPLAY,
WAYLAND_DISPLAY unset), Xvfb screenshot harness sanity for the heavy UI
files, AI-slop sweep of the whole diff.

## Owed to Marc's bench

- Live-Wayland launch sanity — PAID 2026-07-25: session load, CLAP editor
  embed, timer-driven UI, full shutdown chain all clean on live Wayland.
- OOP plugin host smoke (PluginHostMain callAsync flip) with a real
  out-of-process plugin — still open.

## Resume phrase

"Events-remainder tower, branch dejuce/events-remainder — check spec status
line, continue at first unchecked phase."
