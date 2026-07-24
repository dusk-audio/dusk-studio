# De-JUCE campaign — map and working agreement

Read this first among the de-JUCE docs in any session doing de-JUCE work. It is
the campaign-level map: where we are, what remains, how a tower session runs,
and when to escalate. It does not replace the product spec — anything that
changes behaviour rather than re-platforming it still goes through
[DuskStudio.md](../DuskStudio.md) (read the relevant part; it is ~30k tokens, so
chunk it).
The per-tower executable spec for the current tower is a sibling doc (see
"Remaining towers" below). Live session-to-session state (current branch, phase
status, gate count) lives in the memory ledger
`project_dejuce_roadmap.md` — read that for *today's* state; this file is the
stable background.

## Goal and metric

Remove **all** JUCE from Dusk Studio by 1.0, incrementally, tower by tower.
The north-star metric is **JUCE modules unlinked from CMake**, not
files-off-allowlist. Currently linked (12):

`juce_audio_basics, juce_audio_devices, juce_audio_formats,
juce_audio_processors, juce_audio_utils, juce_core, juce_data_structures,
juce_dsp, juce_events, juce_graphics, juce_gui_basics, juce_gui_extra`

A module leaves the link line only when its whole subsystem is reimplemented
and every call site is flipped. `juce_core` unlinks last. The
`tools/juce-gate.sh` ratchet (allowlist 184 files) is the file-level
backstop, not the goal.

Metric caveat: JUCE module declarations pull dependencies transitively —
e.g. `juce_audio_utils` depends on `juce_audio_devices`, so removing the
direct `juce_audio_devices` link still leaves the module compiling until
`juce_audio_utils` itself unlinks. "Unlinked" for a tower means the direct
link line is gone AND zero `src/` translation units include the module's
headers on the platform in question; the object code leaves the binary when
the last transitive dependent goes.

Platform scope: a module is **globally unlinked** — and only then does the
north-star count above drop — once its direct CMake link is removed on
**every** shipping platform (Linux, macOS, Windows). A tower that removes a
link on one platform while the others keep the JUCE path scores a
**platform-scoped** unlink (e.g. "Linux-unlinked"): report that progress
explicitly, but the count stays put until macOS and Windows follow.
Phase-3-audio is exactly this shape — `juce_audio_devices` goes
Linux-unlinked while mac/win keep the JUCE device path, so the count of 12
does not move; it drops to 11 only when the mac/win path is also
reimplemented.

## Done (condensed — full history in memory `project_dejuce_history.md`)

- **Phase 0**: CI juce-token ratchet gate + allowlist + Catch2 A/B harness.
- **Foundation floor**: `dusk::text` / `dusk::fs` (incl. special locations) /
  `dusk::json`; integer-typedef sweep; jmax/jmin/jlimit fully gone from src/
  (math sweep, ~1080 sites).
- **Audio file I/O**: libsndfile FileReader/Writer/ThreadedFileWriter
  (`src/engine/audiofile/`), A/B bit-exact vs JUCE's WavAudioFormat.
- **Audio math**: `dusk::audio` Decibels / SmoothedValue / ScopedNoDenormals,
  A/B bit-exact vs JUCE.
- **DSP tower**: `duskaudio::Biquad` + donor halfband FIR oversampler adopted
  as reference (deliberate re-voice, tests re-baselined);
  `dusk::audio::StereoOversampler` + `IntDelayLine`; MasteringDigitalEq,
  BrickwallLimiter, LoudnessMeter, BusStrip/ChannelStrip/MasterBus flipped
  onto donor DPF cores.
- **Threading**: `dusk::AutoResetEvent` + std::thread in the multicore worker
  pool; native SCHED_RR priority.
- **MIDI content model**: `dusk::MidiBuffer`/`MidiMessage` (byte-view);
  MTC/clock/MCU receivers + MTC emitter flipped; hosting MIDI path (in-process
  CLAP/LV2/VST3 + OOP IPC) fully dusk.
- **MIDI device seam + native backend (M1–M3, PRs #93/#94/#98)**:
  `duskstudio::midi::{MidiInputClient,MidiOutputBank}`
  (`src/engine/midi/MidiDevices.{h,cpp}`) runs on `IMidiInput/OutputBackend` —
  per-input `dusk::MidiCollector` over an SPSC ring, slot queue + `std::thread`
  pump, `std::string` device info. Linux = `AlsaSeqMidi` (snd_seq_*);
  everything else = `JuceMidiBackend`, now the only JUCE MIDI device API left.
  Killed the last `juceManager()` consumer on Linux. Two caveats that outlive
  the tower: `juce_audio_devices` did NOT unlink (dual-gated with
  Phase-3-audio, gate stayed at 194), and the mac/win fallback has never been
  executed — it is not compiled on Linux.
- **Events seam**: `dusk::Timer` + `dusk::callAsync`
  (`src/foundation/MessageThread.h`); McuController flipped.
- **Transport seam**: `dusk::TransportPosition` in hosting.
- **PipeWire backend**: native `src/engine/pipewire/` device (streams on real
  hardware; JUCE-JACK shim deleted). Native ALSA backend already existed.
- **Device tower P1–2c**: `dusk::audio` device interfaces + `DeviceManager`
  seam (`src/engine/device/`); AudioEngine + all audio consumers flipped;
  `juceManager()` escape hatch survives with exactly one MIDI consumer.
- **Misc clean-ups**: SF2 reader, scanners, RecentSessions/AppConfig, String
  leaf batches, ChordAnalyzer, hosting-transport cluster.
- **Device Phase-3-audio (P0–P5, PRs #103/#104/#105, merged 2026-07-22)**:
  native PipeWire/ALSA backends implement `IODevice`/`IODeviceType` directly;
  `DeviceManager` is a native orchestrator on Linux (CallbackFanout,
  DeviceStateBlob JSON state with one-way legacy-XML migration); ALSA I/O on
  `std::thread` + SCHED_RR with timed-join/detach-and-park teardown;
  `juce_audio_devices` is Linux-unlinked (direct link + all Linux src
  includes gone; mac/win keep the wrapped path in `DeviceManagerJuce.cpp`, so
  the global count stays 12 per the platform-scope rule). Deliberate
  behaviour change: no auto-open on backend switch. Allowlist 194 → 184.
  Bench debts outstanding at merge: ALSA hot-unplug mid-play, xrun recovery
  under load, buffer/SR swap, periods knob, PipeWire streaming + SR/quantum
  renegotiation (UMC1820 perf matrix green pre-merge). Spec:
  [dejuce-device-phase3-audio-plan.md](dejuce-device-phase3-audio-plan.md).
- **Audio-file call-site sweep (A0–A4, PR #107 + the sweep-2 PR)**: every
  production consumer of the JUCE audio-format APIs
  (record/playback/bounce/importers/mastering/mp3) runs on the dusk
  libsndfile seam, unconditionally on all platforms (libsndfile is a hard
  configure requirement; no fallback branch). New RT primitives:
  `BufferedFileReader` (last-position prefetch window, timeout-0 parity),
  `IFileWriteSink` + externally-drained `ThreadedFileWriter` +
  `WriterDrainPool` (one disk thread per subsystem, as before). LameMp3Writer
  is JUCE-free (allowlist −2, gate 182 — the "zero movement" honest-yield
  prediction missed this). `juce_audio_formats` stays linked only for
  `juce_audio_utils`/AudioThumbnail; it unlinks globally with the GUI tower.
  Bench debts at spec §Owed. Spec:
  [dejuce-audiofile-plan.md](dejuce-audiofile-plan.md).

## Remaining towers, in order

1. **Plugin-hosting JUCE fallback drop** — native CLAP/LV2/VST3 become the
   only hosts; delete `juce::AudioPluginInstance`/`AudioProcessor` hosting
   path (PluginSlot/PluginManager/PluginHostMain). Depends on donor DPF ports
   finishing (APVTS gone) and Marc's call on dropping JUCE-plugin-format
   support. Unlinks `juce_audio_processors`/`juce_audio_utils`/
   `juce_audio_formats` territory.
2. **FFT** — DpAligner + MasteringEqEditor onto pffft/kissfft (lib decision
   open). No ratchet movement, low priority; batch with whichever tower
   touches those files.
3. **Events remainder + GUI tower (finale)** — ~40 UI juce::Timer subclasses,
   ~79 callAsync sites, then the bespoke Wayland toolkit (xdg-shell +
   XWayland/XEmbed for plugin UIs, 2D renderer TBD Blend2D/Skia,
   HarfBuzz/FreeType, own widget layer; swap behind the existing
   DuskComboBox/DuskContextMenu/LookAndFeel/EmbeddedModal seams). ~120 files,
   50%+ of total effort, multi-release. Unlinks everything else, `juce_core`
   last.

Dissolved tower — **String-floor remnants** (2026-07-22 scout): zero files in
session/engine scope can go fully clean on a String/File flip; every
candidate is anchored by a heavier type (Session model by juce Colour,
importers/playback by AudioFormat, hosting surfaces by PluginDescription,
edit actions by UndoableAction, MidiBindings by gui_basics menus,
CrashHandler by Logger/SystemStats). The string work rides along inside
whichever tower removes each file's anchor; no standalone sweep.

## Standing workflow ritual (every tower session)

1. **Branch first.** Never commit on main; slips have happened. One branch per
   tower.
2. **One PR per tower, one tower at a time.** Batch sub-phases as commits on
   the tower branch; push once, on Marc's go. Don't start tower N+1 before N
   merges (CodeRabbit reviews one PR at a time; 45-min cooldown). Split a
   tower into multiple PRs only if >15 files or RT-risky work is mixed with
   mechanical work.
3. **Commit, never push.** Marc reviews every commit locally, then says push.
   Never force-push main. No attribution trailers of any kind on commits/PRs.
4. **The gate** (`tools/juce-gate.sh`): clean files must stay clean; the
   literal `juce`-colon-colon token in a *comment* trips it (write "JUCE's
   Foo"); don't add more JUCE to allowlisted files either — the gate is
   silent there but review is not.
5. **Tests per layer swap**: A/B-vs-JUCE parity (JUCE linked in the test
   target only) or re-baselined property tests where divergence is accepted +
   silence-in/silence-out. Narrow-link Catch2 in `tests/`, build with
   `build-tests/`, run ctest. RT primitives get deterministic unit tests
   (injectable clocks, no sleeps).
6. **Verification bar**: app build zero new warnings; ctest green; for
   audio-path changes `DUSKSTUDIO_RUN_SELFTEST=1` — **always under Xvfb on a
   private DISPLAY with WAYLAND_DISPLAY unset, never on live Wayland** (it
   crashes the session).
7. **Sync-primitive swaps carry TSan baggage**: replacing a JUCE primitive
   that has a `tools/tsan_suppressions.txt` entry needs the equivalent dusk
   entry in the same PR, notify-under-lock, and awareness that TSan may emit
   mutex/deadlock/race report classes for one primitive. CI TSan failures are
   contention-only — read the CI log, don't chase local repro.
8. **Before commit**: AI-slop sweep the diff (strip change-narration comments,
   dead code, on-spec defensiveness); check MANUAL.md if anything
   user-visible moved; re-read files before editing if the session is long.
9. **Hardware sign-off is a first-class TODO**: anything touching MIDI timing,
   device failure paths, or RT scheduling gets a named "owed to Marc's bench"
   line in the PR description rather than a silent assumption.

## Model / token policy (why this doc exists)

Towers are executed by **Opus sessions** (`/model claude-opus-4-8[1m]`),
not Fable. The specs are written so execution is mechanical.

- **Session recipe**: read this doc → read the current tower spec → read the
  memory ledger (`project_dejuce_roadmap.md`) → re-read the files you're
  about to edit → execute ONE phase → verify (bar above) → commit → update
  the ledger (state, not prose) and the spec's status line → end with a
  resume phrase for the next session.
- **Fresh session per phase.** Don't grind a deep-context session into a new
  phase; hand off instead.
- **Delegate**: use the cavecrew subagents (investigator = locate,
  builder = bounded 1–2-file edits, reviewer = diff review) — their output is
  compressed; use Explore agents for wide read-only scouting.
- **Escalate to Fable only for**: (a) the Phase-3-audio kickoff plan,
  (b) an RT design question the spec doesn't answer, (c) an A/B test that
  won't converge after honest effort, (d) a novel CI TSan report class.
  Everything else: proceed on the spec, or park the question in the ledger
  for Marc.
