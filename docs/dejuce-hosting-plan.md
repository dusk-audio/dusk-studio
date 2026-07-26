# De-JUCE — plugin-hosting tower (campaign plan)

Status: **H1 executed on branch `dejuce/hosting`; H2-H6 planned.** Marc's call on
2026-07-26 reversed the 2026-07-01 keep-JUCE-fallback decision: the JUCE
plugin-hosting path gets deleted entirely; native hosting must cover
VST3 + LV2 + CLAP on Linux, macOS, and Windows, plus AU on macOS.
Multi-PR tower (>15 files per phase, RT-risky and mechanical work mixed) —
one PR per phase, sequenced below. Do not start phase N+1's PR before N
merges. Read `docs/dejuce-campaign.md` + the memory ledger first.

## End-state

- Only hosts: `INativeInstance` implementations (clap/, lv2/, vst3/, au/).
- `juce_audio_processors` unlinked on all three platforms (module count
  12 -> 11; `juce_audio_utils`/`juce_audio_formats` still wait for the GUI
  tower's AudioThumbnail replacement).
- Deleted: PluginSlot, PluginManager's JUCE half (AudioPluginFormatManager /
  KnownPluginList / createPluginInstance), JuceCompat.h, PluginHostMain's
  JUCE message loop + MessageManagerLock sites (the OOP child shrinks to the
  native scan sandbox — the shm/futex layer is already tri-platform),
  JUCE_PLUGINHOST_* defines.
- Donor JUCE processors out of the app: TapeMachineAudioProcessor and
  UniversalCompressor (Multiband) replaced by JUCE-free cores + native UI.
- DuskMultisampleProcessor re-homed off juce::AudioPluginInstance.

## Ground truth (2026-07-26 scouts)

- Strips/buses already run JUCE-free donor cores: FourKEQDSP (the 4k-eq-2
  core — adopted in PR #55 with a deliberate sonic re-baseline; "use
  4k-eq-2" is DONE), UniversalCompressorDSP (Opto/FET/VCA/Bus),
  MultiQTube. Build points at the `plugins-multicomp-core` worktree
  (DUSK_PLUGINS_PATH in both build caches) — its core scope is LOCKED and
  excludes Multiband.
- Remaining JUCE donor in-app: UniversalCompressor mode 7 Multiband
  (MasteringChain bindCompParams + MasteringView's embedded donor
  MultibandCompressorPanel writing mb_*).
- TapeMachine2 = TapeMachine/dpf-plugin + TapeMachine/core TapeMachineDSP:
  verbatim port of the same algorithm Dusk Studio hosts today (null-test
  expected), 20 atomic setters + VU getters + latencySamples. Two donor
  setters are documented DEAD: setSaturation, setNoiseEnabled — check which
  params Dusk Studio actually drives before flipping.
- Native host cores are portable C++ (ClapInstance 531 LOC, Lv2Instance
  1035, Vst3Instance 721, hosting/ shared layer). Platform surface is
  contained: bundle loaders (dlopen -> per-OS shim; mac .clap bundle dirs),
  scan paths (FHS hardcoded), editors (Xlib TUs; JUCE getNativeHandle
  already returns NSView*/HWND so the embed components generalize),
  CLAP posix-fd ext (absent on Windows by spec — timers only), VST3
  Linux::IRunLoop (collapses away on mac/win), CMake gates (Linux-only
  conditions today; vst3sdk ships module_mac.mm / module_win32.cpp).
- LV2-on-Windows risk: no vcpkg suil port. Options when H5 lands: vendor
  suil, or ship LV2 headless-params-only on Windows first. Decision
  deferred to H5 spec.
- OOP host: JUCE audio path only + native scan sandbox (clap/vst3). After
  the drop it keeps only scanning; its juce_events dispatch loop goes with
  the JUCE path (also closes the events-tower out-of-scope items).
- Deletion surface: 36 files typed against AudioProcessor /
  AudioPluginInstance / PluginDescription / KnownPluginList outside the
  native dirs (scout table in session notes; re-scout at H4 kickoff).

## Phases

- **H1 — TapeMachine2 swap (Dusk Studio). DONE on `dejuce/hosting`.**
  MasterBus hosts TapeMachineDSP through the `MasterTape` wrapper
  (prepare/processInPlace/latencySamples, oversampling set in prepare, the
  on/off crossfade owned by MasterBus); tape settings live in session JSON
  as plain values with one-way migration from the old getStateInformation
  blob, and the AudioEngine state/playhead sites are gone. Native
  `src/ui/TapePanel.{h,cpp}` replaces TapeMachineModalEditor; the donor
  TapeMachine sources left the app build. A/B null test in
  `tests/tape_core_ab.cpp`. Remaining: H1d (TM2 DPF UI embed) once the donor
  is consolidated - see
  [dejuce-hosting-h1-tape.md](dejuce-hosting-h1-tape.md).
- **H2 — Mastering multiband (donor first, then Dusk Studio).** Donor:
  port Multiband mode into a JUCE-free core (extend
  multi-comp/core scope or sibling MultibandCompressorDSP; Marc's
  "Multicomp will need to be ported to DPF"). Merge donor main, bump
  DONOR_REV in all 8 workflows together. Dusk Studio: MasteringChain off
  UniversalCompressor/APVTS onto the new core; MasteringView native
  multiband panel replacing the embedded donor panel. A/B vs JUCE
  multiband.
- **H3 — DuskMultisample re-home.** Off juce::AudioPluginInstance /
  AudioProcessorEditor / AudioPluginFormat onto an internal-instrument seam
  (its params are already plain atomics; sfizz does the DSP — coupling is
  shim-shaped). Editor becomes a plain Component.
- **H4 — descriptor/plumbing de-JUCE.** Replace PluginDescription /
  KnownPluginList surfaces with a dusk descriptor type: picker +
  PluginPickerHelpers, NativeScanRows, scan caches, session references,
  PluginScanProtocol wire format, ScreenshotCapture fake rows,
  DuskStudioApp session-restore path. Mechanical but wide; re-scout first.
- **H5 — platform ports (mac, win).** Flip CMake gates per-OS; loader
  shims (dlopen/LoadLibrary, mac bundle resolution); per-OS scan paths;
  editor embed variants (COCOA/HWND CLAP targets, kPlatformTypeNSView/HWND,
  suil Cocoa/Windows hosts); CLAP fd-ext gated off on win; VST3 host
  context minus IRunLoop; AU layer cloned from the CLAP template
  (ClapBundle/Host/Instance/Editor/Scanner + NativeInsertSlot traits,
  ~1.3k LOC) on AudioComponent/AudioUnit APIs, macOS only. Verified via
  macos-build.yml per push (full app + tests) and windows-tests.yml;
  windows-build.yml full app needs workflow_dispatch. Runtime sign-off =
  Marc's Mac/Windows bench, first-class TODO.
- **H6 — the drop.** Delete the JUCE hosting path everywhere at once (H5
  made mac/win self-sufficient), unlink juce_audio_processors on all
  platforms, retire tsan suppressions tied to deleted primitives in the
  same PR, gate ratchet update.

Ordering: H1..H4 are platform-independent and can proceed now (after the
FFT PR merges — one PR at a time). H5 before H6 strictly. H2's donor half
gates H2's app half, not the other phases.

## Standing risks / owed

- Session compatibility: tape + multiband settings migrate from JUCE state
  blobs to param snapshots — one-way migration, test with a real 0.12
  session.
- Marc bench: tape null-listen, multiband null-listen, mac/win native
  hosting with real third-party plugins, AU with stock Apple units.
- LV2-on-Windows suil decision at H5.
- DUSK_PLUGINS_PATH still points at the plugins-multicomp-core worktree;
  donor consolidation (merge core work to donor main, retire worktree,
  repoint) should happen at H2's donor step.

## Resume phrase

"Hosting tower, plan docs/dejuce-hosting-plan.md — check phase statuses,
execute next pending phase (one PR per phase)."
