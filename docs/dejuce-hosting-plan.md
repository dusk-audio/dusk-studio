# De-JUCE — plugin-hosting tower (campaign plan)

Status: **H1a-c merged (PR #114); H3 merged (PR #118); H4 merged (PR #119,
`3c5c901`); H5 scout complete; H5a ready on `dejuce/hosting-h5a`; H1d blocked on donor
consolidation; H2 blocked on the donor multiband port.** Marc's call on
2026-07-26 reversed the 2026-07-01 keep-JUCE-fallback decision: the JUCE
plugin-hosting path gets deleted entirely; native hosting must cover CLAP and
VST3 on Linux, macOS, and Windows, LV2 on Linux and macOS, plus AU on macOS.
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
- LV2 on Windows is deferred: vcpkg has lilv but no suil port, and H5c will
  neither vendor suil nor ship a parameters-only LV2 tier.
- OOP host: JUCE audio path only + native scan sandbox (clap/vst3). After
  the drop it keeps only scanning; its juce_events dispatch loop goes with
  the JUCE path (also closes the events-tower out-of-scope items).
- Deletion surface: the completed post-H3 H4 re-scout covered the picker and
  helpers, native scan rows and caches, session references, scan wire format,
  screenshot fixtures, restore paths, and MIDI bindings. H4 moved descriptor
  plumbing to the Dusk type; the remaining processor/editor ownership stays
  explicitly deferred to H6.

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
- **H3 — DuskMultisample re-home. DONE (PR #118).**
  DuskMultisampleProcessor implements hosting::INativeInstance; fourth
  native rung (NativeMultisampleSlot, ladder CLAP > LV2 > VST3 > MS > JUCE);
  session keys native_multisample_path/state with one-way legacy desc-XML
  migration; format wrapper deleted (gate 183 -> 181); two-phase
  prime/commit keeps sfizz parses outside the engine gate; spec:
  [dejuce-hosting-h3-multisample.md](dejuce-hosting-h3-multisample.md).
- **H4 — descriptor/plumbing de-JUCE. DONE (PR #119, `3c5c901`).** Dusk
  `PluginDescriptor` now owns picker, native scan/cache, scan-protocol,
  session-reference, offline/clone, screenshot-fixture, and restore plumbing.
  The private JUCE host boundary converts only where H6 still needs it.
  Session schema v4 preserves structured descriptors and migrates legacy XML;
  native sidecar caches are versioned JSON with one-way XML import. Gate
  181 -> 179; full CTest 493/493 plus private-Xvfb self-test, synthetic SFZ
  sound verdict, and picker screenshot review passed. Spec:
  [dejuce-hosting-h4-descriptors.md](dejuce-hosting-h4-descriptors.md).
- **H5 — platform ports (three sequential PRs; H5a is READY).** H5a ports
  CLAP/LV2/VST3 to macOS, including bundle discovery/loading, Cocoa editor
  embeds, and the portable VST3 host context. H5b adds the native macOS AU
  layer on AudioComponent/AudioUnit APIs. H5c ports CLAP/VST3 to Windows,
  with CLAP POSIX-fd support absent and VST3's Linux IRunLoop removed.
  Windows LV2 stays deferred. H5a must merge before H5b starts, and H5b must
  merge before H5c starts. Executable spec:
  [dejuce-hosting-h5-platform.md](dejuce-hosting-h5-platform.md). Verification
  runs through macos-build.yml and windows-tests.yml; windows-build.yml's full
  app remains workflow_dispatch. Runtime sign-off is owed to Marc's real
  Mac/Windows bench.
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
- Windows LV2 is deferred beyond H5; H5c ships CLAP/VST3 only.
- DUSK_PLUGINS_PATH still points at the plugins-multicomp-core worktree;
  donor consolidation (merge core work to donor main, retire worktree,
  repoint) should happen at H2's donor step.

## Resume phrase

"Hosting tower, plan docs/dejuce-hosting-plan.md — check phase statuses,
execute next pending phase (one PR per phase)."
