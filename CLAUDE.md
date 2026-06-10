# Dusk Studio — instructions for Claude

Dusk Studio is a portastudio-style DAW for Linux, JUCE 8 / C++17. The authoritative spec is [DuskStudio.md](DuskStudio.md). Read it before changing anything non-trivial.

## Architecture cheat-sheet

- **Audio backend**: PipeWire (primary) via JUCE's JACK backend; ALSA fallback.
- **DSP**: extracted from the user's existing Dusk Audio plugins at `/home/marc/projects/plugins/`. Shared headers live (or will live) at `plugins/plugins/shared/dsp-cores/` so both Dusk Studio and the Dusk plugins are single-source-of-truth consumers. Resolved via `-DDUSK_PLUGINS_PATH=/path/to/plugins` or sibling `../plugins` (mirror of the JUCE pattern). Header-only cores: edit a file in the plugins repo, next Dusk Studio build picks it up — no copy step, no submodule bump.
- **JUCE**: 8.x, resolved via `-DJUCE_PATH` or sibling `../JUCE` (same scheme as the Dusk plugins repo).
- **Topology**: 24 channel strips (HPF → 4-band EQ → FET/Opto comp → sends → pan → bus assign → fader → mute/solo) → 4 aux buses (EQ + comp + fader) → master (Pultec EQ + bus comp + tape sat + fader). Three banks of 8 select which 8 strips the control surface drives at a time; the full 24 are visible on screen.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/DuskStudio_artefacts/Release/DuskStudio
```

JUCE and the Dusk plugins repo are auto-discovered from sibling directories. Pass `-DJUCE_PATH=...` or `-DDUSK_PLUGINS_PATH=...` to override either.

### Cross-OS dev (macOS authoring, Linux testing)

Single canonical build dir on both OSes: `build/` (app) and `build-tests/` (Catch2 tests). The sibling repos and JUCE source differ per OS, but the build directory does not — switching machines does not require a new build dir.

| OS    | App build | Tests build      | JUCE source                              | Plugins source                 |
|-------|-----------|------------------|------------------------------------------|--------------------------------|
| macOS | `build/`  | `build-tests/`   | `../JUCE` (upstream)                     | `../plugins` (any branch)      |
| Linux | `build/`  | `build-tests/`   | `../JUCE-wayland` (plugdata-team fork)   | `../plugins-main` (worktree)   |

CMake auto-detects:
- **JUCE** — on Linux it prefers `../JUCE-wayland` if present, falls back to `../JUCE`. The wayland fork has 5 local commits Dusk Studio depends on (XEmbed mapping, X11-on-Wayland fix, peer-creation latch — see [memory](../../.claude/projects/-home-marc-projects-Dusk Studio/memory/linux_juce_wayland_pin.md)) and a divergent `addDefaultFormatsToManager` free function.
- **Plugins** — prefers `../plugins-main` over `../plugins`. The `plugins-main` directory is a git worktree pinned to the plugins repo's `main` branch so feature-branch work in `../plugins/` doesn't break Dusk Studio's expected donor API. Set up once on Linux:
  ```bash
  cd ../plugins && git worktree add ../plugins-main main
  ```
  Mac dev typically stays on `main` of `../plugins` and skips the worktree.

The upstream-vs-fork `addDefaultFormats` API split is hidden behind [src/engine/JuceCompat.h](src/engine/JuceCompat.h) — call `duskstudio::juce_compat::addDefaultFormats(fm)` and the `#if defined(__linux__)` lives in one place. Don't sprinkle new platform `#ifdef`s into call sites.

## Phase plan

Phases 1a → 5 follow [DuskStudio.md](DuskStudio.md). Don't skip ahead. As of writing, Phase 1a (live mixer) and most of Phase 2 (multitrack recording + atomic JSON session save/load + autosave) are working. Phase 1b (send-bus plugin hosting on aux strips) and Phase 3 prep (take history) are the most recent additions. Phase 3 proper (markers, fader automation, punch+loop refinements) is next.

## Audio thread rules (MANDATORY)

The audio thread (`AudioEngine::audioDeviceIOCallbackWithContext` and any `process*` it calls) is **real-time**. Violating these rules causes glitches, clicks, dropouts, or crashes:

- **NEVER allocate memory** — no `new`, `make_unique`, `push_back`, `resize`, `std::string`, or `juce::String`. Pre-size all buffers in `prepare`/`audioDeviceAboutToStart`.
- **NEVER lock a mutex** — cross-thread state is `std::atomic<T>` or `juce::AbstractFifo`. For rare swap-load patterns (see PluginSlot), use a lock-free atomic instance pointer; the audio thread reads via `acquire`, the message thread mutates via `release` and only releases the old object after the swap completes.
- **NEVER do I/O** — no file reads, no logging, no `DBG()`, no network. Disk I/O for recording goes through `juce::AudioFormatWriter::ThreadedWriter` + `juce::TimeSliceThread`; the audio thread only `write()`s to a queue.
- **NEVER call message-thread APIs** — no `sendChangeMessage()`, no `Component` methods, no `MessageManager`. Cross-thread signalling for UI metering uses `std::atomic<float>` polled by a `juce::Timer`.
- **Use `juce::ScopedNoDenormals`** at the top of any audio-callback function that runs DSP — prevents CPU spikes from subnormal floats.
- **Cache atomic pointers in `prepare`/`bind`**, never on the audio thread. See `ChannelStrip::bindCompParams()` for the canonical pattern (caches `getRawParameterValue()` pointers from `UniversalCompressor`'s APVTS).
- **Memory ordering**: metering atomics use `std::memory_order_relaxed`; state flags that gate audio reads (e.g. plugin instance pointer, RT counters in Session) use `release` on the writer side and `acquire` on the audio-thread reader.
- **Check `numSamples == 0`** — early return, JUCE/JACK can send empty blocks during transitions.
- **Check `mixL.size() < numSamples`** — host-driven oversized blocks are real (see the existing guard in the callback). Either bail with cleared output, or chunk the work.
- **Smoothers don't tick during skipped passes** — when an aux/track is silent and we skip its DSP, smoothers stay frozen and pick up correctly when audio resumes. Don't "advance" smoothers manually.

## DSP lifecycle

Dusk Studio's DSP classes (`ChannelStrip`, `AuxBusStrip`, `MasterBus`, `MasteringChain`, `PluginSlot`) follow a consistent shape:

- **`prepare(sampleRate, blockSize, ...)`** — cache `sampleRate`, `.prepare(spec)` every `juce::dsp::*` member, `.reset(sampleRate, rampSeconds)` every `SmoothedValue`, size every per-block scratch buffer. Called from `AudioEngine::prepareForSelfTest` (and indirectly from `audioDeviceAboutToStart`). May be called multiple times — must be idempotent.
- **`bind(params)`** — stash a reference to the matching `ChannelStripParams` / `AuxBusParams` / `MasterBusParams` so the audio thread can read live values via the cached atom addresses. Called once at `AudioEngine` construction.
- **`processInPlace(L, R, numSamples)`** or **`processAndAccumulate(...)`** — the audio-thread entry point. Updates smoother targets at the top, runs DSP, updates `mutable` meter atomics at the bottom.
- **Latency** — set via `setLatencySamples()` in `prepareToPlay` for plugins; clear to 0 when bypassed, restore on un-bypass. Dusk Studio's hosted plugin slots inherit latency from the loaded instance.
- **Smoothing** — `juce::SmoothedValue<float>` everywhere a control changes. `.reset()` in `prepare`, `.setTargetValue()` from the param-update step at the top of each block, `.getNextValue()` per sample inside the DSP loop.
- **Buffer processing** — raw pointer loops (`getWritePointer`) for sample-level DSP; wrap as `juce::AudioBuffer<float>` over existing storage (NOT a copy) when handing off to a JUCE DSP module like `BritishEQProcessor::process`.

## Parameter conventions

Dusk Studio has two parameter sources, both following the same "cache-the-atomic-pointer-once, read-lock-free-on-the-audio-thread" rule:

1. **Session-level atomics** — `ChannelStripParams::faderDb`, `AuxBusParams::eqLfGainDb`, etc. are plain `std::atomic<T>` members of structs in [src/session/Session.h](src/session/Session.h). The UI mutates via `.store(v, std::memory_order_relaxed)` on `onValueChange`; the audio thread reads via `.load(std::memory_order_relaxed)`. New audio params go here.
2. **APVTS atoms in vendored DSP** — `UniversalCompressor`, `BritishEQProcessor`, `TubeEQProcessor` etc. expose their parameters via `juce::AudioProcessorValueTreeState::getRawParameterValue("name")`. Cache the returned pointer ONCE in a `bindCompParams()`-style function called from `prepare`. The audio thread writes via `storeAtom(p, v)` at the top of each block (lock-free, notification-free path) and the donor's `processBlock` reads it.

Never call `getRawParameterValue()` on the audio thread — it's a string lookup. The pattern in [src/dsp/ChannelStrip.cpp](src/dsp/ChannelStrip.cpp) `bindCompParams()` is the reference; copy it for any new vendored DSP.

For metering, use `mutable std::atomic<float>` with `relaxed` ordering on the params struct so DSP code holding a `const Params*` can still update meters.

## Async resource loading

For plugin instances, audio file readers, or any heavy resource — never block the audio thread or `prepareToPlay` on construction:

- **Plugin slots** — see [src/engine/PluginSlot.cpp](src/engine/PluginSlot.cpp). Message-thread `loadFromFile`/`loadFromDescription` constructs the new plugin off-thread, primes it via `prepareToPlay`, then atomically swaps `currentInstance` (a `std::atomic<juce::AudioPluginInstance*>`). The audio thread reads via `.load(acquire)`. The previous instance is released AFTER the swap completes — its destructor isn't RT-safe.
- **Recording** — `RecordManager` owns one `juce::AudioFormatWriter::ThreadedWriter` per active track. The audio thread only calls `writer->write()` (lock-free push to a JUCE-managed queue); `juce::TimeSliceThread` drains it.
- **Autosave / session save** — message thread only. Atomic temp-file + rename in `SessionSerializer::save` so a crash mid-save never produces a half-written `session.json`.

## Common DSP patterns

- **Oversampling** — All oversampling (donor plugins and otherwise) is controlled globally by the **Effect Oversampling** dropdown in the Audio Device settings panel. Individual DSP units must NOT enable internal oversampling on their own; the engine drives the chosen factor through to every processor that supports it. Don't call `setInternalOversamplingEnabled` (or equivalent) at the DSP-class level — wire new processors to read the engine-wide setting instead. **One deliberate exception**: the mastering [BrickwallLimiter](src/dsp/BrickwallLimiter.h) owns a fixed 4× oversampler. True-peak limiting is impossible without it, and the limiter is the terminal output stage (no donor saturation downstream to double-oversample), so it stays independent of the global setting.
- **Filters** — `juce::dsp::IIR::Filter` and `BritishEQProcessor` (vendored). Always `.prepare(spec)` in `prepare`. **No EQ cramping ever** — IIR filters near Nyquist need oversampling; pre-warping alone is insufficient. The vendored EQs handle this.
- **Metering** — `std::atomic<float>` with `relaxed`, written from the audio thread, polled by the UI on a 30 Hz `juce::Timer`. SIMD-friendly peak detection via `juce::FloatVectorOperations::findMinAndMax`.
- **Smoothing** — `juce::SmoothedValue` with a 20 ms ramp is the default for any continuous control (faders, pans, gains). Bus toggles smooth 0..1 over 20 ms to avoid clicks on assign/unassign.
- **Saturation / tubes / transformers** — use the `AnalogEmulation` library at `../plugins/plugins/shared/AnalogEmulation/`. Don't roll your own.
- **Lock-free skip optimisation** — see the aux DSP-skip in [src/engine/AudioEngine.cpp](src/engine/AudioEngine.cpp). Cheap `findMinAndMax` peak check before the expensive EQ+comp pass; skips when the bus is silent. Smoothers freeze across the skip and resume cleanly.

## Testing

Dusk Studio has Catch2 v3 unit tests in [tests/](tests/), gated behind `-DDUSKSTUDIO_BUILD_TESTS=ON` so the default app build is unaffected. The convention is **narrow-link tests**: each test binary compiles only the `src/...` translation units it actually exercises, plus the JUCE modules those need. No GUI, no Dusk DSP, no full `Dusk Studio` target. This keeps the test build fast and isolates failures to the unit under test.

### Build + run

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

Use `build-tests/` (separate from `build/`) so the two configurations don't fight over CMake cache state.

### When to add a test

- **Any non-trivial DSP change** — new processor, algorithm tweak, or a bug fix where the failure can be expressed as an audio-buffer assertion (silence-in/silence-out, peak ≤ ceiling, gain at unity, latency report matches actual delay, etc.).
- **Any pure logic change** in `src/session/`, `src/engine/`, or `src/dsp/` that doesn't require a live audio device or Dusk DSP — region edit math, marker math, parameter range conversions, smoother behaviour.
- **Regression tests for fixed bugs** — write the failing test first, then the fix.

Don't write tests for: UI components (no JUCE message-loop / Component test harness yet), code that requires a real audio device, or anything that would need to spin up the full `AudioEngine` + Dusk DSP chain. Those are integration scope and currently belong in `DUSKSTUDIO_RUN_SELFTEST=1`.

### How to add a test

1. Drop a new `tests/<unit>_<aspect>.cpp` following the [tests/smoke_brickwall_limiter.cpp](tests/smoke_brickwall_limiter.cpp) pattern (`#include <catch2/...>`, `TEST_CASE(...)`, `REQUIRE(...)` / `REQUIRE_THAT(..., WithinAbs(...))`).
2. In [tests/CMakeLists.txt](tests/CMakeLists.txt), add the new `.cpp` AND every additional `src/...` source file it pulls in (header-only deps don't need listing). Keep the source list minimal — only what's transitively reachable from the test.
3. If the unit needs a JUCE module not yet linked (e.g. `juce_dsp` for an oversampler test), add it to `target_link_libraries(dusk-studio-tests PRIVATE ...)`.
4. Build + run the commands above. `catch_discover_tests` registers each `TEST_CASE` with ctest automatically — no manual wiring per test.

### Test style

- **Float comparisons go through `WithinAbs` / `WithinRel`**, never `==`. Pick a tolerance that reflects the math (1e-6 for arithmetic, 1e-4 for accumulated DSP, larger for filters near Nyquist).
- **Drive DSP through several blocks before measuring** when the unit has lookahead, smoothing, or filter state. Measuring the first block gives misleading results because envelopes / smoothers / delay lines haven't reached steady state.
- **One concept per `TEST_CASE`.** Use `SECTION` for variations on the same setup, separate `TEST_CASE`s for unrelated scenarios.
- **No sleeps, no threads, no real audio device.** Tests run in milliseconds and on every build.

### Forced verification (extends rule 4 in Agent directives)

For DSP / engine / session changes, "task complete" now requires the test build + ctest pass in addition to the main build:

```bash
cmake --build build-tests --target dusk-studio-tests -j$(nproc) && ctest --test-dir build-tests --output-on-failure
```

If a change touches a unit that has tests, those tests must pass. If it touches a unit that *doesn't* have tests but easily could, add at least one.

## Code style

- **C++17.** No `concepts`, no `std::format`, no `std::ranges` outside of trivially-replaceable uses.
- **Naming** — `camelCase` methods/locals/members, `PascalCase` classes/structs, `kPascalCase` constants, `SCREAMING_SNAKE` only for macros.
- **Headers** — `#pragma once`. Include order: JUCE → project-local (`../session/Session.h`, etc.) → STL.
- **Comments** — default to none. Only write a comment when the *why* is non-obvious: a hidden invariant, a workaround for a specific bug, behaviour that would surprise a reader. Don't restate what the code does. Don't reference task numbers, PR descriptions, or the conversation that produced the change.
- **No backwards-compat shims** — when something's gone, delete it. Don't leave `// removed`, don't keep dead `_var` parameters, don't rename variables to suppress unused warnings (compile flag handles those properly).
- **No abstractions on spec** — three similar lines is better than a premature class. Add the abstraction the third time you'd actually use it, not the first.
- **DSP separates from UI** — `src/dsp/`, `src/engine/`, and `src/session/` never include `src/ui/`. The other direction is fine. New cross-cutting helpers go in `src/ui/` if UI-only (see [src/ui/PluginPickerHelpers.h](src/ui/PluginPickerHelpers.h)) or in `src/engine/` if shared.

## Git

- Never add a `Co-Authored-By: Claude` (or any Claude/Anthropic) trailer to commits. Commits are authored by the user only.
- Commits should be small and reviewable. Phase boundaries are natural commit boundaries.
- Don't `git push` without explicit instruction. Don't force-push to `main` ever.

## Agent directives — mechanical overrides

Operating within a constrained context window. Adhere to these regardless of any default behaviour:

### Pre-work
1. **Step 0 — clean before refactoring.** Dead code accelerates context compaction. Before any structural refactor of a file >300 LOC, first remove unused includes, dead functions, commented-out blocks, and debug logging in a separate commit.
2. **Phased execution.** Never attempt large multi-file refactors in a single response. Break work into explicit phases of max 5 files. Verify and pause for explicit approval between phases.

### Code quality
3. **Senior dev override.** Ignore the "simplest approach first / don't refactor beyond the ask" defaults when the architecture is genuinely flawed, state is duplicated, or patterns diverge. Ask: "What would a senior dev reject in code review?" Fix the structural issues, not just the surface symptom. (This is in tension with "no abstractions on spec" — the difference is: existing duplication that's already a problem is fair game; speculative future flexibility is not.)
4. **Forced verification.** Don't claim a task is complete until you have run:
   - `cmake --build build -j$(nproc)` — must succeed with zero new warnings
   - For audio-path changes: launch the binary with `DUSKSTUDIO_RUN_SELFTEST=1` if practical, or at minimum confirm the binary launches without immediate crash
   If verification isn't possible in the current environment, say so explicitly. "Builds locally" ≠ "works."

### Context management
5. **Sub-agent strategy.** For research tasks touching >5 independent files, spawn parallel `Explore` agents (each gets its own clean context) rather than serially loading every file into the main context. We did this earlier in this session — works well.
6. **Context decay awareness.** After ~10 messages or any focus shift, re-read the files you're about to edit. Don't trust prior memory — auto-compaction may have altered it. The conversation summary is lossy.
7. **File read budget.** `Read` is hard-capped at ~25k tokens (~2000 lines for typical source). For files over that limit (notably [DuskStudio.md](DuskStudio.md), ~30k tokens), read in offset/limit chunks or delegate to an Explore agent. Never assume a single read covered the whole file.
8. **Tool result blindness.** Large tool outputs may be silently truncated. If a `grep` or `find` returns suspiciously few results, re-run with narrower scope and explicitly say in the summary that earlier output may have been truncated.

### Edit safety
9. **Edit integrity.** Re-read the target file before every edit. After editing, the harness confirms success or errors — trust that, but if you're making >3 sequential edits to the same file, re-read once mid-stream to confirm the cumulative state matches your mental model.
10. **No semantic search.** Only `grep` is available — no AST awareness. When renaming a function/type/variable or changing a signature, search separately for:
    - Direct calls and references (`grep -rn 'name\b'`)
    - Type-level references (templates, typedefs, forward declarations)
    - String literals containing the name (look in JSON serialisers, alert text)
    - Header includes and forward declarations
    - The vendored DSP at `../plugins/plugins/` if the symbol is shared
    - `CMakeLists.txt` if files moved
    Don't assume one grep caught everything.
