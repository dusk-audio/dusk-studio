# AUX-first native CLAP plugin host — implementation plan

Decision: replace JUCE plugin hosting with our own native host, starting with the
**AUX section**, CLAP-based, then expand to channel strips + master. The goal that
drove this: plugin editors that display **instantly + flash-free on X11 and Wayland**,
which we guarantee by owning the editor create/cache/map lifecycle ourselves — no
JUCE off-screen-embed guesswork (which failed 3×).

## Why AUX first

- Smallest, most-contained host surface: one plugin slot per lane, send-effect role,
  4 lanes — vs 24 channel strips + master.
- It's where the felt pain is (the editor lag this whole thread chased).
- Proves the native host + the native editor embed on a small footprint before the
  big rollout. Coexists with JUCE hosting elsewhere during transition.

## Architecture — `src/engine/clap/`

- **ClapBundle** — `dlopen` a `.clap` shared object, read `clap_plugin_entry`, get the
  `clap_plugin_factory`, enumerate `clap_plugin_descriptor`s. Lifetime + unload.
- **ClapHost** — implements `clap_host` + the host-side extensions we provide:
  `log`, `thread-check`, `params`, `state`, `gui`, `posix-fd-support`, `timer-support`,
  `audio-ports`, `latency`. One per instance (or shared with per-instance context).
- **ClapInstance** — wraps a `clap_plugin`: `activate` / `start_processing` /
  `process` / `params` / `state`. Converts our audio buffers + MIDI ↔ `clap_process`
  / `clap_event_*`. Honors the existing RT process-gate + per-slot SpinLock
  (see [[project_multicore_dsp]]).
- **ClapEditor (Linux)** — the native X11 editor embed, the core win:
  `gui.create(api="x11", is_floating=false)` → create our **host X11 child window
  UNMAPPED** → `gui.set_parent(our window)` → `set_scale` / `get_size` → embed. Then:
  - **reveal** = `XMapWindow` + `gui.show()`; **hide** = `XUnmapWindow` + `gui.hide()`.
    Pre-created at plugin load, so reveal is instant.
  - **flash-free by construction**: host window created unmapped, plugin embeds while
    unmapped, we `XMapWindow` only on reveal — we control the exact order (no JUCE
    `WindowMapper`).
  - **XWayland**: force our editor X11 window onto XWayland (reuse the existing
    `preferX11ForNextNativeWindow` latch). CLAP has no Wayland embed (spec: "use
    floating") — same XWayland reality as every host.
  - **event pump**: register the plugin's fd via `clap_host_posix_fd_support` +
    timers via `clap_host_timer_support` into the JUCE message loop.

## Integration with the existing aux path

- `AuxLaneStrip` currently processes via the JUCE `PluginSlot`. Add a **`NativeClapSlot`**
  alternative, selected behind a flag (`DUSKSTUDIO_AUX_NATIVE_CLAP`) so JUCE + native
  coexist during transition; the audio thread calls one or the other.
- `AuxLaneComponent` hosts the `ClapEditor` component (our X11 host window wrapped in a
  JUCE `Component`) instead of the JUCE plugin editor when the lane's plugin is CLAP.
- Reuse what transfers: the OOP scanner shell, the X11 error handler, the preferX11
  latch, the session plugin-state plumbing.

## DuskVerb as CLAP

Build the Dusk plugins (convolution-reverb = **DuskVerb**, etc.) as **CLAP** targets in
`plugins-main` (clap entry or clap-wrapper). You own these, so the whole aux path —
load, process, editor — can be native CLAP end-to-end. 3rd-party VST3/LV2 stay on JUCE
until native VST3/LV2 land.

## Staged increments (each independently buildable + verifiable)

| # | Increment | Verify | Status |
|---|---|---|---|
| **0** | Foundation: vendor the CLAP C API as a git submodule at `external/clap` (`free-audio/clap`), `src/engine/clap/` skeleton, `ClapBundle` load + enumerate. | Catch2: load a `.clap`, list plugins. | ✅ `ba9ff67` |
| **1** | `ClapInstance` audio: load + activate + `process` offline. | Catch2 A/B: silence→silence, a gain/known plugin matches reference. | ✅ `7738d73` (vs DuskVerb) |
| **2** | `ClapEditor` native X11 embed: create/set_parent/map/unmap + fd/timer pump, pre-create + cache. | **You, live Wayland**: instant first open, no flash, correct render/resize/close. | ✅ `667fa02` — renders DuskVerb under Xvfb; **live-Wayland confirm pending** |
| **3a** | `NativeClapSlot`: aux plugin slot owning bundle+instance, lock-free `ready` gate. | Catch2: load→process→unload. | ✅ `8b74136` |
| **3b** | Route `AuxLaneStrip`'s plugin insert through `NativeClapSlot` when loaded (else JUCE slot). Inert by default. | Builds; aux byte-identical with no native CLAP. | ✅ `6b2ee90` |
| **3c** | UI: aux picker selects a `.clap`; `AuxLaneComponent` reveals the `ClapEditor` on tab switch (instant). Session persists+restores the lane's native CLAP. | **You, live Wayland**: pick → load → instant editor; save/reload. | ⏳ held — UI, needs live verify |
| **4** | Scanning + state + params: `ClapScanner` (`.clap` discovery), `ClapInstance` state save/load, param enumerate/read + automate via a per-block CLAP event list. | Catch2 scan + state round-trip + param-change applied. | ✅ scan `3bba3e5`, state `1d452dc`, params `ce0272a` (+hardening `2f1ff3e`) |
| **RT** | Empty event lists moved off function-statics → members (no audio-thread static-init guard). | Catch2 still processes DuskVerb. | ✅ `bcc3100` |
| **5** | Hardening; flip `DUSKSTUDIO_AUX_NATIVE_CLAP` default on for aux. | Soak + your live sign-off. | ⏳ |

Later (separate efforts): channel strips + master; native VST3 (`IRunLoop` +
`IPlugView`) + LV2 (`lilv`/`suil`) for 3rd-party; full JUCE-hosting removal.

### Next: 3c wiring (needs live-Wayland verify — held while away)

The non-UI foundation is done and unit-verified. What remains is glue that drives
the X11 embed, so it must be checked on the live compositor:

1. **Picker** — surface `ClapScanner::scan()` results in the aux plugin picker
   (`AuxLaneComponent` / `PluginPickerHelpers`). On choose: `auxLane.loadNativeClap(slot, file)`.
2. **Editor** — when a lane's `NativeClapSlot::isLoaded()`, host a `ClapEditor`
   (via `ClapPluginEditorComponent`) in `AuxLaneComponent` instead of the JUCE
   editor; **reveal on tab switch** (pre-created + `XMapWindow`) for the instant open.
3. **Session** — persist `{ native_clap_path, base64(state) }` per aux slot in
   `SessionSerializer`; on load, `loadNativeClap` then `loadState`. Mechanics exist
   (`NativeClapSlot::saveState/loadState`); only the serializer fields + restore call remain.

API already in place: `AuxLaneStrip::{loadNativeClap,unloadNativeClap,isNativeClapLoaded,getNativeClapSlot}`,
`NativeClapSlot::{saveState,loadState,getInstance,paramCount,paramInfo,getParamValue,setParamValue}`,
`ClapScanner::scan`. Params: `setParamValue` (message thread, single producer) queues a change applied on
the next audio block — wire MIDI-map / automation through it.

## What stays JUCE during transition

App shell, audio device, DSP, UI framework; VST3/LV2/AU hosting (until native lands);
channel-strip + master plugin hosting (until the aux model is proven + ported). Never
without working plugins mid-migration.

## Verification protocol

- Audio increments (0,1,4) are **offline-unit-testable** — Catch2, no device, no
  compositor. I can fully verify these.
- Editor increments (2,3,5) need **live-Wayland** verification — I have no compositor
  (Xvfb), so YOU run them and I provide a checklist per round (instant first open, no
  flash, render, resize, DPI/scale, close, re-open). This is the part I cannot self-validate.

## First step

Increment 0 — additive, safe, doesn't touch the working app: vendor the CLAP C API
as a git submodule at `external/clap`, scaffold `src/engine/clap/ClapBundle`, and a
Catch2 test that loads a `.clap` and lists its plugins. Proves the foundation + the
toolchain.
