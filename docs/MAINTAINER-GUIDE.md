# Dusk Studio — Maintainer's Guide

A guide to understanding, building, debugging, and extending Dusk Studio. It assumes you can read C++ but does **not** assume you know JUCE, real-time audio, or this codebase. Work through Part 1 once, then keep Parts 2–6 open as reference.

> Companion docs:
> - [DuskStudio.md](../DuskStudio.md) — the product spec (the *why* and the *what*). ~1000 lines.
> - [MANUAL.md](../MANUAL.md) — the end-user manual (what each control does).
> - [README.md](../README.md), [BUILDING-LINUX.md](../BUILDING-LINUX.md), [BUILDING-WINDOWS.md](../BUILDING-WINDOWS.md) — build entry points.

---

## Part 0 — What you are actually maintaining

Dusk Studio is a **deliberately constrained, portastudio-style DAW** for Linux/macOS/Windows, written in **JUCE 8 / C++17**. It is one native desktop application — no server, no web component, no database. State lives in RAM (the `Session` object) and is serialized to a single `session.json` file plus a folder of WAV takes.

It is ~**85,000 lines** of C++ across `src/`, plus a large `CMakeLists.txt` (~990 lines) and 51 Catch2 test files. The DSP (EQ, compressors, tape) is **not** written here — it is shared header code pulled in from a sibling repo of Dusk Audio plugins.

The single most important mental model: **there are several threads, and the rules about what each may do are absolute.** Most bugs that look mysterious are thread-rule violations. Internalize Part 3 before you touch the audio path.

### The portastudio sensibility (the product's spine)

Dusk Studio is deliberately constrained. These aren't arbitrary limits to be "improved" away — they're the product:

- **24 channels.** Three banks of 8, mirroring a control surface.
- **Fixed signal chain.** HPF → EQ → comp → sends → pan → fader, in that order, on every strip. No reordering, no per-channel plugin chains beyond the insert slots.
- **Regions, not waveforms.** Move/split/trim/fade/delete — no sample-level destructive editing.
- **Minimal preferences.** The settings surface is the audio device panel plus a handful of adjacent config (MIDI bindings, sync). Resist adding options — pick a good default instead.

The touchstone is hardware like the Tascam DP-24: "would this exist on a standalone hardware recorder?" is the right instinct when judging a feature request, even though it's a sensibility rather than a law. Many "missing feature" requests are intentional omissions — check [DuskStudio.md](../DuskStudio.md) before assuming something was forgotten.

---

## Part 1 — The learning path (do this in order)

Here is the realistic ramp from zero to maintaining this codebase. Budget a few weeks of evenings. Each step has a concrete "you can do this now" checkpoint.

### Step 1 — Get it building and running (½ day)

Before reading any code, build it. You cannot learn a codebase you can't compile.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/DuskStudio_artefacts/Release/DuskStudio
```

JUCE and the Dusk plugins repo are auto-discovered from sibling directories (`../JUCE` / `../JUCE-wayland`, `../plugins` / `../plugins-main`). See Part 5 for what happens when discovery fails — it will, eventually, and the error messages are not always obvious.

**Checkpoint:** the app launches, you can create a track, arm it, and play a click.

### Step 2 — Run the tests and the self-test (½ day)

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

And the integration self-test (spins up the full audio engine and DSP, no GUI):

```bash
DUSKSTUDIO_RUN_SELFTEST=1 ./build/DuskStudio_artefacts/Release/DuskStudio
```

Read three small test files end to end — they are the cheapest way to see how a subsystem is *meant* to be called: [tests/session_round_trip.cpp](../tests/session_round_trip.cpp), [tests/transport_state_machine.cpp](../tests/transport_state_machine.cpp), [tests/smoke_brickwall_limiter.cpp](../tests/smoke_brickwall_limiter.cpp).

**Checkpoint:** all tests green; you understand the "narrow-link test" idea (each test compiles only the few `src/` files it exercises — see [tests/CMakeLists.txt](../tests/CMakeLists.txt)).

### Step 3 — Learn enough JUCE (1–2 weeks, in parallel with the rest)

You do not need all of JUCE. You need these pieces, in roughly this order:

| JUCE concept | Why it matters here | Where to see it in this repo |
|---|---|---|
| `Component`, `paint()`, `resized()` | Every UI widget is a `Component`. Layout is manual in `resized()`. | [src/ui/ChannelStripComponent.cpp](../src/ui/ChannelStripComponent.cpp) |
| `Timer` / `timerCallback()` | UI polls the engine ~30 Hz for meters; nothing pushes. | `ChannelStripComponent::timerCallback()` |
| `Slider`, `Button`, `Label`, `ComboBox` + `onValueChange` lambdas | How controls send values to the engine. | fader wiring in `ChannelStripComponent.cpp` (~line 759) |
| `LookAndFeel` | All custom drawing (faders, knobs, VU) routes through here. | [src/ui/DuskStudioLookAndFeel.h](../src/ui/DuskStudioLookAndFeel.h) |
| `AudioDeviceManager` + `AudioIODeviceCallback` | The audio entry point. | [src/engine/AudioEngine.cpp](../src/engine/AudioEngine.cpp) |
| `AudioBuffer<float>`, `dsp::ProcessSpec`, `dsp::IIR`, `dsp::Oversampling` | The DSP vocabulary. | [src/dsp/](../src/dsp/) |
| `AudioFormatWriter::ThreadedWriter` + `TimeSliceThread` | Lock-free disk recording. | [src/engine/RecordManager.cpp](../src/engine/RecordManager.cpp) |
| `AudioPluginFormatManager`, `AudioPluginInstance` | Hosting VST3/LV2/AU. | [src/engine/PluginManager.cpp](../src/engine/PluginManager.cpp), [src/engine/PluginSlot.cpp](../src/engine/PluginSlot.cpp) |
| `var` / `JSON` | Session save/load. | [src/session/SessionSerializer.cpp](../src/session/SessionSerializer.cpp) |
| `UndoableAction` / `UndoManager` | Region & marker edits. | [src/session/RegionEditActions.cpp](../src/session/RegionEditActions.cpp) |

Resources: the official JUCE tutorials (audio + GUI tracks), the JUCE class reference, and "The Audio Programmer" YouTube channel for the DSP modules. Build one toy JUCE app (a sine generator with a gain slider) so the `Component`/audio-callback split clicks before you stare at this codebase's 3000-line files.

**Checkpoint:** you can explain, without looking, why `paint()` must never read a file and why the audio callback must never call `new`.

### Step 4 — Learn enough real-time audio (read once, then it's reflex)

This is the part that has no shortcut. The audio thread is a hard-real-time deadline: it must fill the output buffer before the soundcard needs it (a few milliseconds), every time, forever. Miss once → an audible click ("xrun"). Every thread rule in this guide exists because of this. The short version:

On the audio thread you may **not**: allocate (`new`, `push_back`, `resize`, `std::string`, `juce::String`), lock a mutex, do file/network/log I/O, or call any UI/message-thread API. You communicate with other threads only through `std::atomic<T>` and lock-free FIFOs.

Read Part 3's thread table until you can recite it. Then read the actual callback (`AudioEngine::audioDeviceIOCallbackWithContext`, Part 3) and notice it obeys every rule.

### Step 5 — Trace one signal end-to-end (the keystone exercise)

When you can do this from memory, you can maintain the app. Trace a fader move:

1. User drags the channel fader → `Slider::onValueChange` lambda fires (message thread) → `track.strip.faderDb.store(db, relaxed)` writes an atomic in the `Session`.
2. Next audio block: `AudioEngine::audioDeviceIOCallbackWithContext` runs (audio thread). For that track it calls `ChannelStrip::processAndAccumulate(...)`.
3. `ChannelStrip` reads `faderDb` (lock-free), feeds a `SmoothedValue` (so the change ramps over ~20 ms, no click), applies gain, accumulates into the master mix buffer.
4. The master mix is copied to the device output; the strip writes a peak value into a meter atom.
5. `ChannelStripComponent::timerCallback` (message thread, 30 Hz) reads the meter atom and repaints the meter.

That round-trip — **UI writes atom → audio reads atom → audio writes meter atom → UI reads meter atom** — is the entire nervous system of the app. Every feature is a variation on it.

**Checkpoint:** you can point to the exact line in each of the five steps.

---

## Part 2 — Architecture map

### The four layers (and the dependency rule)

```
  src/ui/        ── Components, LookAndFeel, modals, editors      (knows about everything below)
  src/engine/    ── AudioEngine, transport, recording, playback,  (knows session + dsp)
                    plugin hosting/IPC, MIDI sync, control surfaces
  src/dsp/       ── ChannelStrip, BusStrip, MasterBus, limiters    (knows session params only)
  src/session/   ── Session data model + JSON serialize + edits    (knows nothing above it)
```

**Hard rule (enforced by code review):** `dsp/`, `engine/`, and `session/` never `#include` anything from `ui/`. The dependency arrow points one way: UI → engine → dsp → session. Break this and you'll create circular includes and untestable code.

### Where things live (the file map that matters)

| You want to change… | Start here |
|---|---|
| The audio signal flow / mixing order | [src/engine/AudioEngine.cpp](../src/engine/AudioEngine.cpp) — the callback |
| A channel strip's DSP (EQ/comp/fader/sends) | [src/dsp/ChannelStrip.cpp](../src/dsp/ChannelStrip.cpp) |
| A bus or master DSP | [src/dsp/BusStrip.cpp](../src/dsp/BusStrip.cpp), [src/dsp/MasterBus.cpp](../src/dsp/MasterBus.cpp) |
| The mastering chain (EQ/comp/limiter/LUFS) | [src/dsp/MasteringChain.cpp](../src/dsp/MasteringChain.cpp), [src/dsp/BrickwallLimiter.cpp](../src/dsp/BrickwallLimiter.cpp) |
| What gets saved/loaded | [src/session/Session.h](../src/session/Session.h) (the data) + [src/session/SessionSerializer.cpp](../src/session/SessionSerializer.cpp) (the JSON) |
| A mixer strip's look or controls | [src/ui/ChannelStripComponent.cpp](../src/ui/ChannelStripComponent.cpp) (5000 lines — the biggest file) |
| The overall window layout / view switching | [src/ui/MainComponent.cpp](../src/ui/MainComponent.cpp) |
| The arrangement / timeline / regions | [src/ui/TapeStrip.cpp](../src/ui/TapeStrip.cpp) |
| The MIDI piano-roll editor | [src/ui/PianoRollComponent.cpp](../src/ui/PianoRollComponent.cpp) |
| The audio region editor (fades/trim) | [src/ui/AudioRegionEditor.cpp](../src/ui/AudioRegionEditor.cpp) |
| Transport buttons / clock / tempo | [src/ui/TransportBar.cpp](../src/ui/TransportBar.cpp), [src/engine/Transport.h](../src/engine/Transport.h) |
| Recording to disk | [src/engine/RecordManager.cpp](../src/engine/RecordManager.cpp) |
| Playing regions back | [src/engine/PlaybackEngine.cpp](../src/engine/PlaybackEngine.cpp) |
| Plugin loading / scanning / crash isolation | [src/engine/PluginManager.cpp](../src/engine/PluginManager.cpp), [src/engine/PluginSlot.cpp](../src/engine/PluginSlot.cpp), [src/engine/ipc/](../src/engine/ipc/) |
| MCU / Mackie control surface | [src/engine/McuController.cpp](../src/engine/McuController.cpp), [src/engine/McuReceiver.cpp](../src/engine/McuReceiver.cpp) |
| MIDI clock / MTC sync | [src/engine/Midi*Emitter.cpp / Midi*Receiver.cpp](../src/engine/) |
| Custom fader/knob/VU drawing | [src/ui/DuskStudioLookAndFeel.h](../src/ui/DuskStudioLookAndFeel.h), [src/ui/AnalogVuMeter.cpp](../src/ui/AnalogVuMeter.cpp), [src/ui/SteppedKnob.h](../src/ui/SteppedKnob.h) |
| In-window modal dialogs | [src/ui/EmbeddedModal.h](../src/ui/EmbeddedModal.h) |
| App startup / lifecycle | [src/Main.cpp](../src/Main.cpp), [src/DuskStudioApp.cpp](../src/DuskStudioApp.cpp) |
| Build / dependency discovery | [CMakeLists.txt](../CMakeLists.txt) |

### Object ownership (who creates whom)

```
DuskStudioApp (JUCE app)              src/DuskStudioApp.cpp
  └── MainWindow
        └── MainComponent             src/ui/MainComponent.cpp
              ├── Session   session   ← the data model (plain object, owns all state)
              ├── AudioEngine engine{session}  ← takes a reference to the session
              ├── ConsoleView         → 24× ChannelStripComponent, 4× BusComponent, 1× MasterStripComponent
              ├── AuxView             → 4× AuxLaneComponent
              ├── MasteringView
              ├── TransportBar
              ├── TapeStrip
              └── (modals: PianoRoll, AudioRegionEditor, AudioSettings, plugin editors)
```

`MainComponent` owns both the `Session` and the `AudioEngine`, and hands references to both down to the view tree. The `AudioEngine` constructor **binds** each DSP strip to its matching parameter struct in the `Session` (see Part 3). Nothing is copied — the DSP holds references to the session's atomics and reads them live.

---

## Part 3 — The audio engine (the heart)

### The one function to understand: the audio callback

`AudioEngine::audioDeviceIOCallbackWithContext()` in [src/engine/AudioEngine.cpp](../src/engine/AudioEngine.cpp) is ~1000 lines and **runs on the audio thread, once per buffer (every few ms)**. Everything in Part 4 about DSP is in service of this function. Its order is the signal flow:

1. **Bail on empty / oversized blocks** — `numSamples == 0` early-returns; oversized host blocks are guarded. (JACK/PipeWire really do send these during transitions.)
2. **Drain MIDI inputs** — lock-free pull from each device's `MidiMessageCollector`.
3. **MIDI sync in/out** — chase incoming MIDI Clock/MTC if slaving; emit clock/MTC if mastering.
4. **MIDI controller bindings** — route learned CC/notes to fader/pan/mute/solo/plugin atoms.
5. **Automation routing** — per track, evaluate the automation lane at the playhead and publish `live*` atoms (the strip reads these, not the raw param, when in Read/Touch).
6. **Mastering-stage shortcut** — if the user is in the Mastering view, read the bounced stereo file → `MasteringChain` → out, and return early. The whole live mixer below is skipped.
7. **Clear mix/bus/aux buffers** (SIMD).
8. **Per-track loop (×24):** resolve input (playback file via `PlaybackEngine`, live device input, or silence) → `ChannelStrip::processAndAccumulate(...)` which runs HPF→EQ→comp→pan/fader→sends and **accumulates** into the master mix and any assigned bus/aux buffers. Records the input to disk if armed.
9. **Bus loop (×4):** `BusStrip::processInPlace` (EQ+comp), accumulate into master. Skipped if the bus is silent (cheap peak check first).
10. **Aux loop (×4):** `AuxLaneStrip` runs its one plugin, accumulates wet into master.
11. **Master:** `MasterBus::processInPlace` (tape sat → Pultec EQ → bus comp → fader → meter).
12. **Metronome** mixed in post-master.
13. **Recording write** — armed tracks push their input block to the threaded WAV writer (respecting punch/count-in).
14. **Output** — copy final stereo mix to device channels; measure callback time, flag xruns.

> If you change the order of operations here, you change the sound. Two rules in practice: bus/aux accumulate *after* the tracks that feed them, and the master runs *last*.

### How the engine binds to the session

In the `AudioEngine` constructor, each strip is bound to its parameter struct once:

```cpp
strips[i].bind(session.track(i).strip);     // ChannelStrip ← ChannelStripParams
busStrips[i].bind(session.bus(i).strip);    // BusStrip     ← BusParams
auxLaneStrips[i].bind(session.auxLane(i).params);
master.bind(session.master());
```

After binding, the audio thread reads parameters straight from the session's atomics with no further setup. The UI writes those atomics. There is no callback registration, no observer list — the strips are passive readers.

### The threads (memorize this table)

| Thread | Runs | May do | Must NOT do |
|---|---|---|---|
| **Audio** | the callback + all DSP | read/write `atomic`, lock-free FIFO push/pop, math | allocate, lock, I/O, log, touch UI |
| **Message (UI)** | all `Component` code, timers, file dialogs, save/load | everything normal | block on the audio thread |
| **MIDI input** | JUCE's handler → `MidiMessageCollector` | lock-free enqueue | heavy work |
| **Playback prefetch** | `TimeSliceThread` in `PlaybackEngine` | read WAVs ahead of the playhead | — |
| **Record disk** | `TimeSliceThread` in `RecordManager` | drain the write FIFO to disk | — |
| **MIDI out pump** | 1 ms loop in `AudioEngine` | drain the MIDI-out FIFO, call `sendBlockOfMessages` (it locks — that's why it can't run on the audio thread) | — |

Cross-thread state is *always* `std::atomic` or a lock-free FIFO. Metering atomics use `memory_order_relaxed`. Flags that gate audio reads of newly-published data (automation mode, swapped plugin pointer) use `release` on the writer and `acquire` on the audio reader. This is the single subtlest thing in the codebase — when in doubt, find an existing pattern (PluginSlot's swap, RecordManager's in-flight counter) and copy its ordering exactly.

### Transport & playhead

[src/engine/Transport.h](../src/engine/Transport.h) is a thin bag of atomics: `state` (Stopped/Playing/Recording), `playheadSamples`, loop in/out, punch in/out. The callback advances the playhead each block and handles loop wrap. The UI reads it via a 30 Hz timer to move the playhead line and update the clock.

### Recording & playback (lock-free disk I/O)

- **Recording:** `RecordManager` owns one `AudioFormatWriter::ThreadedWriter` per armed track. The audio thread only calls `write()` (lock-free push to a JUCE queue); a `TimeSliceThread` drains it to a WAV. On stop, the new WAV becomes an `AudioRegion` on the track.
- **Playback:** `PlaybackEngine` opens readers for every region ahead of time; the audio thread asks `readForTrack()` to sum all active regions at the playhead. A prefetch miss yields silence — it never blocks.

---

## Part 4 — The DSP layer

### The lifecycle every DSP class follows

`ChannelStrip`, `BusStrip`, `AuxLaneStrip`, `MasterBus`, `MasteringChain`, `PluginSlot` all share this shape:

- **`prepare(sampleRate, blockSize, ...)`** — cache the sample rate, `.prepare(spec)` every `juce::dsp` member, `.reset()` every `SmoothedValue`, size every scratch buffer. Must be idempotent (it gets called again on device change).
- **`bind(params)`** — stash a reference to the matching session param struct.
- **`processInPlace(L, R, n)`** or **`processAndAccumulate(...)`** — the audio-thread entry: update smoother targets at the top, run DSP, write meter atoms at the bottom.

### The fixed channel chain

Per channel: **HPF → 4-band EQ → compressor (Opto/FET/VCA) → sends → pan → bus assign → fader → mute/solo.** This order is fixed by product constraint #2 — it is not configurable, and that is the point.

### Where the actual EQ/comp/tape code lives (vendored DSP)

This is a gotcha that will confuse you the first time: **the EQ, compressor, and tape DSP are not in this repo.** They are header-only "cores" shared with the Dusk Audio plugins, pulled in from a sibling repo resolved at configure time (`-DDUSK_PLUGINS_PATH`, or `../plugins-main`, or `../plugins`). Classes like `UniversalCompressor`, `BritishEQProcessor`, `TubeEQProcessor`, and the TapeMachine processor come from there.

If `DUSK_PLUGINS_PATH` isn't found, the build defines `DUSKSTUDIO_HAS_DUSK_DSP=0` and you get a recorder with basic internal EQ and no comp/tape. So "where did the compressor go?" almost always means "the plugins repo wasn't discovered." Check the CMake configure output.

### The atomic-pointer pattern for vendored DSP (the one pattern to copy)

The vendored DSP exposes its parameters through a JUCE `AudioProcessorValueTreeState` (APVTS). Looking up a parameter by name (`getRawParameterValue("threshold")`) is a **string hash lookup — forbidden on the audio thread.** So the pattern is: cache the pointer **once** in `prepare`, write through it every block.

The reference implementation is `ChannelStrip::bindCompParams()` in [src/dsp/ChannelStrip.cpp](../src/dsp/ChannelStrip.cpp). Copy it verbatim for any new vendored processor:

1. In `prepare`, call `getRawParameterValue("name")` for each param and store the returned `std::atomic<float>*`.
2. Each block, read the session param and `storeAtom(cachedPtr, value)` — no string lookup, no allocation, no host notification.
3. The vendored `processBlock` reads the same atomics.

### Parameters: the two sources

1. **Session atomics** — plain `std::atomic<T>` members of the param structs in [src/session/Session.h](../src/session/Session.h) (`faderDb`, `eqLfGainDb`, …). UI stores, audio loads, both `relaxed`. **New audio params go here.**
2. **APVTS atoms in vendored DSP** — reached via the cached-pointer pattern above.

Metering uses `mutable std::atomic<float>` on the param struct so DSP holding a `const Params*` can still update meters.

### Oversampling — read this before touching any DSP

There is **one** oversampling control: the **Effect Oversampling** dropdown in Audio Device settings. The engine drives the chosen factor (1×/2×/4×) through every processor. Individual DSP units **must not** enable their own internal oversampling — doing so double-oversamples and wastes CPU. When you add a processor, wire it to read the engine-wide setting; never call its `setInternalOversamplingEnabled`.

---

## Part 5 — Building, testing, and the cross-OS setup

### The two build directories

- `build/` — the application.
- `build-tests/` — the Catch2 tests (configured with `-DDUSKSTUDIO_BUILD_TESTS=ON`). Keep it separate so the two CMake configs don't fight.

```bash
# app
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# tests
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

### Dependency discovery (the thing most likely to bite you)

CMake auto-detects two external repos at configure time. **Read the configure output** — it prints which paths it picked.

- **JUCE:** `-DJUCE_PATH=…` wins; else on Linux it prefers `../JUCE-wayland` (a plugdata-team fork with ~5 local commits Dusk Studio depends on — XEmbed, X11-on-Wayland fix, peer-creation latch), falling back to `../JUCE`; on macOS it uses `../JUCE` (upstream). The upstream-vs-fork API difference (`addDefaultFormatsToManager`) is hidden behind [src/engine/JuceCompat.h](../src/engine/JuceCompat.h) — call `duskstudio::juce_compat::addDefaultFormats(fm)` and never sprinkle `#ifdef __linux__` at call sites.
- **Dusk plugins:** `-DDUSK_PLUGINS_PATH=…` wins; else prefers `../plugins-main` (a git worktree pinned to the plugins repo's `main` so feature-branch work doesn't break the donor API), falling back to `../plugins`. Set it up once on Linux: `cd ../plugins && git worktree add ../plugins-main main`.

The cross-OS layout (development happens on macOS, Linux testing on a separate machine — both use the same build-dir names so switching machines never needs a reconfigure):

| OS | App | Tests | JUCE | Plugins |
|---|---|---|---|---|
| macOS | `build/` | `build-tests/` | `../JUCE` (upstream) | `../plugins` |
| Linux | `build/` | `build-tests/` | `../JUCE-wayland` (fork) | `../plugins-main` (worktree) |

### When to add a test

- Any non-trivial DSP change (express it as a buffer assertion: silence-in/silence-out, peak ≤ ceiling, unity gain, latency matches report).
- Any pure-logic change in `session/`, `engine/`, `dsp/` that doesn't need a live device or the Dusk DSP (region math, marker math, range conversions, smoother behavior).
- Every fixed bug: write the failing test first, then fix.

Don't test: UI components (no harness yet), anything needing a real audio device or the full DSP chain — those belong in `DUSKSTUDIO_RUN_SELFTEST=1`.

To add one: drop `tests/<unit>_<aspect>.cpp` following [tests/smoke_brickwall_limiter.cpp](../tests/smoke_brickwall_limiter.cpp), then add the `.cpp` **and every `src/…` it pulls in** to [tests/CMakeLists.txt](../tests/CMakeLists.txt) (narrow link — list only what's transitively reachable). `catch_discover_tests` registers each `TEST_CASE` with ctest automatically.

### Sanitizers (your best debugging friends for this kind of code)

```bash
cmake -S . -B build-asan -DDUSKSTUDIO_ENABLE_ASAN=ON  -DDUSKSTUDIO_BUILD_TESTS=ON   # use-after-free, overflow
cmake -S . -B build-tsan -DDUSKSTUDIO_ENABLE_TSAN=ON  -DDUSKSTUDIO_BUILD_TESTS=ON   # data races (mutually exclusive with ASan)
```

TSan is the one that catches "I forgot this cross-thread field should be atomic" — the most common real bug class here.

### CI

`.github/workflows/` builds Release on Linux (amd64 + arm64), macOS, and Windows, runs the Catch2 suite via ctest, and runs ASan/TSan variants on Linux. If CI is red and local is green, it's almost always a dependency-discovery flag difference (`DUSKSTUDIO_SKIP_FORK_CHECK`, explicit `JUCE_PATH`/`DUSK_PLUGINS_PATH`).

---

## Part 6 — Plugin hosting (the most complex subsystem)

You can support the whole app without ever touching this — but when a user says "loading X crashes Dusk Studio," this is where you go.

### In-process by default, out-of-process on request

VST3/LV2/AU plugins run **in-process by default**. Out-of-process hosting (a separate `dusk-studio-plugin-host` child per plugin) was tried as the default and rolled back: the cross-process editor path added UI latency, and on Linux/XWayland cross-process XEmbed is structurally unreliable (the compositor fights X11 reparenting). In-process gives instant, correct plugin editors at the lowest CPU cost; the trade-off is crash isolation — a misbehaving plugin can take the app down. Set `DUSKSTUDIO_USE_OOP_PLUGINS=1` to opt back into the sandbox (read once at startup; see `MainComponent`'s constructor).

**Plugin scanning is out-of-process regardless of that flag** (whenever the host binary is present): the child runs the scan, so a plugin that crashes or hangs while being probed gets a timeout and a blacklist entry, never a dead DAW. This is why the `src/engine/ipc/` directory exists and is fully maintained even though the in-process path is the runtime default.

### How parent and child talk

- **Audio hot path:** a shared-memory region (memfd on Linux, `shm_open` on macOS, `CreateFileMapping` on Windows) holds the audio buffers; the parent and child hand off via a cross-process futex / `WaitOnAddress` / `os_sync_wait_on_address` round-trip — no allocations, RT-safe. See [src/engine/ipc/platform/IpcShm*](../src/engine/ipc/platform/) and `IpcSync*`.
- **Control plane:** load/prepare/setState/release commands go over a socketpair (blocking is fine, it's the message thread). See `RemotePluginConnection` and `IpcChannel*`.
- **Scanning:** the child runs `--scan <format> <file>`, prints the plugin description wrapped in sentinels (`==DUSK_SCAN_BEGIN==`…`==DUSK_SCAN_END==`), the parent parses it. A crash/timeout blacklists the file. See [src/engine/ipc/PluginScanProtocol.h](../src/engine/ipc/PluginScanProtocol.h) and `OutOfProcessPluginScanner` in [src/engine/PluginManager.cpp](../src/engine/PluginManager.cpp).

### Loading a plugin into a channel (lock-free swap)

[src/engine/PluginSlot.cpp](../src/engine/PluginSlot.cpp): the message thread builds and prepares the new plugin off-thread, then atomically swaps a `std::atomic<AudioPluginInstance*>` (or the remote-connection equivalent). The audio thread reads it with `acquire`. The old instance is destroyed **only after** the swap (its destructor isn't RT-safe). Same pattern, audio side reads, message side mutates — see Part 3's memory-ordering note.

Each platform primitive has three implementations (`*_Linux.cpp`, `*_Mac.cpp`, `*_Windows.cpp`) behind a shared header. The round-trip is exercised without a real plugin by [tests/ipc_stub_round_trip.cpp](../tests/ipc_stub_round_trip.cpp) (spawns the child in `--ipc-stub` mode).

---

## Part 7 — The session data model & persistence

### Session.h is the source of truth

[src/session/Session.h](../src/session/Session.h) (~1080 lines) defines all state: 24 `Track`s (each with `ChannelStripParams`, regions, routing), 4 `BusParams`, 4 `AuxLaneParams`, `MasterBusParams`, `MasteringParams`, plus global tempo/time-sig/oversampling and solo/arm counters. Audio params are `std::atomic`. Key region structs: `AudioRegion` (WAV-on-disk: `timelineStart`, `lengthInSamples`, `sourceOffset`, fades, `previousTakes`) and `MidiRegion` (notes + CCs).

### Saving is crash-safe

[src/session/SessionSerializer.cpp](../src/session/SessionSerializer.cpp) writes JSON to a temp file, fsyncs, then atomically renames over `session.json`, so a crash mid-save never leaves a half-written session. There's a `kFormatVersion` and a `migrateSession()` path for forward-migrating older files; unknown keys are ignored so a newer field doesn't break an older reader. MIDI devices are saved by stable string identifier (not index) so a USB replug still resolves. Autosave and all save/load happen on the message thread only.

### Edits are undoable actions

Region and marker edits are `juce::UndoableAction` subclasses ([src/session/RegionEditActions.cpp](../src/session/RegionEditActions.cpp), [MarkerEditActions.cpp](../src/session/MarkerEditActions.cpp)): each captures before/after state so `undo()` restores exactly. Split/paste/trim/move/marker ops all go through the `UndoManager`.

---

## Part 8 — A debugging playbook

| Symptom | First suspects |
|---|---|
| Audible clicks / dropouts ("xruns") | Something allocating/locking/logging on the audio thread; a `SmoothedValue` not reset in `prepare`; buffer-size mismatch. Run TSan. Re-read the thread rules in Part 3. |
| Intermittent wrong value / flicker | A cross-thread field that should be `atomic` isn't, or wrong memory order. TSan. |
| "The EQ/comp/tape disappeared" | `DUSK_PLUGINS_PATH` not discovered → `DUSKSTUDIO_HAS_DUSK_DSP=0`. Check CMake configure output. |
| Build fails finding JUCE | Wrong sibling dir / fork vs upstream. Pass `-DJUCE_PATH=…` explicitly; check JuceCompat.h API split. |
| Loading one plugin crashes the app | Expected risk of the in-process default — try the same plugin with `DUSKSTUDIO_USE_OOP_PLUGINS=1` to confirm it's the plugin, then blacklist or sandbox it. If it crashes only in OOP mode, suspect the IPC swap / child lifecycle in `PluginSlot`/`ipc/`; reproduce with the stub test. |
| Plugin scan hangs / a plugin never appears | Scanner timeout/blacklist in `PluginManager.cpp`; check the sentinel parsing in `PluginScanProtocol`. |
| Session won't load / loses data | `SessionSerializer` migration path; a field added to `Session.h` but not (de)serialized. Add a round-trip test. |
| Meter frozen but audio plays | UI timer not running, or reading the wrong meter atom. |
| Crash on quit only | Plugin/IPC teardown order; note the Linux plugin-leak-on-shutdown is intentional — see `PluginSlot::leakInstanceForShutdown()` and its comments. |
| Wayland/X11 window weirdness on Linux | The JUCE-wayland fork commits; `PlatformWindowing_Linux.cpp`. |

General approach: reproduce in a test if at all possible (the suite runs in milliseconds), reach for ASan/TSan early, and when touching the audio path, re-read the thread rules *before* writing the fix, not after.

---

## Part 9 — Conventions you must follow (so future-you can read it)

- **C++17 only.** No `concepts`, `std::format`, or gratuitous `std::ranges`.
- **Naming:** `camelCase` methods/locals/members, `PascalCase` types, `kPascalCase` constants, `SCREAMING_SNAKE` macros only.
- **Headers:** `#pragma once`; include order JUCE → project-local → STL.
- **Comments:** default to none. Comment only a non-obvious *why* (a hidden invariant, a bug workaround). Don't narrate what the code does or reference tasks/PRs.
- **No backward-compat shims / dead code.** When something's gone, delete it.
- **No premature abstraction.** Three similar lines beat a speculative class. (But existing duplication that's already a problem *is* fair game to fix.)
- **Layer rule again:** `dsp/`/`engine/`/`session/` never include `ui/`.
- **Git:** small, reviewable commits at natural feature boundaries; no co-author trailers; never push without intending to; never force-push `main`.
- **Keep the manual in sync:** any user-visible change to `src/{ui,session,dsp,engine}` should be reflected in [MANUAL.md](../MANUAL.md).

---

## Quick reference card

```
BUILD APP        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
RUN              ./build/DuskStudio_artefacts/Release/DuskStudio
SELF-TEST        DUSKSTUDIO_RUN_SELFTEST=1 ./build/.../DuskStudio
BUILD TESTS      cmake -S . -B build-tests -DDUSKSTUDIO_BUILD_TESTS=ON && cmake --build build-tests --target dusk-studio-tests -j$(nproc)
RUN TESTS        ctest --test-dir build-tests --output-on-failure
ASAN / TSAN      -DDUSKSTUDIO_ENABLE_ASAN=ON  /  -DDUSKSTUDIO_ENABLE_TSAN=ON
OVERRIDE DEPS    -DJUCE_PATH=…  -DDUSK_PLUGINS_PATH=…

THE HEART        AudioEngine::audioDeviceIOCallbackWithContext()   src/engine/AudioEngine.cpp
THE DATA         src/session/Session.h
THE PATTERN      ChannelStrip::bindCompParams()                    src/dsp/ChannelStrip.cpp
THE NERVOUS SYS  UI writes atom → audio reads atom → audio writes meter atom → UI timer reads it
THE RULES        Part 3's thread table — no alloc / lock / I/O / UI on the audio thread
```

---

*Maintained by hand. When you change a subsystem, update the relevant part here — this document is only useful if it stays true.*
