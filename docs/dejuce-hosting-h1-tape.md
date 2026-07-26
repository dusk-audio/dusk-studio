# Hosting tower H1 — TapeMachine2 swap (executable spec)

Status: **H1a–H1c pending.** Branch `dejuce/hosting`. One PR for H1.
Parent plan: [dejuce-hosting-plan.md](dejuce-hosting-plan.md). Donor core:
`plugins/TapeMachine/core/TapeMachineDSP.{hpp,cpp}` at the build's
DUSK_PLUGINS_PATH (worktree @ 69f0431; PORT_NOTES.md in that dir is
authoritative on semantics).

## Goal

MasterBus hosts `duskaudio::TapeMachineDSP` directly; the donor JUCE
`TapeMachineAudioProcessor` + its editor leave the app build entirely. Tape
settings become first-class session params (plain JSON) with one-way
migration from the old base64 blob. Native Dusk tape panel replaces the
masked donor editor.

## Locked decisions

- Param storage follows house convention #1: new `TapeParams` atomics on the
  session master params; UI writes atoms, MasterBus pushes to core setters
  per block (setters are lock-free atomic stores; push all 18 live params
  unconditionally each block).
- Dead donor params `saturation` and `noiseEnabled` (PORT_NOTES 3.6/3.7) are
  NOT modeled; migration ignores them. `tapeHQ` stays dead-legacy.
- Donor preset cluster (Save/Del/preset combo) is DROPPED — session owns the
  state now. Behavior change, listed in the PR body for Marc.
- Supporters overlay, HQ/oversampling combo (engine-driven), resize corner:
  gone with the donor editor.
- Reels: reuse donor `TapeReelComponent` (plain juce::Component) in the
  native panel ONLY if it compiles standalone without PluginProcessor /
  APVTS includes; drive spin from engine transport-playing + tapeSpeed +
  wow atoms. If it drags processor deps in, ship v1 without reels and note
  it.
- VU: Dusk's own `src/ui/AnalogVuMeter.h` fed from core `getVuL/getVuR`
  (output peak, 300 ms release — semantic change vs donor RMS in-meter,
  acceptable per core PORT_NOTES 3.9; note in PR body).
- Arm-on-touch preserved: any tape-panel control change sets
  `params.tapeEnabled = true` (replaces the AudioProcessorListener arm).
- Migration may use JUCE (`getXmlFromBinary` + XmlElement) — juce_core /
  juce_data_structures outlive this tower; the code lives in serializer
  scope and dies with the GUI tower.

## H1a — engine/session flip (RT-critical)

1. `src/session/Session.h`: `TapeParams` struct on master params — atomics:
   machine, speed, type, signalPath, eqStandard, calibration (ints);
   inputGainDb, bias, highpassHz, lowpassHz, noiseAmount, wow, flutter,
   outputGainDb (floats); autoCal, autoComp (bools). Defaults = donor layout
   defaults (machine 0, speed 1, type 0, path 0, eq 0, cal 0, input 0,
   bias 50, hpf 20, lpf 20000, noise 0, wow 7, flutter 3, output 0,
   autoCal on, autoComp on). Keep existing `tapeEnabled`; delete
   `tapeStateBase64` after migration wiring.
2. `SessionSerializer`: write `"tape"` object (plain values) under master;
   load reads it. One-way migration: when `"tape_state"` (base64) present
   and `"tape"` absent, decode via MemoryBlock::fromBase64Encoding +
   juce getXmlFromBinary, walk `<PARAM id value/>` children, map ids →
   fields (values are plain units; choice indices arrive as float). Stop
   writing `"tape_state"`. Drop `Session.h` tapeStateBase64 member and the
   AudioEngine publish/consume sites (AudioEngine.cpp ~1631, ~2079).
3. `src/dsp/MasterBus.{h,cpp}`: member becomes `duskaudio::TapeMachineDSP`;
   include flips to `<core/TapeMachineDSP.hpp>`; delete `getTapeProcessor()`
   (fix ConsoleView/MasterStrip call sites in H1b), `tapeStereoBuffer`
   (already unwritten), `tapeMidi`, `tapeBypassAtom`, the
   setPlayConfigDetails/dynamic_cast-oversampling/getRawParameterValue
   plumbing. prepare: `tape.setOversampling(idx)` BEFORE `tape.prepare(sr,
   blockSize)`; `tapeLatencySamples = tape.latencySamples()`; PDC delay
   sizing unchanged. process: per block push all TapeParams atoms + bypass
   (`setBypass(! runTape)` replacing the atom stores at :326/:330), then
   `tape.processBlock(lrView, lrView, n, 2)` (in-place contract: verify
   core supports in==out from PORT_NOTES/source; if not, use the dry
   scratch as out and swap). Chunk bound stays (prepared blockSize).
4. `AudioEngine.cpp:2265` setPlayHead site: delete (playhead was
   reel-animation-only). `prepareForSelfTest` keeps preparing MasterBus.
5. `AudioPipelineSelfTest.cpp` tape sweep: drive via session TapeParams
   atoms (inputGainDb/outputGainDb/autoComp) instead of APVTS
   setValueNotifyingHost; same sweep, same 0.5 dB assertion.
6. Tests (`tests/tape_core_ab.cpp`, gated on DUSK_PLUGINS_PATH like
   console_saturation): A/B TapeMachineAudioProcessor vs TapeMachineDSP,
   same params, OS 1x -> exact null (WithinAbs 1e-6 after latency align);
   OS 2x/4x -> bounded residual (tolerance from measurement, document);
   latencySamples() == getLatencySamples() at each OS factor;
   silence-in/silence-out; bypass passthrough. Keep tests/tape_hysteresis
   as-is.

## H1b — native tape panel + CMake excision

1. New `src/ui/TapePanel.{h,cpp}` (embedded-modal body, follow
   MasteringLimiterEditor / BusCompEditorPanel house style): 5 DuskComboBox
   (machine/speed/type/signalPath/eqStandard), calibration combo, 8 knobs
   (input, bias [greyed when autoCal on], hpf, lpf, wow, flutter, output,
   noise), autoComp + autoCal toggles, TAPE enable button, AnalogVuMeter on
   core VU atoms (poll via MasterBus accessor), optional reels per locked
   decision. All controls read/write session TapeParams atoms; any change
   arms tapeEnabled; 20-30 Hz timer resyncs from atoms (dusk::Timer).
2. `MasterStripComponent`: drop TapeMachineAudioProcessor* member +
   AudioProcessorListener + createEditor path; `openTapeMachineModal` shows
   TapePanel in the same EmbeddedModal flow (DimOverlay toggle behavior
   preserved); ConsoleView.cpp:37 ctor arg gone.
3. Delete `src/ui/TapeMachineModalEditor.h`; remove from allowlist (gate
   drops). Grep for other includes.
4. CMakeLists: remove the 6 donor TapeMachine .cpp from the app target, the
   `createPluginFilter=createTapeMachinePluginFilter` rename hack, the
   Source/GUI include dir if nothing else uses it (tests keep theirs), the
   TapeMachine VersionString defines; add
   `${DUSK_PLUGINS_PATH}/plugins/TapeMachine/core/TapeMachineDSP.cpp` to
   app + tests. MasteringChain multicomp donor sources stay (H2).

## H1c — verify

Build zero new warnings; gate must DROP by at least 1 (TapeMachineModalEditor
gone) — update allowlist, never let it rise; full ctest incl. new A/B;
selftest under Xvfb (tape sweep now exercises the core); screenshot harness
+ visual review of the new tape panel and master strip (UI change — review
every affected variant per standing rule); AI-slop sweep; MANUAL.md audit
(tape editor screenshots/wording likely reference donor UI — update).

## Owed to Marc's bench

- Tape null-listen old vs new at 1x/2x/4x on real program material.
- Panel usability pass (knob ranges, layout) — native panel is a redesign.

## Resume phrase

"Hosting H1, branch dejuce/hosting, spec docs/dejuce-hosting-h1-tape.md —
continue at first unchecked phase."
