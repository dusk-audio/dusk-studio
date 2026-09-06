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
cmake --build build -j6
./build/DuskStudio_artefacts/Release/DuskStudio
```

JUCE, the Dusk plugins repo, and the DAF stack behind the native notepad are auto-discovered from sibling directories (`../JUCE` / `../JUCE-wayland`, `../plugins`, `../DAF`, `../DAF-Widgets`). See Part 5 for what happens when discovery fails — it will, eventually, and the error messages are not always obvious.

**Checkpoint:** the app launches, you can create a track, arm it, and play a click.

### Step 2 — Run the tests and the self-test (½ day)

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j6
ctest --test-dir build-tests --output-on-failure
```

And the integration self-test, isolated from the live Wayland session on a
private Xvfb display:

```bash
scripts/run-selftest-xvfb.sh
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

This is a gotcha that will confuse you the first time: **the EQ, compressor, and tape DSP are not in this repo.** They are header-only "cores" shared with the Dusk Audio plugins, pulled in from a sibling repo resolved at configure time (`-DDUSK_PLUGINS_PATH`, else `../plugins`). Classes like `UniversalCompressor`, `BritishEQProcessor`, `TubeEQProcessor`, and the TapeMachine processor come from there.

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
cmake --build build -j6

# tests
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j6
ctest --test-dir build-tests --output-on-failure
```

### Dependency discovery (the thing most likely to bite you)

CMake auto-detects four external repos at configure time, on top of three git submodules. **Read the configure output** — it prints which paths it picked.

- **Submodules** (`external/clap`, `external/sfizz`, `external/vst3sdk`): clone with `--recurse-submodules`, or run `git submodule update --init --recursive`. They fail in three different ways, which is worth knowing before you debug the wrong one. Missing `external/clap` is fatal — the native CLAP host defaults ON on Linux, macOS, and Windows, and the configure stops with a "CLAP headers missing" error. Missing `external/vst3sdk` is loud but survivable: a STATUS line, native VST3 disabled, unless you explicitly asked for `-DDUSKSTUDIO_NATIVE_VST3=ON`, which turns it fatal. Missing `external/sfizz` says **nothing at all** — the block is wrapped in a bare `EXISTS` test, so SF2 / multisample support simply isn't in the binary.

- **JUCE:** `-DJUCE_PATH=…` wins; else on Linux it prefers `../JUCE-wayland` (a plugdata-team fork with ~5 local commits Dusk Studio depends on — XEmbed, X11-on-Wayland fix, peer-creation latch), falling back to `../JUCE`; on macOS it uses `../JUCE` (upstream). The upstream-vs-fork API difference (`addDefaultFormatsToManager`) is hidden behind [src/engine/JuceCompat.h](../src/engine/JuceCompat.h) — call `duskstudio::juce_compat::addDefaultFormats(fm)` and never sprinkle `#ifdef __linux__` at call sites.
- **Dusk plugins:** `-DDUSK_PLUGINS_PATH=…` wins; else `../plugins`, and that is the whole list. Check out the `DONOR_REV` shared by the build and release workflows so every build uses the same DSP and layout. Missing entirely, configure only *warns*: you get a recorder with no EQ, comp, or tape rather than a failed build, so read the configure output.
- **DAF + DAF-Widgets** (the native UI: notepad, startup dialog, compressor editor, virtual keyboard, audio settings): `-DDAF_PATH=…` / `-DDAF_WIDGETS_PATH=…` win; else `../DAF` and `../DAF-Widgets`, then the `external/` fallbacks, which are placeholders for eventual release pinning and are not populated today. The two checks are ANDed, so missing *either* one quietly defaults `DUSKSTUDIO_ENABLE_NATIVE_UI` to OFF, announced by one easy-to-miss STATUS line (`Native UI: DAF / DAF-Widgets not found - disabled`); at runtime every native view is gone - the notepad reports *"Notepad unavailable: built without the native notepad UI"*, the compressor editor, the virtual keyboard and the audio settings panel say the same of themselves, and the startup dialog simply does not appear. Forcing `-DDUSKSTUDIO_ENABLE_NATIVE_UI=ON` without them is a configure error rather than a silent downgrade. Clone the Dusk-owned forks at the revisions CI pins — [.github/actions/clone-daf-stack/action.yml](../.github/actions/clone-daf-stack/action.yml) is the single source of truth for those pins and verifies that DAF uses the Dusk Pugl fork:

```bash
cd /path/to/dusk-studio

git clone https://github.com/dusk-audio/DAF.git ../DAF
git -C ../DAF checkout 50ad8c22a2f05b85be4b40e473d830d2dc91c2c2
git -C ../DAF submodule update --init     # dgl/src/pugl-upstream

git clone https://github.com/dusk-audio/DAF-Widgets.git ../DAF-Widgets
git -C ../DAF-Widgets checkout 798154e874eaaa024371f6076249398b51498142
```

Clone then check out the SHA rather than cloning a moving branch. Both pins are
on their forks' `main` histories today, but exact revisions keep local and CI
builds reproducible as those branches advance.

DAF's Pugl revision is carried by the Pugl branch `dusk-pin-5e2621d`, not that
fork's `main`. Keep that branch too: DAF submodule initialization and CI require
the pinned commit to remain reachable.

`DUSKSTUDIO_ENABLE_NATIVE_UI` is a cached `option()`, which makes the OFF sticky in a nasty way: configure a build dir before the checkouts exist, add them later, and re-running CMake in that same dir leaves the notepad off — and the STATUS line above no longer prints, because its guard also requires the deps to be missing. Use a fresh build dir after cloning, or pass `-DDUSKSTUDIO_ENABLE_NATIVE_UI=ON` to overwrite the cache entry.

The cross-OS layout (development happens on macOS, Linux testing on a separate machine — both use the same build-dir names so switching machines never needs a reconfigure):

| OS | App | Tests | JUCE | Plugins |
|---|---|---|---|---|
| macOS | `build/` | `build-tests/` | `../JUCE` (upstream) | `../plugins` |
| Linux | `build/` | `build-tests/` | `../JUCE-wayland` (fork) | `../plugins` |

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

- **Audio hot path:** a shared-memory region (memfd on Linux, `shm_open` on macOS, `CreateFileMapping` on Windows) holds the audio buffers; the parent and child hand off via a cross-process futex / non-blocking pipe / kernel-event round-trip — no allocations, RT-safe. See [src/engine/ipc/platform/IpcShm*](../src/engine/ipc/platform/) and `IpcSync*`.
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

## Part 9b - Regression run across platforms

After a fix wave, `scripts/regress.sh` re-verifies the tree on all three target
platforms from the Linux box. Each platform runs a list of *legs*; every leg
prints one `PASS` / `FAIL` / `WARN` / `SKIP` line, the run ends with a summary
table, and the script exits non-zero if any leg failed. Re-running is safe: no
leg prompts, and each one either recreates its inputs or checks them.

```bash
scripts/regress.sh                       # linux (the default target)
scripts/regress.sh linux --perf          # plus the headless engine perf suite
scripts/regress.sh linux --vst3 ~/.vst3/Multi-Q.vst3
scripts/regress.sh linux --scenarios-only
scripts/regress.sh mac
scripts/regress.sh windows --msi /path/to/dusk-studio-X.Y.Z-Windows-x64.msi
scripts/regress.sh windows --release-run 1234567890
scripts/regress.sh all --perf --msi /path/to/installer.msi
```

`all` routes each option to the platform that owns it, so one command line can
carry Linux, macOS and Windows options at once. An option no platform claims is
a usage error rather than a silently ignored word.

Layout: `scripts/regress.sh` only dispatches and routes options. The work is in
`scripts/regress/{linux,mac,windows}.sh` over the shared leg bookkeeping in
`scripts/regress/common.sh`, the private-display plumbing in
`scripts/regress/xvfb.sh` and the scenario legs in
`scripts/regress/scenarios.sh`, plus the guest-side helpers in
`scripts/regress/windows/`.

### Linux

Prerequisites: `build/` and `build-tests/` already configured, `Xvfb`, GNU
`timeout`, `flock`, and the pinned donor checkout at `../dusk-donor-pin`.

| Leg | What it proves |
|---|---|
| `configure-check` | both build dirs point `DUSK_PLUGINS_PATH` at `../dusk-donor-pin`. A drifted `../plugins` changes the DSP under test, and the failure then reads as a Dusk Studio regression. A mismatch aborts the run before anything is built. |
| `build-app` / `build-tests` | both targets compile at `-j6`. |
| `ctest` | the Catch2 suite in `build-tests/`. |
| `juce-gate` | `tools/juce-gate.sh`: no file gained JUCE and no listed file gained occurrences. |
| `selftest-xvfb` | `scripts/run-selftest-xvfb.sh` - the headless audio self-test on a private X display. |
| `ipc-selftest` | `DUSKSTUDIO_RUN_IPC_SELFTEST=1`: the shm + futex round-trip against the `dusk-studio-plugin-host` stub. |
| `ipc-host-test` | `DUSKSTUDIO_IPC_HOST_TEST=<plugin>`: a real plugin loaded out-of-process, 1000 stereo blocks, signal asserted modified. Uses `--vst3`, else the first `~/.vst3/*.vst3`; `SKIP` when there is none. |
| `perf-suite` | `DUSKSTUDIO_RUN_PERF_TEST=1` across the (rate, buffer, load) matrix. Off unless `--perf`. |
| `scenarios-headless` | `DUSKSTUDIO_RUN_SCENARIOS=all`: the in-app scenario suite. Passes only on exit 0, no `[FAIL]` line, and the terminal `=== scenarios: ` summary - a crash after the last case must not pass on a lucky exit code. Skipped cases go into the leg's note. |
| `bb-handoff`, `bb-crash-relaunch`, `bb-no-runtime-dir`, `bb-damaged-recent`, `bb-clean-quit`, `bb-quit-twice`, `bb-oop-child-kill`, `bb-oop-quit-during-load` | the black-box legs: real app processes spawned, killed and read back through their stderr. See "Scenario legs" below. |
| `scenarios-gui` | `DUSKSTUDIO_RUN_SCENARIOS=gui`: the plugin-editor scenarios that need a window. Off unless `--gui-scenarios` - it is the slowest leg and the most sensitive to GLX under Xvfb. |

`--no-scenarios` leaves the scenario legs out of the run entirely.
`--scenarios-only` runs nothing but them, against whatever binary is already in
`build/`; the compile and self-test legs are reported as `SKIP` so the table
still says what was not run. `build-tests/` has to exist either way: it is half
of `DUSKSTUDIO_FIXTURE_DIR`, which is how the app binary finds the test
fixtures.

Everything from `selftest-xvfb` down runs on a private Xvfb display with
`WAYLAND_DISPLAY` unset - the binary aborts against a live Wayland session, so
no leg may ever launch it on the desktop.

`DUSK_REGRESS_BUILD_LOCK=/path/to/lockfile` wraps the two compile legs in
`flock` when something else may be building the same tree.

#### Scenario legs

The `bb-*` legs run several real app processes at once on one shared Xvfb
display, so each leg gets a throwaway directory and a private environment:

- Private `XDG_RUNTIME_DIR`, mode 0700. `makeSocketPath` in
  [src/util/SingleInstance.cpp](../src/util/SingleInstance.cpp) keys the
  single-instance slot on `$XDG_RUNTIME_DIR/dusk-studio/instance-<hash of
  DISPLAY>.sock`, so a per-leg runtime dir is what stops a leg handing a session
  to - or stealing one from - the copy of Dusk Studio you have open.
- Private `HOME` and `XDG_CONFIG_HOME`, mode 0700. `dusk::fs::userConfigDir()`
  resolves `$HOME/.config` and does **not** read `XDG_CONFIG_HOME`, so only a
  private `HOME` keeps Recent Sessions, `app-config.properties` and crash logs
  out of your profile. It also means the legs start with no plugin cache, which
  is why they start in well under a second.
- A private copy of `scripts/regress/sessions/minimal/session.json` per leg. The
  checked-in file is never loaded in place: an autosave tick would write
  `session.json.autosave` into the working tree.
- One stdout and one stderr file per process, never merged. Marker order is an
  assertion in the quit legs, and interleaving two processes destroys it. Every
  `.err` file is dumped when a leg fails.
- Every wait carries an explicit budget in seconds, and each leg has an overall
  deadline that caps the waits inside it.
- Child processes are found with `pgrep -P <app pid>`. Never a bare `pkill` or
  `pkill -f`: it would take down the maintainer's own session along with the leg.
- `DUSK_REGRESS_SCENARIO_KEEP=1` keeps each leg's directory instead of deleting
  it, and prints the path.

`bb-crash-relaunch` is the one to watch. The POSIX handoff has no
acknowledgement, so two instances racing for a slot a killed instance left
behind is the leg most likely to flake. It fails with both processes' stderr and
is not retried; re-run it ten times before trusting a change to that path.

`bb-damaged-recent` needs the startup picker, and GLX under Xvfb is not
guaranteed on every host (the same caveat that keeps DAF/DGL windows in the
hardware pass). The leg accepts either `picker shown` or `picker unavailable on
this display` - what it will not accept is neither. Set
`DUSK_REGRESS_REQUIRE_PICKER=1` on a host where GLX does work to demand the
first.

Legs whose app-side seam is not in the binary report `SKIP` with the name of
what is missing (`DUSKSTUDIO_QUIT_AFTER_MS`, the `[Dusk Studio/startup]`
markers, the `session.mint_oop_fixture` scenario) rather than passing on an
assertion that never ran.

`bash scripts/regress/scenarios.sh` runs just these legs and prints the same
table; `--app <binary>` points it at a build other than `build/`.

CI runs only the headless half: `.github/workflows/linux-build.yml` has a
`Scenario suite (xvfb)` step carrying `continue-on-error: true`, so a scenario
failure there is reported without failing the job. It becomes a required check
once it has run clean for a week. The `bb-*` legs stay local - they need
`pgrep -P`, `kill -9` and several app processes running at once.

### macOS

The M3 Air (`marc@macbook-air.local`) is a headless build node: key auth, no
sudo ever, toolchain in `~/bin` and `~/tools`, and no GNU `timeout` - remote
deadlines are a poll loop over a completion marker.

Prerequisites: an ssh key on the node, a clean `~/src/dusk-studio` working tree
(submodule pointers may drift; tracked files may not), `~/mac-configure.sh`
(takes the build dir as `$1` and passes `-DDAF_PATH` / `-DDAF_WIDGETS_PATH`),
and the sibling checkouts `~/src/plugins-main`, `~/src/DPF`, `~/src/DPF-Widgets`.

The commit under test never goes through GitHub: it is pushed straight over ssh
to `refs/heads/regress-<short sha>` in the node's checkout. The node's previous
branch and submodule commits are recorded at the start and restored on exit,
including when a leg fails.

| Leg | What it proves |
|---|---|
| `mac-preflight` | node reachable, review model unloaded from ollama (it pins several GB and a `-j6` build would swap against it), working tree clean. |
| `push-head` / `mac-checkout` | the node builds this exact commit. `WARN` when a submodule cannot be synced to the recorded pin. |
| `donor-pin` | `~/src/plugins-main` detached at the `DONOR_REV` in `.github/workflows/release.yml`. |
| `daf-pins` | `~/src/DPF`, `~/src/DPF-Widgets` and the Pugl submodule moved to the revisions in `.github/actions/clone-daf-stack/action.yml`. Best effort: an unfetchable revision is a `WARN`, not a failure. The DAF checkouts are left at the pin, not put back. |
| `configure-app` / `configure-tests` / `build-app` / `build-tests` / `ctest` | the same build and test surface as CI's macOS job. |
| `selftest` | `DUSKSTUDIO_RUN_SELFTEST=1` against the built `.app`, behind a marker-file deadline. |
| `scenarios` | `DUSKSTUDIO_RUN_SCENARIOS=all` against the same `.app`, behind the same marker-file deadline, with `DUSKSTUDIO_FIXTURE_DIR` pointed at the node's `build-tests` tree and `tests/fixtures`. Passes only on exit 0, no `[FAIL]` line, and the terminal `=== scenarios: ` summary. Skipped cases go into the leg's note - this node builds no LV2 host, so the LV2 cases skip there and the AU scan case runs only there. |

What it cannot prove: **anything with a window**. Launching the GUI from an ssh
session aborts in the main window constructor on that node, and it does so on
`main` too, so it is the environment and not the build. That covers both
`gui-launch` and `scenarios-gui`, the GUI half of the scenario suite. Those legs
report `SKIP` rather than a false failure; run them by hand from a console
session on the Air:

```bash
cd ~/src/dusk-studio
APP=./build/DuskStudio_artefacts/Release/DuskStudio.app/Contents/MacOS/DuskStudio
"$APP"                                                    # gui-launch
DUSKSTUDIO_RUN_SCENARIOS=gui \
  DUSKSTUDIO_FIXTURE_DIR="$PWD/build-tests:$PWD/tests/fixtures" \
  "$APP"                                                  # scenarios-gui
```

The IPC self-test is Linux-only code and is skipped for that reason, not this one.

### Windows

The libvirt domain `win11` on this box has no qemu-guest-agent and no SSH. The
channel is: `virsh send-key` types into a guest PowerShell console, an HTTP
server on `192.168.122.1:8000` serves the phase scripts and the payload, a
collector on `:9000` receives each phase's POSTed report, and `virsh screenshot`
shows what the guest is actually doing. Both servers are stopped on exit with
self-excluding `pkill` patterns.

Prerequisites: libvirt access to `win11` without sudo (`qemu:///system`), the
guest running, `7z`, `zip`, `python3` (Pillow for the PNG screenshots), `gh`
authenticated for `--release-run`. Nothing has to be set up inside the guest:
the phases bring their own session, and none of them clicks anything.

The payload is built on the host: the MSI is unpacked with `7z`, the flattened
`CM_FP_bin.*` names are put back into `bin/`, the plugin host is renamed to
`dusk-studio-plugin-host.exe` (the app looks for it beside itself under that
name), `scripts/regress/sessions/minimal/session.json` is copied in as
`regress-session/session.json` for phase 3 to load, and the result is zipped.
The guest unpacks it to `%LOCALAPPDATA%\DuskStudio-regress`, which needs no
elevation - installing into `C:\Program Files` would need UAC, which must not
be auto-accepted.

| Leg | What it proves |
|---|---|
| `payload` | the installer unpacks to a runnable `bin/` tree with both executables. |
| `host-servers` | the script and report channels are listening on `192.168.122.1`. |
| `guest-wake` | the domain is running and its display is not blanked. Screenshot in the run directory. |
| `console-probe` | a fresh PowerShell console is up and accepting typed input, before any phase is typed into it. |
| `phase1-selftest` | headless `DUSKSTUDIO_RUN_SELFTEST=1` with stdout and stderr captured through `ProcessStartInfo` redirection: exit code 0, at least one `[PASS]`, no `[FAIL]`. |
| `phase2-handoff` | GUI launch, then a second launch carrying a session path hands over and exits 0 within 30 s while the first instance stays alive, and `GetForegroundWindow()` is the first instance's window afterwards - the handoff is supposed to raise it, which the exit code alone cannot see. |
| `phase3-session-close` | the session the payload ships is loaded through `DUSKSTUDIO_LOAD_SESSION` (waited for by its `[Dusk Studio/Load]` line), then two `WM_CLOSE` messages are posted back to back. Exit 0 within 50 s, at least eight `[Dusk Studio/shutdown] phase` markers, and `re-entry ignored: shutdown already in progress` from the second close landing on the latch. |
| `ipc-selftest` | `SKIP`. `DUSKSTUDIO_RUN_IPC_SELFTEST` never returns on Windows (issue #504), so the out-of-process transport is only compile- and contract-verified there. Remove the skip when #504 closes. |

Every phase opens its own console from the Start menu: a phase that calls
`SetForegroundWindow` steals focus, so the next `send-key` would land in the
wrong window. Constraints the guest-side scripts have to respect, all of them
things that have already gone wrong here:

- `$host` is a read-only PowerShell automatic variable; a script assigning it
  dies before its POST.
- `iex` runs in the console's session scope, so variables survive between
  phases. Every script assigns its own before reading them.
- A P/Invoke with a PowerShell scriptblock delegate (`EnumWindows`) throws under
  `iex`. The P/Invoke surface stays limited to direct calls.
- `WM_CLOSE` on a fresh launch is swallowed while the startup picker is open:
  `requestQuit` returns with a modal up. That is why phase 3 passes the session
  in through `DUSKSTUDIO_LOAD_SESSION`, which skips the picker entirely. No
  phase depends on a screen coordinate or on synthesized mouse input.
- Redirected stderr cannot be read with `ReadToEndAsync` while the app is still
  running: that task only completes when the pipe closes. Phase 3 drains it one
  bounded `ReadLineAsync` at a time, which is how it can wait for a marker mid
  run.

Screenshots, the raw report and the served payload all stay in the run
directory printed at the end (`/tmp/dusk-regress-windows-<timestamp>/`). If a
phase times out, read its screenshot before re-running: something else driving
the VM has been the cause before.

Overrides: `DUSK_REGRESS_VM`, `DUSK_REGRESS_LIBVIRT_URI`, `DUSK_REGRESS_HOST_IP`.

### Adding a leg

1. Write the check as a shell function in the platform's script that returns 0
   for pass, non-zero for fail, and prints its own detail.
2. Register it with `regress_leg "<name>" <function>` in leg order, or
   `regress_leg_soft` when a `warn:` line in its output should downgrade it to
   `WARN` instead of failing the run, or `regress_skip "<name>" "<reason>"` when
   the environment cannot run it. A `SKIP` needs a reason that says what to run
   by hand instead.
3. For a Windows leg, add a `.ps1` under `scripts/regress/windows/`, map it to a
   short served name in `install_scripts` (the name is typed one keystroke at a
   time), and end it with the two contract lines the runner waits for:

   ```
   REGRESS-PHASE <name> RESULT PASS|FAIL
   REGRESS-PHASE <name> END
   ```

   The host substitutes `@@HOSTIP@@`, `@@ROOT@@` and `@@ZIP@@` when serving, so
   those values are not duplicated per script.
4. `bash -n` and `shellcheck` every script you touched.
5. If the leg greps a string out of the app's output, pin that string in
   [tests/stderr_marker_contract.cpp](../tests/stderr_marker_contract.cpp). A
   reworded marker still compiles and still runs; without the contract case the
   leg quietly stops asserting anything and keeps passing.

---

## Part 10 - Release

### Release order

Releases ship from `main`, which is where 0.13.3 and later are tagged.
`release/0.12` and `release/0.13` are historical: they carry the `v0.12.6` and
`v0.13.2` tags and receive no further work.

The order is load-bearing. Replace `X.Y.Z` with the release version throughout.
Set `RELEASE_VERSION=X.Y.Z` in the shell used for the guarded commands.

[`CPACK_PACKAGE_CONTACT`](../CMakeLists.txt) holds the maintainer address and
feeds only DEB/RPM package metadata. Neither format is among the six assets the
current tag workflows publish, so nothing a tagged release produces uses it.

1. Finish the `## [X.Y.Z] - Unreleased` section in
   [CHANGELOG.md](../CHANGELOG.md), then run
   [`scripts/bump-version.sh`](../scripts/bump-version.sh)
   `"$RELEASE_VERSION" "<one-line notes>"`. The script requires the exact
   unreleased changelog heading, writes `VERSION`, prepends the AppStream
   release entry, and updates the summary in
   [packaging/RELEASE-NOTES.md](../packaging/RELEASE-NOTES.md). It aborts if
   either metadata file or insertion marker is unavailable. Require the diff
   to contain all three expected metadata changes before proceeding.
2. Replace `Unreleased` in that changelog heading with the release date.
   Review and verify all release metadata. Then run the Patreon freshness check
   from the primary checkout, not a `.codex/worktrees/*` issue worktree:

   ```bash
   (
     set -e
     python3 - <<'PY'
   import json
   from pathlib import Path
   config_path = Path.home() / ".config" / "dusk-audio" / "patreon.json"
   overrides = json.loads(config_path.read_text(encoding="utf-8")).get("name_overrides", {})
   if overrides:
       raise SystemExit(
           "STOP: local Patreon name_overrides are not available to release workflows"
       )
   PY
     DONOR_REV=$(sed -nE \
       's/^[[:space:]]*DONOR_REV:[[:space:]]*([0-9a-f]{40})[[:space:]]*$/\1/p' \
       .github/workflows/release.yml)
     [[ "$DONOR_REV" =~ ^[0-9a-f]{40}$ ]] \
       || { echo "STOP: release workflow has no valid DONOR_REV" >&2; exit 1; }
     git -C ../plugins cat-file -e "$DONOR_REV^{commit}" 2>/dev/null \
       || git -C ../plugins fetch origin "$DONOR_REV"
     git -C ../plugins show "$DONOR_REV:plugins/shared/PatreonBackers.h"
     env -u DUSK_PLUGINS_PATH scripts/update-patrons.py --dry-run
   )
   ```

   If the script stops because sibling headers are out of sync, reconcile those
   local copies and rerun it; that error is separate from Patreon freshness.
   The block also stops when the local config contains `name_overrides`, because
   the release workflows do not receive those mappings and would inject
   different display names. Clear the mappings only if the unmodified Patreon
   names are intended; otherwise stop and add reviewed workflow propagation.
   The command reads `DONOR_REV` from the Linux release workflow; all donor
   workflow pins must match. Compare the reported `champions`, `patrons`,
   `supporters`, and `hugs` tiers with the
   header printed from that exact revision. The dry run does not rewrite
   supporter headers, but it can refresh the local Patreon access and refresh
   tokens. If it prints `Patreon tokens refreshed and saved.`, update the
   `PATREON_ACCESS_TOKEN` and `PATREON_REFRESH_TOKEN` Actions secrets before
   tagging. If the live tiers differ from the committed header, require all four
   Patreon Actions secrets so the binary workflows inject the live list. Without
   those secrets, stop: the workflows warn and ship the committed list.
3. Rebuild the app and tests, run the full test suite and JUCE gate, then run
   the integration self-test on a private Xvfb display. Never launch the
   release binary on the live Wayland session.

   ```bash
   (
     set -e
     cmake --build build -j6
     cmake --build build-tests --target dusk-studio-tests -j6
     ctest --test-dir build-tests --output-on-failure
     bash tools/juce-gate.sh
     scripts/run-selftest-xvfb.sh
   )
   ```

   Stop if any command or self-test check fails. A known-slow cold plugin scan
   can use `DUSKSTUDIO_SELFTEST_TIMEOUT=180s`; do not remove the timeout.
4. Commit the reviewed metadata and verified changelog. Record the exact
   commit with `RELEASE_COMMIT=$(git rev-parse HEAD)`. The guarded checks in
   the next step confirm that commit contains `RELEASE_VERSION`.
5. Land that exact commit on `origin/main` before creating a tag, by direct
   push or PR as repository rules require. Fetch the remote state and require
   the recorded commit to be contained in it:

   ```bash
   (
     set -e
     git fetch origin main
     git merge-base --is-ancestor \
       "${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}" origin/main \
       && echo "landed on origin/main" \
       || { echo "STOP: release commit is not on origin/main" >&2; false; }
     test "$(git show \
       "${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}:VERSION" \
       | tr -d '[:space:]')" = \
       "${RELEASE_VERSION:?set RELEASE_VERSION first}"
     git show \
       "${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}:CHANGELOG.md" \
       | grep -E '^## \[[0-9]+\.[0-9]+\.[0-9]+\] - [0-9]{4}-[0-9]{2}-[0-9]{2}$' \
       | grep -F "## [${RELEASE_VERSION:?set RELEASE_VERSION first}] - "
   )
   ```

   Do not continue if any guard fails. If the landing method rewrote the
   commit, identify the exact landed commit, re-record `RELEASE_COMMIT`, and
   repeat the metadata and containment checks.
6. Tag the exact guarded commit and push only that tag:

   ```bash
   git tag -a "v${RELEASE_VERSION:?set RELEASE_VERSION first}" \
     -m "Dusk Studio ${RELEASE_VERSION}" \
     "${RELEASE_COMMIT:?record RELEASE_COMMIT after committing metadata}"
   git push origin "refs/tags/v${RELEASE_VERSION:?set RELEASE_VERSION first}"
   ```

7. A `v*` tag starts [`Dusk Studio release`](../.github/workflows/release.yml).
   Its preflight rejects a tag that does not equal `v$(cat VERSION)` and proves
   the private-repository token can write before any platform build starts.
   Linux, macOS, Windows, and manual jobs then run in parallel. Only after all
   four succeed does one publisher assemble the assets, create `SHA256SUMS`,
   and write to `dusk-audio/dusk-studio-releases`.

Do not announce the release when the workflow merely turns green; complete the
acceptance checks below first.

### Tag assets and acceptance

A complete `vX.Y.Z` release has exactly these six assets:

- `dusk-studio-X.Y.Z-Linux-x86_64.tar.xz`
- `dusk-studio-X.Y.Z-Linux-aarch64.tar.xz`
- `dusk-studio-X.Y.Z-macOS-arm64.dmg`
- `dusk-studio-X.Y.Z-Windows-x64.msi`
- `MANUAL.pdf`
- `SHA256SUMS`

The publisher downloads all five payloads into one job and refuses to publish
unless their exact filenames are present. It writes a sorted, lowercase
`SHA256SUMS` with five entries and verifies it locally before uploading all six
assets together.

Before announcement, run
[`scripts/verify-release-assets.sh`](../scripts/verify-release-assets.sh)
`vX.Y.Z`. It fails if an expected asset class is missing or duplicated, an
unexpected asset exists, or the release body's summary slot is missing or
empty. It matches every asset name exactly, including the `macOS-arm64` and
`Windows-x64` suffixes, but it reads names only and never opens a payload, so
it cannot prove the architecture of what is inside one. Summary-slot
validation applies to v0.13.0 and later; older release bodies predate the
markers and are expected to fail that check.
Download the assets into a clean directory and perform the
manual checks that the script cannot cover:

- Confirm the populated release summary is correct for this version. The
  verifier rejects an empty slot but cannot judge editorial accuracy.
- Confirm all six filenames exactly match the list above. Inspect the
  executable inside the DMG and MSI and confirm arm64 and x64 respectively;
  do not infer architecture from the filename. The DMG must carry the `.app`,
  `LICENSE` and `LICENSES.txt` and nothing else; a `share/` tree of XDG desktop
  files in it is a Linux install rule leaking into the macOS package.
- Run `sha256sum --check SHA256SUMS` against the downloaded payloads and
  require every one of the five to pass. A payload rebuilt after the checksums
  were written passes the name check and fails here, which is the point.
- Open `MANUAL.pdf`; confirm the figures render and the sharp and flat
  accidentals display correctly.
- Extract both Linux tarballs and smoke each binary only on its matching
  architecture, under a private Xvfb display with `WAYLAND_DISPLAY` unset.
  Never let a release binary touch the live Wayland session. DAF/DGL windows
  that require GLX belong in the hardware pass.
- Inspect the tarball, DMG, and MSI payloads. Each must be Dusk-only, with no
  `JUCE-*` paths, and each must contain the complete
  [LICENSES.txt](../LICENSES.txt), not merely a file by that name.
- On macOS and Windows, run the
  [window-activation smoke test](../tests/window_activation_smoke_test.md) for
  initial launch, session open, minimized restore, and second-process handoff.
- In every bundled `LICENSES.txt`, confirm the full-text section is present and
  the JUCE-bundled entries cover HarfBuzz, SheenBidi, libjpeg, libpng, zlib,
  FLAC, and Ogg/Vorbis. If a future SheenBidi source adds a NOTICE file, ship
  that file alongside its license text.

The release is accepted only when the asset/body check, checksum verification,
PDF inspection, private-Xvfb smoke tests, payload inspection, macOS and Windows
window-activation smoke tests, and license checks all pass.

---

## Quick reference card

```bash
BUILD APP        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j6
RUN              ./build/DuskStudio_artefacts/Release/DuskStudio
SELF-TEST        scripts/run-selftest-xvfb.sh
BUILD TESTS      cmake -S . -B build-tests -DDUSKSTUDIO_BUILD_TESTS=ON && cmake --build build-tests --target dusk-studio-tests -j6
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
