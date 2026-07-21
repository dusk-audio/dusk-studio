# De-JUCE tower spec — Device Phase-3-audio

**STATUS: P4 DONE (branch `dejuce/device-phase4-alsa` off main 6c7d569, commit
5365cec, NOT pushed). P0-P2 merged as #103 (091fcb5); P3 merged as its OWN PR
#104 (6c7d569) - Marc split the planned batched PR-B, so the `-a`/`-b` branches
are both retired and PR-B is now P4-P5 on the new branch. P4: AlsaAudioIODevice/
Type + AlsaPerformanceTest re-based onto device::IODevice/IODeviceType/
IODeviceCallback (String/Array->std::string/std::vector, BigInteger->ChannelSet,
HeapBlock/AudioBuffer scratch->std::vector + planar pointer arrays,
CriticalSection->std::mutex, high-res ticks->std::chrono, runSelfTest()->
std::string; dead rescan() deleted). Thread swap per §D5: std::thread, ioShouldExit
acquire-polled at the same five exit sites, ioExited (dusk::AutoResetEvent)
signalled as the thread's last statement; stop() = 2000 ms timed wait then join,
on timeout detach + abandon; the WHOLE device is leaked via
AlsaAudioIODevice::destroyOrPark (createDevice wraps devices in AlsaDeviceHandle
whose dtor routes there; an abandoned device refuses open/start, close releases
nothing). Thread body self-promotes via rt::applyRealtimeSchedRR - verified live
SCHED_RR rt-prio=9 -> kernel 89 under RLIMIT 95; "thread started" stderr line
kept (juce-prio label renamed rt-prio; nothing greps it). RT hunks proven
type-rename-only by normalized diff: recoverFromXrun + format table
byte-identical; rearm/configurePcm/converters/loop/prefill differ only in the
sanctioned type swaps; ioExited.signal() is the sole addition. DeviceManager.cpp
registers ALSA natively; JuceDeviceAdapter/JuceDeviceTypeAdapter/shim +
toBig/fromBig/toStrings/toVector + the JUCE include DELETED - the TU is JUCE-free
and the gate ratchet forced it OFF the allowlist NOW (the spec's "stays until P5"
was unsatisfiable: the gate fails clean-but-listed files; P5's allowlist step is
banked early). DeviceManagerJuce.cpp dropped its unreachable HAS_PIPEWIRE/
HAS_ALSA registration blocks. Tone test falls back to JUCE stock ALSA until P5.
Selftest backend cycle picks each type's default device explicitly (P2
no-auto-open drift reworked) and the restore reports its error. Allowlist
191->184 (6 alsa + DeviceManager.cpp). Build 0 NEW warnings (the
readInterleavedS24Packed sign-conversion warning is the pre-existing line,
verbatim from main). ctest 438/438 incl. NEW tests/alsa_thread_teardown.cpp
(wedged injected thread: stop() holds the 2000 ms window then detaches+abandons,
destroyOrPark leaks-not-frees, state valid through the thread's late touch;
clean thread: joins, freed normally). Xvfb selftest: ALSA pure-logic 15/15 +
PipeWire 8/8; backend-cycle open attempts fail on this box's environment
(built-in PipeWire sink delivers quantum 0; UMC kernel device held by WirePlumber
during the ALSA probe) - diagnostic text only, the restore reopens the UMC.
DUSKSTUDIO_RUN_ALSA_PERF matrix on real hw:UMC1820,0 (WirePlumber suspended):
44.1k+48k x 32..2048 duplex, all cells SAFE (one xrun at 44.1k/32), open/close
50 cycles SAFE, start/stop race 20 cycles SAFE, 34 SCHED_RR thread starts.
Next: P5 (consumer sweep + Linux CMake unlink + docs, §P5) on the SAME branch
dejuce/device-phase4-alsa; PR-B (P4-P5) prepared at P5 - Marc may merge P4 alone,
his call.
Resume: "P4 (ALSA re-type + thread swap) committed on dejuce/device-phase4-alsa
(5365cec), not pushed; start P5 (§P5) on the same branch".**
Update this line each session (phase done, branch, resume phrase).

Read order for an executing session: `docs/dejuce-campaign.md` → this file →
memory ledger `project_dejuce_roadmap.md` → the files of the ONE phase you are
about to execute. Execute one phase per session, then stop.

## Goal

Native PipeWire/ALSA backends implement the dusk `duskstudio::device`
interfaces (`IODevice`, `IODeviceType`, `IODeviceCallback`) directly.
`DeviceManager` on Linux drops its wrapped `juce::AudioDeviceManager` and the
adapter layer. `juce::juce_audio_devices` leaves the Linux link line. mac/win
keep the JUCE-wrapped path (CoreAudio/ASIO/WASAPI + JuceMidiBackend), mirroring
the native-MIDI-tower precedent.

**Decisions from Marc (2026-07-20):** code proceeds now; his hardware bench
validates the NEW native path and gates merge (one bench pass). Two PRs:
PR-A = P0–P2, PR-B = P3–P5; PR-B branch starts only after PR-A merges.

## Verified ground truth (do not re-derive; re-verify only what you touch)

1. **Only one RT callback consumer exists.** `deviceManager.addCallback()` is
   called exclusively by `AudioEngine` (AudioEngine.cpp:555/641/1048/1052);
   Bounce/Freeze detach and reattach the same engine. JUCE's multi-callback
   summing machinery runs with list size 1 here. We still replicate N-callback
   support (API allows it) but with pre-sized scratch, never resizing in the
   callback the way JUCE's `tempBuffer.setSize` does.
2. **Four stray includes are vestigial.** AudioEngine.h:3, BounceEngine.h:3,
   FreezeDialog.h:3, BounceDialog.h:3 include `<juce_audio_devices/...>` but
   reference no type from the module. Pure deletion.
3. **`type->rescan()` is dead code.** The UI path is
   `DeviceManager::scanAllDeviceTypes()` + `notifyChange()`
   (AudioSettingsPanel.cpp:682/689). JUCE's audioDeviceListChanged
   auto-close-on-vanished path never fires in practice; hot-unplug is detected
   via the ALSA fatal `-ENODEV` → `audioDeviceError` path and the H5 detector.
   Delete `rescan()` in P3/P4; do not build type-level change-listener
   plumbing into the native manager.
4. **Transitive link caveat (goes in both PR bodies).** `juce_audio_utils`
   stays linked on Linux (AudioThumbnail in MainComponent.h,
   AudioRegionEditor, MasteringView, AudioSettingsPanel.h) and its JUCE module
   declaration depends on `juce_audio_devices`, so the module keeps compiling
   transitively after we remove the direct link. The tower's honest
   deliverable: **direct link removed on Linux + zero `src/` translation
   units include the module's headers on Linux.** Object code physically
   leaves the binary when `juce_audio_utils` unlinks (hosting/GUI towers).
5. **Allowlist mechanics.** `JuceMidiBackend.{h,cpp}` set the precedent of
   *adding* a last-resort mac/win fallback file to `tools/juce-allowlist.txt`.
   `DeviceManagerJuce.cpp` follows (+1). The tower removes ~11 entries
   (pipewire ×4, alsa ×6, DeviceManager.cpp). **`DeviceManager.h` STAYS on
   the allowlist** — it keeps a `#if !defined(__linux__)`-gated `juce`
   forward declaration for the mac/win MIDI hatch. Do not try to clean it;
   it dies when the mac/win MIDI fallback dies.
6. **Legacy state format** (from JUCE-wayland juce_AudioDeviceManager.cpp
   ~L394-520, 997-1012): one `<DEVICESETUP deviceType= audioOutputDeviceName=
   audioInputDeviceName= audioDeviceRate= audioDeviceBufferSize=
   audioDeviceInChans= audioDeviceOutChans=/>` element. Channel masks are
   BigInteger binary strings, **MSB-first**; `useDefault*Channels` ⇔
   attribute absent. Trivially parseable without JUCE.
7. **`getStateBlob()` semantics**: empty until the user explicitly chooses,
   BUT `initialise(blob)` seeds `lastExplicitSettings` from a parsed blob, so
   a restored blob round-trips without a re-pick. AudioEngine.cpp:2391-2397
   depends on both halves. Replicate exactly.

## Design decisions (D1–D6)

### D1 — Sequencing: no big-bang flip

The existing `JuceDeviceAdapter`/`JuceDeviceTypeAdapter` already wrap dusk
interfaces *over* JUCE objects — exactly the direction needed to run
juce-typed backends under a dusk-native manager. Therefore:

- **P2**: DeviceManager (Linux) becomes a native orchestrator that owns
  `IODeviceType`s and drives `IODevice::open/start/stop/close` itself. The
  backends stay juce-typed, wrapped in *owning* adapters
  (`JuceDeviceTypeAdapter::createDevice` gains a real implementation
  returning an owning `JuceDeviceAdapter` whose `start(IODeviceCallback*)`
  installs a small juce-callback shim). The wrapped
  `juce::AudioDeviceManager` is deleted from Linux in this phase.
- **P3**: PipeWire backend re-bases onto the dusk interfaces; its adapter dies.
- **P4**: ALSA ditto; all adapters and the shim die.

Every phase boundary is a working state: each backend is registered through
exactly one live path, and the manager's public dusk API (hence
AudioEngine/UI) never changes.

### D2 — Native DeviceManager internals (Linux, DeviceManager.cpp)

- **Ownership**: `std::vector<std::unique_ptr<IODeviceType>> types`
  (stable pointers for the selector); `std::unique_ptr<IODevice>
  currentDevice`; `getCurrentDevice()` returns the raw pointer ("don't cache
  it" contract unchanged).
- **Callback fan-out** — `CallbackFanout`, an `IODeviceCallback` handed to
  `IODevice::start`:
  - `std::mutex callbackListLock` taken in the RT callback AND in add/remove.
    This is the identical locking discipline to JUCE's audioCallbackLock:
    uncontended pthread mutex per block; contention only during
    message-thread add/remove, which is exactly when JUCE also blocked the
    audio thread. No condition variable, so no notify-under-lock concern. No
    TSan suppression exists for the JUCE equivalent, so none is carried;
    watch the first TSan CI run and add `mutex:duskstudio::device::` entries
    only if a genuinely-benign report class appears (campaign ritual rule 7).
  - Dispatch parity: callbacks[0] writes device buffers directly;
    callbacks[1..] render into a pre-sized planar scratch
    (`std::vector<float>` + pointer array, sized numOutputChannels ×
    bufferSize in `audioDeviceAboutToStart`) and are summed in. Empty list ⇒
    zero outputs. Oversized block (defensive; PipeWire's over-quantum guard
    already drops those cycles): dispatch callbacks[0] only, skip the summed
    extras. Never allocate.
  - `audioDeviceAboutToStart`: synchronously clear `deviceChangePending`
    (release order) exactly as `CallbackBridge` does today
    (DeviceManager.cpp:145), then fan out.
  - `addCallback` while running: prime the newcomer with
    `audioDeviceAboutToStart(currentDevice)` BEFORE inserting into the list
    under the lock (JUCE's order — prevents a first block hitting an
    unprepared engine). `removeCallback` while running: remove under lock,
    then call `audioDeviceStopped()` on it. Keep today's idempotence guards.
  - `audioDeviceError`: forward to all clients; manager takes no additional
    action (engine handles teardown at AudioEngine.cpp:2314).
- **Device lifecycle** (message thread):
  - `openDeviceFromSetup(setup, treatAsChosen)`: stop (fan-out delivers
    `audioDeviceStopped`) → close → reset → resolve type by name →
    `type->createDevice(out, in)` → channel masks (`useDefault*Channels` ⇒
    first min(needed, deviceCount) channels, needed = the `initialise(16, 2)`
    arguments — JUCE parity) → `open(inMask, outMask, rate, buf)` → on
    success `device->start(&fanout)`, update `lastExplicitSettings` if
    treatAsChosen, broadcast. On failure propagate the error string; caller
    decides fallback.
  - Rate/buffer defaulting when setup carries 0: rate = 48000 if listed,
    else 44100, else largest listed rate ≥ 44100, else max available;
    buffer = `device->getDefaultBufferSize()`. Re-derived contract (JUCE
    prefers the 44.1–48k band); pinned by mock tests as re-baselined
    behaviour, not bit-parity.
  - `initialise(numIn, numOut, blob, selectDefaultOnFailure)`: register +
    scan backends (PipeWire first, ALSA second — preserve today's preference
    order); parse blob via `DeviceStateBlob` (D3); seed
    `lastExplicitSettings` from a successfully-parsed blob (ground truth 7);
    resolve saved type else first type that enumerates devices; open; on
    failure with selectDefaultOnFailure, open the type's default device with
    default masks WITHOUT clobbering `lastExplicitSettings` (saved intent
    must survive a busy device — `DeviceFallbackMessage` and
    `outputDeviceNameFromState` depend on it).
  - `setCurrentDeviceType`: arm `deviceChangePending` only for a
    will-actually-move request (same two-step check as today,
    DeviceManager.cpp:327-345), close the device, switch type, broadcast,
    leave the device null until the user picks — documented app contract and
    the H5 detector's assumption (AudioEngine.cpp:2411-2420). Do not
    auto-open.
  - `setSetup`: arm pending iff setup differs or device null (parity with
    today's :379-382). `closeDevice()`: arm iff device live.
- **Change notification**: internal broadcasts delivered async + coalesced on
  the message thread via `dusk::callAsync` with a pending flag — replicating
  JUCE ChangeBroadcaster timing that `onDeviceManagerChanged`'s defer logic
  was written against. `notifyChange()` stays synchronous (as today). Keep
  the exact snapshot-owners re-check loop from `fireListeners()`
  (DeviceManager.cpp:189-208).
- **Destructor ordering**: stop + close the device first (delivering
  `audioDeviceStopped`), then clear listeners WITHOUT firing, and never
  `callAsync` from the dtor (message pump is torn down after MainComponent;
  see the SIGSEGV history in DuskStudioApp.cpp:2028-2042 and
  MainComponent.h:254-258). AudioEngine's dtor already removes its callback
  first; the manager's own teardown must still be safe standalone.

### D3 — State blob: dusk JSON + one-way legacy migration

New clean unit `src/engine/device/DeviceStateBlob.{h,cpp}`:

- **New format** (always written on Linux):
  ```json
  { "version": 1, "deviceType": "PipeWire", "outputDevice": "…",
    "inputDevice": "…", "sampleRate": 48000.0, "bufferSize": 256,
    "outputChans": [0,1], "inputChans": [0,1] }
  ```
  Channel keys are explicit index arrays; key absent ⇔
  `useDefault*Channels = true` (preserves JUCE's presence semantics without
  MSB-first binary strings).
- **Reader**: sniff first non-space char — `{` ⇒ JSON via `dusk::json`; `<` ⇒
  legacy DEVICESETUP via a bespoke ~50-line attribute scanner (name="value"
  pairs in the first element; decode the five standard XML entities +
  numeric refs; parse mask strings MSB-first exactly as BigInteger's base-2
  parse does). No JUCE XML parsing anywhere in src/ — keeps the new files
  clean and lets the device/backend files leave the allowlist. Correctness
  pinned A/B in tests/ where JUCE IS linkable: same XML through the JUCE
  parser + BigInteger and through DeviceStateBlob ⇒ identical `DeviceSetup`.
- **Migration**: read-once fallback. Legacy XML parses to the same in-memory
  setup; the file converts to JSON the next time the engine persists
  (AudioEngine.cpp:2395-2397 writes on change broadcast when
  `lastExplicitSettings` is non-empty; `initialise` seeded it from the
  legacy blob, so conversion happens without a re-pick and selection can
  never be silently dropped). Unparseable blob ⇒ behave exactly like empty
  (fresh default), same as JUCE today.
- `outputDeviceNameFromState` handles both formats.
- mac/win keep the JUCE XML blob untouched in DeviceManagerJuce.cpp (state
  file is per-machine; no cross-OS migration exists).

### D4 — Non-Linux structure: two TUs, zero `#if` sprawl

- `DeviceManager.h`: API unchanged. The `juceManager()` hatch + gated `juce`
  forward-decl stay; header remains allowlisted (ground truth 5).
- `src/engine/device/DeviceManager.cpp`: native impl, CMake-gated Linux.
- `src/engine/device/DeviceManagerJuce.cpp` (new, allowlisted with a reason
  comment): today's wrapped impl moved essentially verbatim — adapters,
  CallbackBridge, wrapped manager, Windows ASIO/WASAPI/DirectSound factory
  registration (today's :243-259), XML state, `juceManager()`. Gated
  NOT-Linux in CMake. Byte-minimal diff = lowest risk for a path we cannot
  execute locally; macOS CI build + Windows CI tests are the compile proof
  (native-MIDI-tower precedent, PR #98).

### D5 — ALSA thread swap: JUCE Thread → std::thread (semantics to preserve)

- Members: `std::thread ioThread; std::atomic<bool> ioShouldExit{false};
  dusk::AutoResetEvent ioExited;` (AutoResetEvent already has TSan
  suppressions — reuse, add none).
- Start (today's AlsaAudioIODevice.cpp:812-857): thread body self-promotes
  as its first act via `rt::applyRealtimeSchedRR(...)` (RtPriority.h:53 —
  same map as the DSP worker pool, preserving the RR-fairness invariant at
  :813-817). If refused: plain SCHED_OTHER thread — JUCE's Priority::high is
  a no-op on Linux pthreads, so not a behaviour change. Keep the
  `[Dusk Studio/ALSA] thread started:` stderr line via
  `pthread_getschedparam(pthread_self())`.
- Exit-flag polling: `ioShouldExit.load(acquire)` substituted at EVERY
  existing threadShouldExit site — top of the while (:1003), after
  snd_pcm_wait (:1021, :1103), inside partial-read (:1033) and partial-write
  (:1113) retry loops. No site added, none removed.
- stopThread(2000) equivalent: set `ioShouldExit` (release); wait `ioExited`
  up to 2000 ms (thread signals it as its last statement); on success
  `join()`. On timeout (every blocking call in the loop is bounded ≤ 1 s, so
  healthy worst-case exit ≈ 1.2 s): log loudly, `detach()`, and move the
  **entire** AlsaAudioIODevice into a deliberately-leaked holder. The
  detached thread body still dereferences all of `this` — PCM handles, the
  mutex, the pre-sized scratch pointers, `ioShouldExit`, `ioExited` — so the
  whole owner must outlive the thread; parking only the PCM handles +
  interleave buffers would free the rest of the object under a live thread
  (use-after-free). The owner's destructor must therefore NEVER run while the
  thread is live: a stuck thread leaks the whole device rather than racing
  its teardown — strictly safer than JUCE's killThread. The engine's
  `callbacksInFlight` reconcile (AudioEngine.cpp:2346-2352) stays.
- Untouched by design (prove via diff review): recoverFromXrun / rearmStream
  (:884-950), prefill/start sequence (:760-810), format negotiation,
  interleave converters, `-ENODEV` fatal path → audioDeviceError
  (:1150-1160).
- Remaining JUCE in the TU is mechanical: String → std::string/dusk::text,
  StringArray/Array → std::vector, HeapBlock<char> → std::vector<char>,
  AudioBuffer<float> scratch → planar std::vector<float> + pointer array
  (pre-sized at open, as now), CriticalSection → std::mutex, BigInteger →
  ChannelSet.

### D6 — PipeWire re-type

No thread-model change (pw_thread_loop owns the RT thread). Mechanical: base
class swap to `device::IODevice`, BigInteger → ChannelSet, String/Array/
HeapBlock/CriticalSection swaps as in D5, `runSelfTest()` returns
std::string. RT invariants preserved verbatim: xrun rising-edge counter
(:727-737), over-quantum drop guard (:739-747), pre-zeroed outputs,
lock-then-dispatch (:765-770). `CallbackContext` continues to pass `{}` (no
host timestamp).

## Phase plan

One tower branch per PR. PR-A = P0–P2. PR-B = P3–P5, branched only after
PR-A merges (squash-merge hygiene: retire the PR-A branch).

### P0 — vestigial include sweep (mechanical)

- Files: `src/engine/AudioEngine.h`, `src/engine/BounceEngine.h`,
  `src/ui/FreezeDialog.h`, `src/ui/BounceDialog.h` — delete the
  `<juce_audio_devices/...>` include line in each.
- Gate: no allowlist movement (files keep other JUCE). Verify: app build
  zero warnings, ctest green.

### P1 — DeviceStateBlob unit + tests (mechanical, new code only)

- Files: NEW `src/engine/device/DeviceStateBlob.h/.cpp` (per D3); NEW
  `tests/device_state_blob.cpp`; `tests/CMakeLists.txt`.
- Test gate (narrow-link Catch2): JSON round-trip incl. absent-channel-keys
  ⇒ useDefault; legacy XML A/B vs the JUCE parser + BigInteger base-2 parse
  on: full attribute set, missing chans attrs, entity-laden device names
  (incl. UTF-8), MSB-first mask strings ("110" ≠ "011"), malformed input ⇒
  empty-equivalent; `outputDeviceNameFromState` on both formats incl.
  output-empty-falls-to-input.
- Not wired into the app yet — zero runtime risk.

### P2 — native DeviceManager on Linux (RT-RISKY: dispatch ownership moves)

- Files: `src/engine/device/DeviceManager.cpp` (native rewrite per D2, with
  temporary owning adapters per D1); NEW
  `src/engine/device/DeviceManagerJuce.cpp` (moved wrapped impl, allowlist
  +1); `DeviceManager.h` (comment truth-up only); `CMakeLists.txt` (gate the
  two TUs); `tests/CMakeLists.txt`; NEW `tests/device_manager_native.cpp`.
- The tone-test standalone manager in DuskStudioApp.cpp:301 survives until
  P5 — self-contained, still links.
- Test gate — the tower's centerpiece. Mock IODeviceType/IODevice recording
  open/start/stop/close order, driving synthetic blocks from a test thread:
  - initialise: empty blob ⇒ first-type-with-devices default; saved blob ⇒
    named device+type; busy saved device + selectDefaultOnFailure ⇒ fallback
    opens AND getStateBlob() still round-trips saved intent; blob empty
    until treatAsChosen.
  - setSetup: changed ⇒ stop→close→create→open→start ordering; unchanged ⇒
    no-op, no broadcast, pending untouched.
  - setCurrentDeviceType: unknown/current name ⇒ no arm; real switch ⇒
    device null + pending set; pending clears on next aboutToStart (sync)
    and on broadcast-with-live-device.
  - fan-out: add-while-running primes with aboutToStart before first block;
    remove-while-running gets stopped; two callbacks ⇒ summed output
    bit-exact vs computed reference; zero callbacks ⇒ zeroed output;
    audioDeviceError reaches all clients.
  - listeners: owner add/remove during fire (port the fireListeners comment
    scenarios); async-coalesced broadcast on the message thread (use the
    foundation message-thread pump pattern from existing tests).
- Verify: app build; ctest; Xvfb selftest 15/15; manual PipeWire and ALSA
  open/play/settings-change/backend-switch smoke on the dev box.
- Named bench debt: dispatch ownership under real hot-unplug + device-busy
  at boot.

### P3 — PipeWire backend onto dusk interfaces (mechanical re-type)

- Files: `src/engine/pipewire/PipeWireAudioIODevice.h/.cpp`,
  `PipeWireAudioIODeviceType.h/.cpp` (per D6; delete dead rescan());
  `src/engine/device/DeviceManager.cpp` (register the native type directly,
  drop its adapter); `src/engine/AudioPipelineSelfTest.cpp` (+.h if the
  runSelfTest signature is exposed); `tools/juce-allowlist.txt` (−4).
- Test gate: build + ctest + Xvfb selftest (backend-cycle section exercises
  a real open on the dev box's PipeWire). Optional
  `tests/pipewire_helpers.cpp` porting countActiveChannels/formatNodeLatency
  A/B out of the selftest. Diff-review assertion: onProcess body identical
  modulo type names.
- Bench debt: PipeWire streaming on hardware, SR/quantum swap.

### P4 — ALSA backend + thread swap (RT-RISKY: thread lifecycle)

- Files: `src/engine/alsa/AlsaAudioIODevice.h/.cpp` (per D5),
  `AlsaAudioIODeviceType.h/.cpp`, `AlsaPerformanceTest.h/.cpp` (dusk
  callback types; high-res ticks → std::chrono::steady_clock);
  `src/engine/device/DeviceManager.cpp` (delete adapter/shim machinery
  entirely); `src/DuskStudioApp.cpp` (perf-harness report strings);
  `src/engine/AudioPipelineSelfTest.cpp`; `tools/juce-allowlist.txt` (−6).
- AudioSettingsPanel's `setRequestedPeriods` static calls survive unchanged
  (signature already juce-free).
- Test gate: build/ctest/Xvfb selftest; ALSA selftest section (format
  round-trips, mask routing, periods clamp) green; forced-timeout
  destruction test (inject a thread body that ignores `ioShouldExit` past the
  2000 ms join, then destroy the device: assert the whole AlsaAudioIODevice
  is leaked-not-freed and its state stays valid until the thread finally
  signals `ioExited` — no use-after-free of the thread context) green;
  dev-box smoke: open duplex hw device; `DUSKSTUDIO_RUN_ALSA_PERF=1` matrix.
  Diff-review assertion: recovery/rearm/prefill/negotiation hunks unchanged.
- Bench debt (the big one): hot-unplug mid-play with std::thread teardown,
  xrun recovery under load, SR/buffer swap, timed-join timeout path.

### P5 — consumers, CMake unlink, docs (mechanical)

- Files: `src/DuskStudioApp.cpp` (Linux tone test over device::DeviceManager
  + a ~30-line sine IODeviceCallback replacing the JUCE test-sound path;
  non-Linux keeps the JUCE body gated — file is allowlisted);
  `CMakeLists.txt` (line ~1038: the juce_audio_devices link moves into a
  NOT-Linux block with the transitive-dep caveat comment);
  `tools/juce-allowlist.txt` (DeviceManager.cpp leaves; 194 → ~184);
  `docs/dejuce-campaign.md` (tower status + metric caveat); MANUAL.md check
  (expected no-op — nothing user-visible moves).
- Verify: full build; confirm the module's headers appear in no Linux src/
  TU (grep compile_commands.json) and state the transitive result honestly
  in the PR body; ctest; Xvfb selftest; gate script shows the shrink.

## Risk register

| # | Risk | Phase | Mitigation |
|---|------|-------|------------|
| 1 | Callback dispatch ownership moves off JUCE (aboutToStart ordering, add/remove-while-running, pending-flag clears) | P2 | Mock suite pins every ordering; prime-before-insert replicated; bench debt named |
| 2 | JUCE's lazy in-callback tempBuffer resize replaced by pre-sized scratch — divergence (an improvement) | P2 | Documented; oversized-block fallback dispatches callbacks[0] only |
| 3 | Change-broadcast timing: engine H5 defer logic assumes async | P2 | callAsync + coalesce; notifyChange() stays sync; H5 scenarios in mock tests |
| 4 | Teardown SIGSEGV class (manager dtor during shutdown) | P2 | Dtor closes device first, never fires listeners or callAsync |
| 5 | State migration silently losing device selection | P1/P2 | lastExplicitSettings seeded from legacy blob; fallback never clobbers it; A/B parse tests; unparseable ⇒ fresh default |
| 6 | ALSA std::thread has no killThread escape hatch; a detached thread must not outlive its owner (UAF on thread context) | P4 | Bounded blocking calls + timed join; on timeout detach + leak the **whole** device so the owner outlives the thread (dtor never races it); forced-timeout destruction test; engine reconcile stays |
| 7 | Xrun recovery / prefill / rearm regressions | P3/P4 | Logic frozen; enforced by diff review; bench debt named |
| 8 | mac/win path never executed locally | P2 | Byte-minimal move to DeviceManagerJuce.cpp; macOS CI build + Windows CI tests as compile proof |
| 9 | TSan new report classes (std::mutex in RT callback) | P2/P4 | No suppression carried (none exists for the JUCE primitive); CI-log-first per ritual rule 7 |
| 10 | Gate mechanics | P2/P5 | +1 DeviceManagerJuce.cpp (precedented), −11; DeviceManager.h stays listed — do not chase |
| 11 | Metric honesty: transitive juce_audio_utils dependency | P5 | Caveat in campaign doc + PR bodies; deliverable = direct link + zero includes on Linux |

## Owed to Marc's bench (gates merge; name in both PR descriptions)

- PipeWire streaming on real hardware; quantum/SR renegotiation.
- ALSA: hot-unplug mid-play (fatal device-loss path with the new thread),
  xrun recovery under load, buffer/SR swap, periods knob.
- Device-busy-at-boot fallback + alert copy (DeviceFallbackMessage) against
  the migrated blob.
- Legacy-XML → JSON conversion on a machine with a real saved DEVICESETUP.

## Opus session prompts

One fresh Opus session (`/model claude-opus-4-8[1m]`) per phase. Paste the
matching prompt. Each session: execute the ONE phase, verify, commit (never
push without Marc's go), update the memory ledger + this file's STATUS line,
end with a resume phrase.

Common preamble (prepend to every prompt):

> De-JUCE tower session, Dusk Studio repo. Read `docs/dejuce-campaign.md`
> (rules + ritual), then `docs/dejuce-device-phase3-audio-plan.md` (this
> tower's spec), then the memory ledger `project_dejuce_roadmap.md`. Follow
> the campaign ritual exactly: branch first, commit never push, no
> attribution trailers, AI-slop sweep before commit, gate script before
> commit, verification bar (app build zero warnings, ctest in build-tests,
> Xvfb-only selftest — NEVER launch the binary on live Wayland). Delegate
> wide scouting to Explore agents and bounded 1–2-file edits to cavecrew
> subagents. Escalate to Fable only for: an RT design question the spec
> doesn't answer, a stuck A/B test, a novel CI TSan report class. Execute
> ONLY the phase named below, then verify, commit, update the ledger and the
> spec STATUS line, and stop with a resume phrase.

- **P0 prompt**: "Phase P0 on a new branch `dejuce/device-phase3-audio-a`:
  delete the four vestigial juce_audio_devices includes (spec §P0). Re-verify
  each file really uses no type from the module before deleting. Build +
  ctest. One commit."
- **P1 prompt**: "Phase P1 on branch `dejuce/device-phase3-audio-a`:
  implement DeviceStateBlob per spec §D3/§P1 with the full A/B test matrix.
  Narrow-link test wiring per tests/CMakeLists.txt conventions. One commit."
- **P2 prompt**: "Phase P2 on branch `dejuce/device-phase3-audio-a`: native
  DeviceManager per spec §D1/§D2/§D4/§P2. Re-read DeviceManager.cpp,
  DeviceManager.h, AudioEngine.cpp callback/H5 sites before editing. Mock
  suite per §P2 is mandatory. Xvfb selftest + dev-box smoke. Commit; then
  prepare the PR-A description (bench-debt lines from §Owed, transitive-link
  caveat from ground truth 4) and stop — Marc reviews before push."
- **P3 prompt**: "Phase P3 on a new branch `dejuce/device-phase3-audio-b`
  (only after PR-A merged; verify main): PipeWire re-type per spec §D6/§P3.
  RT logic frozen — end with a diff review proving onProcess unchanged
  modulo types. One commit."
- **P4 prompt**: "Phase P4 on branch `dejuce/device-phase3-audio-b`: ALSA
  re-type + thread swap per spec §D5/§P4. Re-read AlsaAudioIODevice.cpp in
  full first. Diff review proving recovery/rearm/prefill/negotiation hunks
  unchanged. One commit."
- **P5 prompt**: "Phase P5 on branch `dejuce/device-phase3-audio-b`:
  consumer sweep + Linux unlink + docs per spec §P5. Verify zero
  juce_audio_devices includes in Linux src/ TUs via compile_commands.json.
  Prepare the PR-B description (bench debts + caveat). Update
  docs/dejuce-campaign.md tower status and the campaign metric caveat.
  Commit and stop."
