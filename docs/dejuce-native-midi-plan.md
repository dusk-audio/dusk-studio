# Native ALSA-seq MIDI backend — executable spec (M1 → M3)

**Status: TOWER COMPLETE.** M1 (PR #93) primitives + backend interface, M2
(PR #94) `AlsaSeqMidi`, M3 (PR #98) the seam flip — all merged to main. Gate
194, suite 417/417, Xvfb self-test 15/15, ALSA loopback + migration suites
green on merged main.

What shipped: the seam runs on `IMidiInput/OutputBackend` (per-input
`dusk::MidiCollector`, SPSC slot queue + `std::thread` pump, `std::string`
device info); Linux uses `AlsaSeqMidi`, everything else `JuceMidiBackend`; both
device-API leaks outside the seam are routed through it; `juceManager()` is
compiled out on Linux; legacy JUCE identifiers migrate by re-resolving the old
`<client>-<port>` address.

Three things this tower did NOT do, which the next planner must not assume:

- **`juce_audio_devices` did not unlink.** Dual-gated with Phase-3-audio, so
  the gate stayed flat at 194 and the module count at 12. The metric moving is
  Phase-3-audio's job.
- **The mac/win `JuceMidiBackend` has never been executed.** It is not compiled
  on Linux; it was syntax-checked against real JUCE headers and reviewed, and
  its first genuine run is mac/win CI.
- **Hardware sign-off is still owed**: real-keyboard smoke, and MTC / MIDI-clock
  timing against external gear.

Known follow-on, designed but not built: MIDI hot-plug auto-detect. Nothing
calls `refreshMidiInputs()` automatically (only the Audio Settings Rescan
button), which predates this tower. Plan is to subscribe the input port to the
ALSA System Announce port and refresh when the transport is stopped.

The phase-by-phase spec below is kept as the record of what was built and why.

Read [dejuce-campaign.md](dejuce-campaign.md) first for the workflow ritual.
This tower replaces the JUCE backing of the MIDI device seam with a native
ALSA sequencer backend on Linux, keeping a JUCE fallback for mac/win, and
deletes the last `juceManager()` escape-hatch consumer.

**Honest scope caveat (goes in the PR description):** `juce_audio_devices`
does NOT unlink from this tower alone — the dusk `DeviceManager` still wraps
`juce::AudioDeviceManager` for audio until device Phase-3-audio lands. The
gate barely moves (seam files leave the allowlist, a JUCE-fallback TU joins
it). The value is the lower-risk half of the dual-gated unlink, done now.

## Surface map (verified 2026-07-17, main @ fd13132)

The seam is `src/engine/midi/MidiDevices.{h,cpp}` (~495 LOC). Its **API is
already dusk** (`MidiDeviceInfo`, `drainBlock`/`queueRt` take
`dusk::MidiBuffer`); JUCE is only the backing:

- **Input**: `juce::MidiInput::getAvailableDevices()` enum (MidiDevices.cpp:12);
  enable/callback via `juce::AudioDeviceManager`
  `{set,is}MidiInputDeviceEnabled` + `{add,remove}MidiInputDeviceCallback`
  (.cpp:27/28/58/71); one `juce::MidiMessageCollector` per input (MIDI
  thread → audio thread ring + timestamp retime); `handleIncomingMidiMessage`
  routes by source identifier (.cpp:100–115); `drainBlock` (audio RT) drains a
  collector through a reused `juce::MidiBuffer` scratch into the dusk buffer
  (.cpp:87–98).
- **Output**: `juce::MidiOutput` `{getAvailableDevices,openDevice,
  startBackgroundThread,sendBlockOfMessages}`; `juce::AbstractFifo` FIFO of
  64 × 4096-byte pre-sized slots; `juce::Thread` 1 ms pump (`Pump::run`,
  .cpp:299) drains under `bankMutex`; `queueRt` (audio RT) converts
  dusk → juce directly into a slot, whole-block-or-drop (.cpp:238–269).
- **VKB synthetic input**: fixed id `"Dusk Studio:virtual-keyboard"`
  (MidiDevices.h:34), appended after hardware enum so its index is stable;
  not bound to an OS device — the on-screen keyboard adds messages into its
  collector directly.

**Engine consumers** (AudioEngine.cpp): `midiIn.setDeviceManager
(deviceManager.juceManager())` at :531 — **the last juceManager() consumer in
the codebase**; detach/attach fence + rebuild around device changes
(:533/:550/:555/:604, :970–:975); `resolveByIdentifier` maps saved string ids
to indices (:563–:570, :911, :946); RT drain loop `midiIn.drainBlock` (:2602);
RT out `midiOut.queueRt` (:2879 clock/MTC mux, :4285 per-track routing);
message-thread `midiOut.send` (:379); pump lifecycle (:407, :1096);
`resetCollectors` (:2026).

**Two leaks outside the seam** (must route through it in M3):
1. `SessionSerializer.cpp:226,236` — calls `juce::MidiInput/MidiOutput::
   getAvailableDevices()` directly to resolve saved identifiers on load.
2. VKB path — `AudioEngine.h:166 getVirtualKeyboardCollector()` returns
   `juce::MidiMessageCollector*`; `VirtualKeyboardComponent.cpp:223,231`
   injects `juce::MidiMessage` into it.

**OUT OF SCOPE — do not pull in:** construction of `juce::MidiMessage` for
playback/sync/test content (AudioEngine.cpp ~:4016–:4206, DuskStudioApp.cpp
:422+, AudioPipelineSelfTest.cpp:1227, VirtualKeyboardComponent's message
*building*). That is MIDI content, not device I/O — it dies with the
events/GUI towers. Only the VKB *injection route* changes here.

**Template precedent:** the device tower's seam split —
`src/engine/device/DeviceManager.cpp` keeps Juce*Adapter classes in one
allowlisted TU while the header/API stays clean. M3 mirrors that shape.
Native ALSA audio (`src/engine/alsa/`) shows the present-or-stub CMake gating
pattern and already links `asound`. **`snd_seq_*` lives in that same
libasound — no new dependency.**

---

## M1 — additive primitives + backend interface (gate-neutral, no wiring)

New files only; nothing existing changes except tests/CMakeLists.txt. All new
headers must be juce-token-free (they are new clean files under the gate).

### 1a. `src/foundation/MidiRing.h` — `dusk::MidiRing`

SPSC lock-free ring of variable-length MIDI event records. One primitive
serves both directions: MIDI-thread producer → audio-thread consumer (input
collector) and audio-thread producer → pump-thread consumer (output queue).

- Fixed byte capacity set once off-RT (`reset(capacityBytes)` or ctor);
  **no allocation after that, ever** — both sides run on RT or RT-adjacent
  threads.
- Record = small header `{double timeMs; int32 numBytes}` + raw MIDI bytes.
  (`timeMs` doubles as the sample-position carrier where the caller wants
  one; keep it a plain double + int payload, no unions.)
- Single producer, single consumer: `head`/`tail` are
  `std::atomic<size_t>`, writer publishes with `release` after the bytes are
  in, reader consumes with `acquire` (same discipline as the engine's other
  RT flags). Wrap by two-segment memcpy — records may split across the
  physical end; do NOT require contiguity/padding.
- **Overflow policy: drop the whole incoming record**, return false. Matches
  the seam's existing drop-on-oversize policy (dropping clock bytes beats an
  xrun). Never grow. (Deliberate divergence from JUCE's collector, which
  grows its buffer under a mutex — that divergence is the point.)
- API sketch: `bool push(const uint8_t* bytes, int n, double timeMs)`;
  `template<class Fn> int drain(Fn&& fn)` calling `fn(bytes, n, timeMs)` per
  record until empty (consumer side); `void clear()` (consumer-side only,
  or document single-thread-only).

### 1b. `src/foundation/MidiCollector.h` — `dusk::MidiCollector`

Retiming collector on top of `MidiRing`, replacing
`juce::MidiMessageCollector`. Producer: backend MIDI thread. Consumer: audio
thread, once per block.

**JUCE's algorithm (read from JUCE source 2026-07-17,
`juce_MidiMessageCollector.cpp`) — replicate the semantics:**

- State: `sampleRate`, `lastCallbackTime` (ms, hi-res clock), pending event
  list with per-event sample numbers.
- `reset(sampleRate)`: clear pending, `lastCallbackTime = now`.
- `addMessage(bytes, n, timeStampSec)` (producer):
  `sampleNumber = int((timeStampSec − 0.001·lastCallbackTime) · sampleRate)`;
  append. Backlog trim: if `sampleNumber > sampleRate` (>1 s pending), drop
  events older than `sampleNumber − sampleRate`.
- `removeNextBlock(dusk::MidiBuffer& out, int numSamples)` (consumer):
  `msElapsed = now − lastCallbackTime; lastCallbackTime = now;`
  `numSourceSamples = max(1, round(msElapsed · 0.001 · sampleRate))`.
  - If `numSourceSamples > numSamples` (backlog longer than block): window to
    at most `numSamples << 5` source samples (skip events before
    `startSample = numSourceSamples − (numSamples<<5)`), then squeeze with
    integer math `scale = (numSamples << 10) / numSourceSamples`, event
    offset `= ((pos − startSample) · scale) >> 10`.
  - Else right-align: offset `= pos + (numSamples − numSourceSamples)`.
  - Clamp every offset to `[0, numSamples − 1]`; clear pending after drain.
- **Adaptation for dusk:** JUCE holds a mutex in add/remove — ours must not
  (that's the SPSC ring's job). Pending events between drains live *in the
  ring*; the retime math runs on the consumer side at drain time (producer
  stamps raw `timeMs` only). This reorders where the math happens but must
  preserve the observable semantics above. `lastCallbackTime`/`sampleRate`
  are consumer-side state (audio thread only) — no atomics needed for them.
- **Injectable clock** for determinism: drain takes `nowMs` as a parameter
  (`removeNextBlock(out, numSamples, nowMs)`); the seam passes the real
  hi-res clock, tests pass synthetic times. JUCE reads the real clock
  internally, so exact A/B against JUCE is timing-flaky — do NOT chase it
  (see tests below).

### 1c. `src/engine/midi/MidiBackend.h` — backend interfaces

JUCE-free header. `namespace duskstudio::midi`:

```cpp
struct BackendDeviceInfo { std::string name; std::string identifier; };

class IMidiInputBackend {
public:
    virtual ~IMidiInputBackend() = default;
    virtual std::vector<BackendDeviceInfo> enumerate() = 0;
    // Receiver fires on the backend's MIDI thread. deviceIdentifier matches
    // enumerate(); timeMs is a hi-res ms timestamp comparable to
    // MidiCollector's clock domain.
    using Receiver = std::function<void (const std::string& deviceIdentifier,
                                         const uint8_t* bytes, int numBytes,
                                         double timeMs)>;
    virtual void setReceiver (Receiver r) = 0;      // set once before start
    virtual bool enable (const std::string& identifier) = 0;
    virtual void disableAll() = 0;
    virtual void start() = 0;                       // attach fence
    virtual void stop() = 0;                        // detach fence: joins the
                                                    // dispatch side before
                                                    // returning (JUCE contract
                                                    // the seam relies on)
};

class IMidiOutputBackend {
public:
    virtual ~IMidiOutputBackend() = default;
    virtual std::vector<BackendDeviceInfo> enumerate() = 0;
    virtual bool open (const std::string& identifier) = 0;   // lazy, message thread
    virtual void closeAll() = 0;
    virtual bool isOpen (const std::string& identifier) const = 0;
    // Pump/message thread only; blocking allowed. baseTimeMs + sampleRate
    // carry the sample-offset->ms scheduling math (mirrors
    // sendBlockOfMessages semantics).
    virtual bool send (const std::string& identifier,
                       const dusk::MidiBuffer& events,
                       double baseTimeMs, double sampleRate) = 0;
};
```

Adjust freely where the seam wiring (M3) wants index-based calls instead of
identifier lookups — the seam owns index→identifier; keep the backend
identifier-keyed (indices are seam-order, backends must not care).

### 1d. Tests (`tests/CMakeLists.txt` narrow-link additions)

- `tests/midi_ring.cpp`: push/drain byte-exactness incl. sysex-sized records;
  wrap-around across the physical end (fill, drain, refill patterns);
  overflow → false + intact ring; interleaved producer/consumer from two
  std::threads with a total-bytes/ordering invariant (bounded loop, no
  sleeps); clear semantics.
- `tests/midi_collector.cpp`: deterministic with injected clock —
  steady-state offsets land where JUCE's math says (hand-computed cases);
  right-align path (`numSourceSamples < numSamples`); squeeze path
  (backlog > block, scale math, window at `numSamples<<5`); >1 s backlog trim;
  clamp bounds `[0, numSamples−1]`; smooth-clock property: monotonically
  timestamped stream drains with non-decreasing offsets across consecutive
  blocks. Optional loose sanity A/B vs `juce::MidiMessageCollector` (real
  clock, coarse tolerance) — only if it can be made non-flaky; skip otherwise
  and say so in the commit message.

**M1 done-when:** app builds untouched (no src changes outside new headers),
`ctest` green with the two new suites, gate count unchanged, no allowlist
edits.

---

## M2 — `AlsaSeqMidi` backend (Linux) + identifier scheme

New `src/engine/midi/AlsaSeqMidi.{h,cpp}` implementing both interfaces via
`snd_seq_*`. CMake: compile under the existing ALSA presence gate (same
`asound` link; add a `DUSKSTUDIO_HAS_ALSA_SEQ`-style define mirroring the
present-or-stub pattern if the audio-ALSA gate doesn't fit cleanly).

- One `snd_seq_t` handle per direction (or one shared — implementer's call;
  document the threading). Input: dedicated poll thread on
  `snd_seq_poll_descriptors`, decode with `snd_midi_event_t` into raw bytes,
  stamp `timeMs` from the same hi-res clock domain the collector drains
  against, invoke the Receiver. Output: `snd_midi_event_encode` +
  `snd_seq_event_output_direct` (blocking OK — called from the seam's pump).
- Sysex: `snd_midi_event_t` handles running status/sysex chunking — reset the
  coder appropriately between events (`snd_midi_event_reset_decode/encode`);
  test multi-chunk sysex explicitly.
- **Identifier scheme (the subtle correctness risk):** ALSA client *numbers*
  are not stable across reboot/replug. Identifier must be name-based, e.g.
  `"alsa-seq:<client name>:<port name>[:<dup-index>]"` (dup-index for
  same-named ports, ordinal within enumeration). Requirements:
  - Stable across reboot/replug for the same hardware.
  - Saved-session compat: Session stores string ids
    (Session.h:778–791 track midiInput/OutputIdentifier, :1388–93 MCU) and
    resolves via `resolveByIdentifier` string match (AudioEngine.cpp:563).
    JUCE-era saved ids will NOT match the new scheme → M3 must ship a
    migration match: when exact match fails, fall back to matching on device
    *name* (JUCE `MidiDeviceInfo.name` vs ALSA client/port name). Keep the
    fallback in the seam, not in Session.
  - VKB keeps its fixed id `"Dusk Studio:virtual-keyboard"` untouched (it is
    synthetic, backend-independent).
- **Headless loopback tests** (`tests/alsa_seq_midi.cpp`, the de-risk that
  audio-ALSA never had): `snd_seq_create_simple_port` in-process, subscribe
  our input to our own output port, send bytes → assert byte-exact receipt
  through the real backend path (enum → open → send → receive → decode).
  Cover: 3-byte channel messages, running-status bursts, multi-chunk sysex,
  enumeration finds the loopback port, identifier round-trip stability.
  Guard the suite to skip cleanly when `/dev/snd/seq` is absent (CI
  containers) — probe `snd_seq_open` and `SKIP()` on failure, never fail.

**M2 done-when:** backend + tests green locally (loopback suite passes on a
real machine, skips gracefully headless-CI), gate unchanged (new files clean
— keep ALSA includes but zero juce tokens), still nothing wired.

---

## M3 — flip the seam, route the leaks, delete the hatch

The behavior-risky phase; commit in reviewable steps on the tower branch.

1. **Seam re-plumb**: `MidiInputClient`/`MidiOutputBank` hold
   `IMidiInputBackend`/`IMidiOutputBackend` and per-input
   `dusk::MidiCollector`s + a `dusk::MidiRing`-based out-queue (replacing
   `juce::MidiMessageCollector`, `juce::AbstractFifo`, `juce::MidiOutput`,
   `juce::Thread` pump → `std::thread` + 1 ms cadence, keep the
   drop-don't-block policies and `bankMutex` roles exactly as documented in
   the current file's comments). Preserve: detach/rebuild/attach fence
   semantics, lazy output open (`ensureOpen` — eager-open stalls the message
   thread on USB-MIDI), stale-slot discard on rebuild, VKB appended last.
2. **Backend selection**: Linux → `AlsaSeqMidi`; else → `JuceMidiBackend` in
   its own TU (`src/engine/midi/JuceMidiBackend.{h,cpp}`, allowlisted),
   mirroring DeviceManager's Juce*Adapter split. `MidiDevices.{h,cpp}` drop
   all juce includes/tokens → **off the allowlist**; `MidiDeviceInfo`
   goes `std::string`. NOTE the JUCE fallback needs a
   `juce::AudioDeviceManager` for input enable — that reference now lives
   only inside the fallback TU; on Linux nothing needs it.
3. **Delete the hatch**: remove AudioEngine.cpp:531
   `midiIn.setDeviceManager(deviceManager.juceManager())` and the
   `juceManager()` method from `DeviceManager.{h,cpp}` — on Linux. If the
   mac/win JUCE fallback still needs the manager, gate the accessor into the
   fallback wiring path (`#if !defined(__linux__)` at the one call site) so
   Linux compiles it out entirely; `DeviceManager.h` goes clean only if the
   juce return type leaves the header (pimpl/void* is NOT wanted — prefer
   moving the accessor into a fallback-only header if needed).
4. **Leak 1**: SessionSerializer.cpp:226/236 → seam/backend enumeration
   (plumb an accessor; do NOT link the serializer against juce MIDI enum).
5. **Leak 2 (VKB)**: replace `getVirtualKeyboardCollector()`
   (AudioEngine.h:166) with a raw-byte post:
   `void postVirtualKeyboardMidi(const uint8_t* bytes, int n)` stamping the
   collector clock internally. VirtualKeyboardComponent builds its 3-byte
   note-on/off locally (it already knows note/vel/chan — trivial byte
   assembly, no juce::MidiMessage).
6. **Session-id migration fallback** per M2's scheme (name match when exact
   id match fails), in the seam's resolve path.

**M3 done-when:** app build 0 new warnings; ctest green (existing MIDI
round-trip suites now run through the new seam types unchanged — they are
dusk-typed already); gate: MidiDevices pair off allowlist, JuceMidiBackend
on, net ≤ 0 added; Xvfb selftest 15/15; real-keyboard smoke + **MTC/MIDI
clock timing vs external gear explicitly listed as owed to Marc's hardware
bench** in the PR description. TSan: new ring/pump primitives get
suppression-file parity review (campaign ritual item 7).

---

## Known traps (bitten before, in one place)

- `juce`-colon-colon token in any comment of a clean file fails the gate.
- Committing on main by accident — branch first, always.
- New sync primitive ⇒ TSan suppression carry-over + notify-under-lock.
- Multi-line `jlimit`-style reorders: use a local helper, don't hand-reorder.
- Test TUs that relied on transitive juce includes break when a header goes
  clean — add explicit includes on the test side.
- Stale test objects can mask header changes — force-rebuild tests after
  struct/field flips.
- MSVC: `M_PI` needs `_USE_MATH_DEFINES`; `%s` with std::string needs
  `.c_str()`.
- Don't launch the binary outside Xvfb. Ever.
- A Catch2 `SKIP()` costs a CI failure unless ctest is told: `catch_discover_tests`
  runs each case with an exact-name filter, so a skip-only run exits **4** and
  ctest scores it failed. Fix is a scoped
  `catch_discover_tests(<t> TEST_SPEC "[tag]" EXTRA_ARGS --allow-running-no-tests)`
  plus a second call taking `~[tag]`; don't apply the flag target-wide (it would
  also let an unmatched/stale filter pass silently), and don't use
  `SKIP_RETURN_CODE 4` (collides with a run that fails exactly 4 assertions).
- Environment-gated tests must probe what they actually need, not a proxy:
  `snd_seq_open` succeeding does not mean the sequencer can create or route
  ports, so the loopback suite probes a real send -> receive round-trip.
