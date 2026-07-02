# Task: wire the native LV2 host into the mixer DSP (make LV2 plugins loadable in-app)

You are working in **DuskStudio** (`/home/marc/projects/DuskStudio`, branch `feature/juce-removals-wayland`). Read `CLAUDE.md` first — its Agent Directives, audio-thread rules, and DSP-lifecycle conventions are binding.

## What already exists (don't rebuild)

A native LV2 host is fully built and tested at the **audio-instance level**, mirroring the existing native CLAP host:

- **Shared foundation** `src/engine/hosting/`: `INativeInstance` (host-agnostic contract), `PortLayout`, `PortBuffers`, `InsertAdapter` (folds a plugin's real bus layout onto the mixer's stereo insert).
- **LV2** `src/engine/lv2/`: `Lv2Bundle`, `Lv2Instance` (implements `INativeInstance` via lilv), **`NativeLv2Slot`** — a per-insert slot that is a **direct mirror of `src/engine/clap/NativeClapSlot`** (same public API: `load`/`unload`/`reactivate`/`leakForShutdown`/`isLoaded`/`getPath`/`setBypassed`/`saveState`/`loadState`/`processStereo`/`getInstance`). It routes audio through the `InsertAdapter`.
- Validated against real effects via `DUSKSTUDIO_TEST_LV2=/path/to/bundle.lv2` (e.g. `~/.lv2/Universal Compressor.lv2`, `/usr/local/lib/ardour9/LV2/a-comp.lv2`).

**`NativeLv2Slot` is done and proven.** Your job is to plug it into the mixer so a user can actually load an LV2 plugin into a channel/aux insert, persist it, and edit it.

## The pattern to mirror: native CLAP integration

Everywhere the codebase already does native CLAP, add a parallel native LV2 path. Find every touch point:

```
grep -rn 'nativeClapSlot\|NativeClapSlot\|loadNativeClap\|isNativeClapLoaded\|pendingClap\|scanClapPlugins\|onPickNativeClap\|nativeClapPath\|DUSKSTUDIO_HAS_NATIVE_CLAP' src/
```

Mirror each for LV2 (`nativeLv2Slot`, `loadNativeLv2`, `isNativeLv2Loaded`, `pendingLv2Path`, `scanLv2Plugins`, `onPickNativeLv2`, `nativeLv2Path`, gate `DUSKSTUDIO_HAS_NATIVE_LV2`). The CLAP code compiles under `#if DUSKSTUDIO_HAS_NATIVE_CLAP` with `#else` stubs — do the same with `DUSKSTUDIO_HAS_NATIVE_LV2` (already defined by CMake on Linux when lilv/suil are present).

## Steps (each its own small commit; verify + pause between phases per CLAUDE.md)

### 1. ChannelStrip DSP integration (`src/dsp/ChannelStrip.{h,cpp}`)
Mirror the CLAP surface exactly:
- Header: `#include "../engine/lv2/NativeLv2Slot.h"` (gated), a `lv2::NativeLv2Slot nativeLv2Slot` member, `pendingLv2Path`/`pendingLv2State`, and the API (`loadNativeLv2`/`unloadNativeLv2`/`isNativeLv2Loaded`/`getNativeLv2Slot`/`setPendingNativeLv2`) with `#else` stubs.
- `.cpp`: mirror `prepare()`'s reactivate/pending-restore block, `loadNativeClap`/`unloadNativeClap`/`setPendingNativeClap`.
- **Audio-thread call sites** (find them: `grep -n 'nativeClapSlot.processStereo' src/dsp/ChannelStrip.cpp` — mono + stereo insert branches). Add an `else if (isNativeLv2Loaded()) nativeLv2Slot.processStereo(...)` branch alongside the CLAP one. **RT rules apply — no alloc, no lock, no logging on this path.**

### 2. One-host-per-slot invariant (NEW — the CLAP-only code never needed this)
An insert slot must hold **at most one** of {JUCE `pluginSlot`, `nativeClapSlot`, `nativeLv2Slot`}. Enforce at load time: `loadNativeLv2` unloads CLAP + the JUCE slot; `loadNativeClap` (edit it) unloads LV2 + JUCE; loading a JUCE plugin unloads both native slots. The audio-thread if/else chain must be provably exclusive (only one `isXLoaded()` true). Session currently notes `nativeClapPath` is "mutually exclusive with the JUCE plugin" (see `Session.h`) — extend that to three-way. Consider one enum `{none,juce,clap,lv2}` + one path per slot rather than parallel optional fields, if it's a clean refactor; otherwise parallel fields + strict unload-others is acceptable for this pass.

### 3. AuxLaneStrip (`src/dsp/AuxLaneStrip.{h,cpp}`)
Same as ChannelStrip but the **array** form — aux lanes have `nativeClapSlots[sIdx]` (`kMaxLanePlugins`). Add `nativeLv2Slots[]` and the branch inside the crossfade gate (`grep -n 'nativeClapSlots' src/dsp/AuxLaneStrip.cpp`).

### 4. Session persistence (`src/session/Session.h`, `src/session/SessionSerializer.cpp`, `src/engine/AudioEngine.cpp`)
Add `nativeLv2Path` + `nativeLv2StateBase64` (track + aux), always-present so non-Linux sessions round-trip. Mirror `nativeClapPath` serialization. In `AudioEngine`, mirror the CLAP branch of `consumePluginStateAfterLoad` (load fenced by `suspendProcessing()`/`resumeProcessing()` when prepared, else `setPendingNativeLv2`), the save side, and `leakAllPluginInstancesForShutdown` (call `nativeLv2Slot.leakForShutdown()` too). **`Lv2Instance::saveState/loadState` currently return false (state not wired)** — so session persistence stores the path only for now; that's fine, note it.

### 5. Picker routing (`src/engine/PluginManager.{h,cpp}`, `src/ui/PluginPickerHelpers.*`)
Add `Lv2Scanner` (mirror `src/engine/clap/ClapScanner` — walk `LV2_PATH` + `~/.lv2` via a shared `LilvWorld`, filter to audio effects, emit `juce::PluginDescription` with `pluginFormatName == "LV2-Native"` into an `lv2-native-cache.xml`). Route a picked LV2-Native row to `loadNativeLv2` (mirror `onPickNativeClap`). **Use the existing sandboxed scan child if practical (crashy plugin shouldn't kill the app on first scan); at minimum note if you don't.**

### 6. Editor (`src/ui/Lv2PluginEditorComponent.*`) — the Wayland-risk part, do last
Mirror `src/ui/ClapPluginEditorComponent` + `src/engine/clap/ClapEditor`, but embed via **suil** (`suil_instance_new` into an unmapped X11 host window, reparent `suil_instance_get_widget`). This is where the actual JUCE-escape payoff lands, and the riskiest — read the CLAP editor's X11 embed sequence carefully (unmapped-until-reveal, XMapWindow host before reparent) and the `PlatformWindowing` `preferX11ForNextNativeWindow` / non-fatal-X-error-handler shims. Requires `suil` (already found via pkg-config).

## Gotchas already handled in `Lv2Instance` (don't undo)
- JUCE/DPF-wrapped LV2 plugins hard-require **urid-map + options(min/max/nominal block + sampleRate) + boundedBlockLength**; Ardour's minimal plugins don't. The feature set is built in `Lv2Instance::assembleFeatures`.
- **Output atom ports** need `atom->size` re-advertised before every `run()` (done).
- Every LV2 port (audio/control/atom/**CV/unknown**) must be connected before `run()` or it's UB.
- Bundle URIs must end in `/`.

## Verification (mandatory before claiming done)
- `cmake --build build -j$(nproc)` — zero new warnings.
- `cmake --build build-tests --target dusk-studio-tests -j$(nproc) && ctest --test-dir build-tests --output-on-failure` — all pass. Add a slot/DSP test where feasible.
- **NEVER launch the binary on live Wayland** (it crashes) — run the self-test under Xvfb: `Xvfb :99 -screen 0 1920x1200x24 & env -u WAYLAND_DISPLAY DISPLAY=:99 DUSKSTUDIO_RUN_SELFTEST=1 timeout 90 ./build/DuskStudio_artefacts/Release/DuskStudio`.
- Prove an LV2 loads + processes in a strip (a targeted test or the self-test with a real bundle).
- Donor path: builds use `-DDUSK_PLUGINS_PATH=/home/marc/projects/plugins` (do NOT recreate `../plugins-main`).

## Constraints
- Small, reviewable commits (one per step). **Do not push; do not `git commit --amend` others' commits.** No `Co-Authored-By` trailers.
- Audio-thread code: no allocation, no locks, no logging (see CLAUDE.md "Audio thread rules").
- Load/unload is message-thread and must be fenced by the engine process gate (see how `AudioEngine` does it for CLAP).
