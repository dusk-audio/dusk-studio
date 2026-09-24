# Native VST3 + LV2 plugin host — implementation plan

Status: **planning / awaiting approval to start Phase F1.** Authored 2026-07-01.

## Why

JUCE's VST3/LV2 hosting on Linux relies on `juce::XEmbedComponent` + an
out-of-process child whose X11 editor window is reparented under Wayland via
XEmbed. That path is the last major JUCE/Wayland liability after the CallOutBox,
menu, cursor, keystate, and file-dialog work already landed. The native CLAP
host (`src/engine/clap/`) proved the pattern — a native C/C++-SDK host with a
directly-embedded X11 editor is far more stable on Mutter/XWayland than JUCE's
XEmbed. This plan extends that to VST3 and LV2.

## Decisions (locked 2026-07-01)

1. **Both formats, in parallel**, shared foundation first.
2. **Accept GPLv3 for the whole binary.** See "License" — DuskStudio is *already*
   GPL-3.0-or-later, so this is a docs/metadata bump + a no-bundling rule, not a
   relicense.
3. **Native-hosted plugin params are automatable** (VST3 + LV2 + retrofit CLAP) —
   captured into Session automation lanes and played back.
4. **Generalize ports now** — mono / stereo / multi-out / sidechain /
   instrument+MIDI, not stereo-only like the CLAP host.

## License (verified clean)

`LICENSE` is GPL-3.0-or-later; JUCE is already consumed under its GPL arm (no
`JUCE_DISPLAY_SPLASH_SCREEN` / commercial define set); `LICENSES.txt:20` already
declares JUCE GPL-used-here; `CMakeLists.txt` sets
`CPACK_RPM_PACKAGE_LICENSE=GPL-3.0-or-later`. Vendoring the Steinberg VST3 SDK
under its GPLv3 arm and adding lilv/suil/lv2 (ISC) is therefore clean. Action
items are documentation + one invariant:

- Add VST3 SDK (GPL-3.0 arm) + lilv/suil/lv2 (ISC) to `LICENSES.txt`.
- **Hard invariant: ship ZERO third-party plugins in any distribution.** Bundling
  even a demo proprietary plugin voids GPL compliance for the whole binary.
- Dusk-owned VST3 SDK mirror + immutable tag (analog to
  `dusk-audio/JUCE-wayland` `dusk-wayland-v1`) — the plugdata-JUCE GC precedent
  makes a mirror mandatory for reproducible release CI, not optional.

## Architecture

A shared, host-agnostic layer under `src/engine/hosting/` that all three native
formats implement, so the audio-thread call sites and the automation/session code
never branch per format:

- **`PortLayout.h`** — negotiated shape produced once at load (message thread):
  audio/event buses tagged Main/Aux/Sidechain, channel counts, `isInstrument`,
  main/sidechain/event bus indices. Single source of truth; replaces
  `ClapInstance`'s two `inCh/outCh` ints.
- **`PortBuffers.h`** — the one audio-thread process argument replacing
  `(inL,inR,outL,outR,n)`: main/sidechain/multi-out audio pointers (into
  pre-sized scratch), a host-neutral MIDI event view/sink
  (`{sampleOffset,status,d1,d2}`), transport view. Nothing allocated on the RT
  thread.
- **`INativeInstance.h`** — abstract base every instance implements. Message
  thread: `create` / `negotiateLayout` / `activate` / `deactivate` /
  `saveState`/`loadState` / param enumeration / `getLatencySamples`. Audio
  thread: `void processBlock(const PortBuffers&) noexcept`.
- **`InsertAdapter.h/.cpp`** — reconciles an arbitrary `PortLayout` to the
  mixer's **fixed stereo-in/stereo-out insert contract**. Owns pre-sized scratch;
  the ONE place the "an insert is stereo" invariant lives. Fold rules: 1-in →
  average L,R to mono feed; mono-out → broadcast to L,R (generalizes the current
  `ChannelStrip.cpp:~719` averaging); multi-out insert → take main-out ch 0/1,
  drop aux buses; sidechain fed from the tap or pre-sized silence (never null).
- **`NativeInsertSlot`** (generalized from `NativeClapSlot`) — format-agnostic
  slot holding `unique_ptr<INativeInstance>` behind the lock-free `ready` flag;
  `leakForShutdown`; "at most one host per slot" enforced here.
- Automation: **`NativePluginParams.h`** (format-neutral `NativeParamInfo`
  {stableId,name,min/max/default,isStepped,...}) + **`PluginAutomationBridge`**
  (id-keyed lane container parallel to the fixed `AutomationParam` enum array).

Then per-format instances plug in: `Vst3Instance` (IComponent/IAudioProcessor/
IEditController, `setBusArrangements`/`activateBus`/`getBusInfo`, event input bus
for MIDI, `IParameterChanges` for automation) and `Lv2Instance` (lilv port
classification: audio/CV/control/atom-midi/morph). CLAP is retrofit onto the same
interfaces so `processStereo` disappears.

## Corrections folded in (from adversarial review)

- **`evaluateLane` is enum-closed** (`Session.cpp:314` → `denormalizeAutomation`
  switch + `isContinuousParam`). It **cannot** serve plugin params as-is. Split
  into `evaluateLaneNormalized(pts, t, bool continuous) -> float01` (interpolate/
  step core, no enum) + keep the enum wrapper for the fixed mixer params. Plugin
  lanes call the core with `continuous = !NativeParamInfo.isStepped`, then
  denormalize via the plugin's own range. (Phase F3.)
- **Single host per slot.** Replace the current parallel `nativeClapPath` field
  with one `enum {none,juce,clap,vst3,lv2}` + one path per slot (migrate the
  existing `nativeClapPath`). No four parallel optional path fields. (Phase F4.)
- **Plugin latency (PDC) is entirely absent today** — CLAP host never queries
  `clap_plugin_latency`; `ChannelStrip.cpp:610` already excludes insert-plugin
  tracks from silent-skip for this reason. Query latency at `activate()` via
  `INativeInstance::getLatencySamples`, feed the PDC aggregator, handle
  `latencyChanged` (VST3 `restartComponent kLatencyChanged` / CLAP
  `latency.changed` / LV2 latency port) on the message thread under the process
  gate. (Phase X1.)
- **AtomicSnapshot RT hazard**: lazy `push_back` of a new `PluginParamLane` while
  the audio thread iterates the vector is a data race. Pre-size the lane vector at
  `prepare()` (mark entries active) or wrap the vector in `AtomicSnapshot`; never
  raw-append under a concurrent RT reader. (Phase A1.)
- **Sidechain source index is a prerequisite, not "open"** — the RT sidechain tap
  needs a per-slot source index in Session; design it in the same increment as the
  tap. (Phase F7 + a Session field.)
- **Instrument-source + MIDI-out are first-class branches**, not afterthoughts:
  an instrument insert makes the plugin output *the strip's source* (bypass the
  pre/post crossfade blend, `ChannelStrip.cpp:~875`); a `MidiEventSink` with no
  consumer is a dead field until a MIDI-out destination is designed (deferred,
  stated).
- **Editor is a hard dependency for WRITE-capture automation** — gestures are
  captured from the plugin's own editor (`beginEdit`/`performEdit`/`endEdit`). Until
  the VST3/LV2 editors land, automation for those formats is READ-only. Stated
  explicitly, not assumed delivered.
- Fix stale `ClapInstance.h:62` "out may not alias in" comment — the in-place
  `processStereo(L,R,L,R)` call at `ChannelStrip.cpp:903` is safe only because of
  copy-in/copy-out scratch discipline; preserve that discipline in `processBlock`.

## Deferred (explicit non-goals for the first cut)

- Sample-accurate automation + MIDI event timing (all events at sampleOffset 0,
  matching current CLAP).
- Preset/program browsing (VST3 `IUnitInfo` program lists, LV2 `pset:Preset`) —
  session state-blob persistence works; factory-preset UI is a later axis.
- MIDI-out consumers (arpeggiator/note-FX routing).
- Multi-out "plugin as a bus/track source" (adapter drop-aux rule assumes insert).
- Cross-platform: VST3 native host is Linux-gated like CLAP for the first cut
  (macOS/Windows keep JUCE).

## Phase DAG

Every phase is ≤5 files, independently buildable, with its own verification. `F`
= shared foundation, `V` = VST3, `L` = LV2, `A` = automation, `X` = cross-cutting,
`S` = scan, `E` = editor UI, `Z` = cutover. Live-Wayland sign-off = no offline
test possible (launching on live Wayland crashes; harness can't drive a real
editor).

| id | phase | files | verification | dependsOn |
|----|-------|-------|--------------|-----------|
| F1 | `PortLayout.h` + `PortBuffers.h` + `INativeInstance.h` (interfaces) | 3 | Catch2: construct/read a PortLayout | — |
| F2 | `InsertAdapter.h/.cpp` fold rules + scratch sizing | 2 | Catch2: mono→stereo broadcast, stereo passthrough, multi-out drop-aux, silence sidechain | F1 |
| F3 | `evaluateLane` split → normalized core + enum wrapper (`Session.h/.cpp`) | 2 | Catch2: existing automation tests pass + normalized-core test | — |
| F4 | single-host-per-slot enum+path model + migration (`Session.h`, `SessionSerializer.cpp`) | 2 | Catch2: round-trip + `nativeClapPath` migration | — |
| F5 | retrofit `ClapInstance` onto `INativeInstance` (drop 2in/2out reject, PortLayout, `processBlock`) | 2 | A/B null-test vs current CLAP + Catch2 | F1,F2 |
| F6 | generalize `NativeClapSlot` → `NativeInsertSlot` (`unique_ptr<INativeInstance>`, one-host invariant) | 2 | Catch2: ready/bypass/leak + exclusivity | F5 |
| F7 | wire `ChannelStrip` + `AuxLaneStrip` call sites to `processInsert(PortBuffers)` (+ instrument-source branch, sidechain tap + Session source field) | 3 | A/B null-test CLAP unchanged; crossfade+instrument branch | F6 |
| X1 | plugin latency query + PDC aggregation + `latencyChanged` handling | 3 | Catch2: reported latency == measured delay | F6 |
| V1 | GPL/build: VST3 SDK submodule (Dusk mirror) + CMake gate + `LICENSES.txt` + `linux-release.yml` + `CLAUDE.md` | 5 | clean configure+build of host subset under `-Werror` | — |
| V2 | `Vst3Bundle` (dlopen module, `GetPluginFactory`, enumerate) | 2 | Catch2: load/enumerate a known `.vst3` | V1 |
| V3 | `Vst3HostContext` (IHostApplication/IComponentHandler/IPlugFrame/IRunLoop + fd/timer registry) | 2 | Catch2: IRunLoop **dispatches** (pipe fd → onFDIsSet; timer cadence) | V1 |
| V4 | `Vst3Instance` offline audio (bus negotiation → PortLayout, `processBlock`) | 2 | Catch2: silence-in/out, mono/stereo/sidechain layouts | F1,F2,V2,V3 |
| V5 | `Vst3Instance` dual-stream state (length-prefixed component+controller, restore order, atomic-park) | 1 | Catch2: round-trip incl controller-only change | V4 |
| V6 | `Vst3Editor` X11 embed + editor-test harness (`setFrame` before `attached`, `resizeView` honored, ContentScale) | 3 | live-Wayland sign-off | V4 |
| L1 | lilv/suil/lv2 pkg-config + `Lv2World`/`Lv2Bundle` + CMake gate | 3 | Catch2: world-load + resolve plugin URI | — |
| L2 | `Lv2Instance` offline audio (port classification, connect_port, atom-midi, `processBlock`) | 2 | Catch2: silence + port classification | F1,F2,L1 |
| L3 | `Lv2Instance` state + rate-change reinstantiate contract | 1 | Catch2: state survives simulated rate change | L2 |
| L4 | `Lv2Editor` suil X11 embed + harness | 3 | live-Wayland sign-off | L2 |
| A1 | `NativePluginParams.h` + `NativePluginHost` iface + `PluginAutomationBridge` (pre-sized/AtomicSnapshot lanes, release/acquire id→slot cache) | 3 | Catch2: arm/disarm no-RT-hazard, id stability | F3,F4 |
| A2 | Session `PluginAutomation` storage + serializer (id hex + normalized points, re-bind by stableId) | 2 | Catch2: round-trip re-bind | A1,F4 |
| A3 | CLAP `clap_host_params` retrofit (extension, 2nd audio-thread SPSC ring, editor-edit ring drain) | 3 | Catch2: param push/capture | A1,F5 |
| A4 | AudioEngine READ playback pass (armed plugin lanes → `evaluateLaneNormalized` → denormalize → RT ring) | 1 | Catch2: playback pushes value | A1,A3,F3 |
| A5 | CLAP WRITE capture wiring (editor gestures → touched/live → `captureWritePoint`, WRITE-only discrete) | 2 | live-Wayland sign-off (CLAP editor exists) | A3,A4 |
| A6 | VST3 automation (`IComponentHandler` begin/perform/endEdit capture + `IParameterChanges` playback) | 2 | live-Wayland sign-off | A4,V6 |
| A7 | LV2 automation (control-port capture + playback, endEdit quiesce heuristic) | 2 | live-Wayland sign-off | A4,L4 |
| S1 | extend `PluginScanProtocol` + scan child for native VST3/LV2 + skip-list cache | 3 | Catch2 protocol + crashy-plugin-doesn't-kill-app | V2,L1 |
| S2 | picker routing (`scanVst3`/`scanLv2` → PluginDescription `VST3-Native`/`LV2-Native`, `onPickNative` by format) | 3 | descriptor routing test | S1,F6 |
| E1 | `Vst3PluginEditorComponent` + wire into ChannelStrip modal + aux inline | 3 | live-Wayland sign-off | V6 |
| E2 | `Lv2PluginEditorComponent` + wire | 3 | live-Wayland sign-off | L4 |
| Z1 | keep JUCE fallback; route by format-name at load; optionally hide JUCE-format rows when native healthy | 3 | manual: both hosts coexist in picker | S2,F7,E1,E2 |

## Critical path

`F1 → F2 → F5 → F6 → F7` unlocks the DSP spine. VST3 (`V1→V2/V3→V4→V6→E1→A6`) and
LV2 (`L1→L2→L4→E2→A7`) run in parallel off the foundation. Automation core
(`F3,F4→A1→A2/A3→A4`) lands against CLAP first (A5), then grafts onto each format
once its editor exists. `V1` (GPL/build) and `L1` (deps) have no deps and can
start immediately alongside `F1`.

## Verification discipline

- DSP/session/engine phases: `cmake --build build-tests` + `ctest` must pass, plus
  an A/B null-test where a CLAP path is being generalized (target ~1e-6, per
  existing multicore A/B harness).
- Editor phases: live-Wayland sign-off only — the harness cannot drive a real
  plugin editor and launching on live Wayland crashes. Use the
  `DUSKSTUDIO_*_EDITOR_TEST` harness clones under Xvfb for construction/no-crash,
  Marc for the interactive pass.
- Each phase is its own reviewed commit; pause for approval between phases per
  CLAUDE.md.
