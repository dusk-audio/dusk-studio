# De-JUCE tower spec — Audio-file call-site sweep (juce_audio_formats consumers → dusk libsndfile seam)

**STATUS: PR-1 READY — A0+A1 DONE on branch `dejuce/audiofile-sweep-1`, plus
libsndfile made a hard requirement on every platform (CI installs it in all
workflows) and every `DUSKSTUDIO_HAS_AUDIOFILE` guard + JUCE fallback branch
deleted: consumers call the seam unconditionally, so the flips are NOT
Linux-scoped. Resume after merge: "A2 BufferedFileReader on
dejuce/audiofile-sweep-2 after PR-1 merges" — write A2/A3 code WITHOUT any
platform gate.**
Update this line each session (phase done, branch, resume phrase).

Read order for an executing session: `docs/dejuce-campaign.md` → this file →
memory ledger `project_dejuce_roadmap.md` → the files of the ONE phase you are
about to execute. Execute one phase per session, then stop.

## Goal and honest yield

Flip every production consumer of JUCE's audio-format APIs
(AudioFormatManager/Reader/Writer, BufferingAudioReader,
AudioFormatWriter::ThreadedWriter, WavAudioFormat) onto the dusk libsndfile
seam in `src/engine/audiofile/` (`dusk::audio::FileReader` / `FileWriter` /
`ThreadedFileWriter`).

**Honest yield, stated up front:** ZERO allowlist movement and NO module
unlink from this tower alone. Every consumer keeps other JUCE (AudioBuffer,
File/String, TimeSliceThread, dsp, GUI), and `juce_audio_utils`
(AudioThumbnail) transitively keeps `juce_audio_formats` compiling until the
GUI tower. The deliverable is: zero production call sites on the JUCE
audio-format APIs, so `juce_audio_formats` becomes unlinkable the moment
`juce_audio_utils` goes (GUI tower), with no residual work. That unlink will
be global rather than platform-scoped: libsndfile is required on every
platform, so no JUCE fallback path survives anywhere. The gate ratchet does
not move; do not chase it.

**Out of scope (do not pull in):** `juce::MidiFile` (FileImporter,
MainComponent — juce_audio_basics MIDI content, dies with the events tower);
`AudioThumbnail`/`AudioThumbnailCache` (AudioRegionEditor, MasteringView —
juce_audio_utils, GUI tower); the resamplers `WindowedSincInterpolator` /
`LagrangeInterpolator` (juce_audio_basics — keep them; a native SRC is its
own decision later); `juce::TimeSliceThread` where it survives for other
uses.

## Verified ground truth

1. **The seam is JUCE-free but TEST-ONLY.** `src/engine/audiofile/*.cpp` is
   compiled only into the test binary and libsndfile is linked only there
   (tests/CMakeLists.txt:171-185). The app target neither compiles the seam
   nor links sndfile. A0 fixes this first.
2. **Seam API:** `FileReader::open(path)` → `FileInfo{sampleRate,
   numChannels, numFrames, bitsPerSample}`, deinterleaved
   `read(float* const*, numDestCh, startFrame, numFrames)` (NOT RT-safe —
   message/disk thread only). `FileWriter::create(path, WriteSpec{sr, ch,
   bits 16/24/32float, Wav|Flac|Aiff})`, `write()` (allocates),
   `writeInterleaved() noexcept` (RT drain path), `flush()`. MP3 write
   deliberately absent (stays on the libmp3lame path).
   `ThreadedFileWriter(unique_ptr<FileWriter>, fifoFrames)`:
   `push(float* const*, numCh, numFrames) noexcept` — lock-free SPSC
   interleaved ring, alloc-free, drops-and-returns-false on
   overflow/failed-writer; one dedicated `std::thread` per instance, 2 ms
   poll, `writeFailed` latch, flush on clean drain, dtor joins.
3. **A/B already proven:** tests/audiofile_ab_null.cpp pins seam WAV output
   bit-exact vs JUCE's WavAudioFormat. tests/audiofile_roundtrip.cpp covers
   drain + overflow-drop.
4. **JUCE decode surface today is WAV/AIFF (+FLAC only where StartupDialog
   registers it explicitly).** No JUCE_USE_MP3/OGG flags anywhere in CMake.
   libsndfile matches or exceeds this — no decode-capability regression.
5. **RecordManager RT contract:** `writeInputBlock` (RecordManager.cpp:
   778-804, noexcept) pushes to `AudioFormatWriter::ThreadedWriter::write`
   (lock-free FIFO, drop-on-overflow=false counted as writeFailures). FIFO
   depth `max(65536, sampleRate*4)` samples (:177). Seam `push` has the same
   contract; it takes deinterleaved float directly (no int conversion).
6. **AudioEngine RtBounceSink** (AudioEngine.h:325, callback push at
   AudioEngine.cpp:~4842) uses the same ThreadedWriter push semantics.
7. **PlaybackEngine/MasteringPlayer read path is RT:** per-region
   `BufferingAudioReader(raw, thread, 96000)` + `setReadTimeout(0)` — audio
   thread read NEVER blocks, returns silence on prefetch miss. PlaybackEngine
   additionally keeps a manual loop pre-cache (loopCacheL/R,
   PlaybackEngine.h:95-104) because forward-only prefetch misses the
   backward seek at loop wrap. The seam HAS NO buffered reader — building
   one is this tower's only new RT primitive (B1).
8. **BounceEngine writer polymorphism:** WAV writers and LameMp3Writer
   (subclasses juce::AudioFormatWriter) both feed
   AudioFormatWriter::ThreadedWriter over ONE shared TimeSliceThread
   (BounceEngine.cpp:796,811,826, fifo 1<<17). The seam's ThreadedFileWriter
   wraps only a concrete libsndfile FileWriter — B2 closes this.
9. **Thread-count delta:** JUCE shares one disk thread across all threaded
   writers; the seam spawns one std::thread per ThreadedFileWriter. Naive
   record flip = up to 24 disk threads. B3 decides this.
10. **FileInfo gap:** no `isFloat` flag (StartupDialog.cpp:96 reads
    `usesFloatingPointData`). Add the field in A1.
11. **CMake:** juce_audio_formats linked at CMakeLists.txt:1058 (app), :764
    (plugin-host child), tests/CMakeLists.txt:260. All three STAY linked this
    tower (transitive juce_audio_utils dependency + tests use JUCE for A/B).

## Design decisions (B1–B3)

### B1 — dusk::audio::BufferedFileReader (the one new RT primitive)

New seam class wrapping `FileReader` with JUCE-BufferingAudioReader-parity
semantics, because PlaybackEngine and MasteringPlayer read on the audio
thread:

- Fixed prefetch window (default 96000 frames, ctor arg) filled by a
  background thread; `readRt(float* const* dest, numDestCh, startFrame,
  numFrames) noexcept` on the audio thread copies from the window under a
  try-lock/seqlock discipline and **zero-fills any span not resident**
  (timeout-0 parity — never blocks, never does I/O). Return value reports
  full/partial residency so callers can count misses if they care (JUCE's
  API returned bool; keep bool = fully-resident).
- Prefetch thread follows the highest `startFrame` seen (forward-only, JUCE
  parity); an explicit `prefetch(startFrame)` hint lets `preparePlayback`
  and loop-wrap logic pre-warm — PlaybackEngine's manual loopCacheL/R stays
  as-is (do NOT fold it into the reader; it exists precisely because
  forward-only prefetch can't cover loop wraps, and it works).
- One background std::thread per BufferedFileReader, normal priority,
  condition-variable wakeup on hint/position change, joined in dtor.
  AutoResetEvent reuse fine. No allocation after construction (window sized
  in ctor).
- Deterministic unit tests, no sleeps: expose a `fillNow()` test hook (or
  injectable "synchronous prefetch" mode) so tests drive residency
  explicitly — miss ⇒ silence + false; resident ⇒ bit-exact vs raw
  FileReader; hint warms a backward span.

### B2 — writer-sink interface for the threaded drain

Minimal abstraction so WAV/FLAC (libsndfile) and MP3 (LAME) share the
threaded path: extract `dusk::audio::IFileWriteSink` (pure virtual:
`writeInterleaved(const float*, int64_t frames) noexcept-ish`, `flush()`,
`info` needed by the ring: numChannels). `FileWriter` implements it.
`LameMp3Writer` re-bases from juce::AudioFormatWriter onto IFileWriteSink
(it already encodes from float internally; drop the juce::OutputStream ctor
for a plain path/FILE*). `ThreadedFileWriter` takes
`unique_ptr<IFileWriteSink>`. Keep the interface EXACTLY these members —
no metadata, no seek, no format negotiation (nothing uses them; add when a
caller exists).

### B3 — shared drain service for many writers

Record (up to 24 tracks) and bounce (N stems) must not spawn a thread per
writer. Add `dusk::audio::WriterDrainPool`: one std::thread draining a
fixed-capacity registry of ThreadedFileWriter rings round-robin (2 ms poll,
same cadence as today's single instance; registry mutations
message-thread-only under mutex; the drain loop snapshots under the same
mutex briefly — never touched by the audio thread, which only pushes to
SPSC rings). ThreadedFileWriter grows a constructor mode "externally
drained" (its own thread not started; pool calls its drainOnce()). Existing
single-writer behaviour (self-draining) stays for one-off consumers.
RecordManager and BounceEngine each own one pool instance (mirrors today's
one diskThread each). RtBounceSink drains via BounceEngine's pool.

## Phase plan

Two PRs (>15 files, RT mixed with mechanical). PR-1 = A0+A1 (branch
`dejuce/audiofile-sweep-1`): wiring + one-shot mechanical flips. PR-2 =
A2+A3 (branch `dejuce/audiofile-sweep-2`, after PR-1 merges): RT read path,
then RT write path.

### A0 — seam into the app build (mechanical, small)

- CMakeLists.txt: compile `src/engine/audiofile/*.cpp` into DuskStudio, find
  + link sndfile for the app and the tests. **libsndfile is a hard
  requirement on every platform** (configure fails without it) — CI installs
  it via `libsndfile1-dev` (Linux, incl. the Raspberry Pi job), `brew install
  libsndfile` (macOS), `vcpkg install libsndfile:x64-windows-static`
  (Windows, same /MT triplet as mp3lame). There is therefore **no
  `DUSKSTUDIO_HAS_AUDIOFILE` gate and no JUCE fallback branch**: consumers
  call the seam unconditionally on all platforms. The
  `dusk-studio-plugin-host` child target doesn't consume the seam; leave it.
- No call sites change. Verify: full build all platforms via CI, ctest, gate
  unchanged.

### A1 — one-shot reader/writer flips (mechanical, message/worker thread only)

No RT paths. Per file: reader open→info/read swaps, WAV-writer create→write
swaps. Keep everything else (AudioBuffer scratch, resamplers, MidiFile,
Thread::sleep) untouched.

- `src/engine/FileImporter.cpp` (reader :105 + 24-bit WAV writer :224-226;
  keep WindowedSinc + MidiFile), `src/engine/DpImporter.cpp` (metadata
  reader :46-56, manager :417), `src/engine/DpAligner.cpp` (full decode
  :17-40), `src/session/RegionEditActions.cpp` (join :690-734, reverse
  :863-884), `src/ui/StartupDialog.cpp` (metadata columns :84-96 — needs the
  new `FileInfo::isFloat` field; add it to FileReader in this phase with a
  roundtrip test), `src/ui/ScreenshotCapture.cpp` (:109-113),
  `src/DuskStudioApp.cpp` (selftest WAV :1176-1293),
  `src/ui/MainComponent.cpp` (reader peeks :3514/:4225 + makeStereoTempWav
  :3763-3775; leave MidiFile peeks + importAudioFormatManager if the
  thumbnail/GUI paths still need it — flip only what detaches cleanly),
  `src/engine/multisample/Sf2ToSfz.cpp` (:108-109, 16-bit mono).
- Split across two sessions if it runs long (A1a engine/session, A1b ui/app).
- Test gate: existing importer/aligner/region suites stay green
  (dp_importer_parse, dp_aligner_align, file_importer_audio,
  session_round_trip et al); new FileInfo::isFloat test; build + ctest +
  Xvfb selftest.

### A2 — BufferedFileReader + playback/mastering flips (RT-RISKY, read path)

- NEW `src/engine/audiofile/BufferedFileReader.{h,cpp}` per B1 + NEW
  `tests/audiofile_buffered_reader.cpp` (deterministic residency tests, A/B
  vs raw FileReader, backward-hint case, miss⇒silence+false).
- Flip `src/engine/PlaybackEngine.{h,cpp}` (BufferingAudioReader h:70/
  cpp:129, reads :243/:408; loop pre-cache logic unchanged) and
  `src/engine/MasteringPlayer.{h,cpp}` (h:73, cpp:98/:188; keep
  LagrangeInterpolator). TimeSliceThread members die where these were their
  only users.
- Test gate: playback_loop_read, mastering_player_resample green; new
  buffered-reader suite; Xvfb selftest; dev-box audible smoke (play a
  session with loop wrap, mastering playback at mismatched SR).

### A3 — threaded write path: sink interface + drain pool + record/bounce
      (RT-RISKY, write path)

- Seam: `IFileWriteSink` + ThreadedFileWriter externally-drained mode +
  `WriterDrainPool` per B2/B3 + tests (pool drains N rings, failure latch
  per writer, registry add/remove while draining).
- `src/engine/LameMp3Writer.{h,cpp}` re-bases onto IFileWriteSink (drop the
  juce AudioFormatWriter base + OutputStream).
- `src/engine/RecordManager.{h,cpp}`: per-track ThreadedFileWriter
  (fifo = max(65536, sr*4) frames as today) drained by one pool;
  writeInputBlock pushes deinterleaved directly; writeFailures semantics
  identical (push false = failure count).
- `src/engine/BounceEngine.{h,cpp}`: makeWriter/openWriterFor return sinks;
  stems + MP3 through pool; WaitableEvent/Thread machinery otherwise
  untouched.
- `src/engine/AudioEngine.{h,cpp}`: RtBounceSink holds ThreadedFileWriter*
  (push in callback unchanged in shape).
- Test gate: recording_byte_accuracy, bounce_stem_targets/filename,
  lame_mp3_writer suites green (re-point harnesses where they construct
  JUCE writers); audiofile_ab_null still proves WAV bit-parity; Xvfb
  selftest; dev-box record smoke (arm several tracks, record, verify files)
  + bounce smoke (multi-stem + mp3). DUSKSTUDIO_BOUNCE_TEST harness run.
- Bench debt (named in PR-2): long-form multitrack record session on real
  hardware (disk-pressure overflow behaviour), realtime bounce with HW loop.

### A4 — sweep residue + docs (mechanical, part of PR-2)

- Delete now-dead juce includes/members in flipped files (WavAudioFormat,
  AudioFormatManager, BufferingAudioReader, ThreadedWriter, TimeSliceThread
  where orphaned). juce_audio_formats link STAYS (transitive + tests).
- docs/dejuce-campaign.md: tower entry → done line (with the honest-yield
  caveat restated); MANUAL.md audit (expected no-op); ledger + STATUS.

## Risk register

| # | Risk | Phase | Mitigation |
|---|------|-------|------------|
| 1 | Audio-thread read regression (buffered reader misses where JUCE prefetched) | A2 | Timeout-0 parity semantics; loop pre-cache untouched; deterministic residency tests; audible dev-box smoke |
| 2 | Record drop behaviour delta under disk pressure | A3 | Same fifo depth, same drop-and-count contract, pool cadence = today's 2 ms; recording_byte_accuracy; bench debt named |
| 3 | Thread-count explosion (writer per thread) | A3 | WriterDrainPool (B3) — one thread per subsystem, as today |
| 4 | MP3 path breaks when LameMp3Writer leaves juce base | A3 | IFileWriteSink minimal; lame_mp3_writer test; bounce mp3 smoke |
| 5 | mac/win sndfile availability | A0 | Resolved: sndfile installed in every CI workflow and required at configure time on all platforms, so the flips are unconditional (no dual paths) |
| 6 | Silent decode-capability change | A1 | Ground truth 4: JUCE surface was WAV/AIFF(+FLAC); libsndfile ⊇ that; importer tests pin behaviour |
| 7 | Scope creep into resamplers/MidiFile/thumbnails | all | Out-of-scope list up top; reviewers reject on sight |

## Owed to Marc's bench (PR-2)

- Long multitrack record on real hardware (disk pressure, overflow counters).
- Realtime bounce with the HW loop; MP3 stem listen-check.
- Loop-wrap playback + mastering playback at mismatched SR, audible check.

## Opus session prompts

Common preamble: as in the device-tower spec (docs/dejuce-device-phase3-audio-plan.md
§Opus session prompts) with this file as the spec. One phase per session.

- **A0**: "Phase A0 on new branch `dejuce/audiofile-sweep-1`: wire
  src/engine/audiofile/ into the app build per spec §A0. No call-site
  changes. All-platform CI is the verification (push only on Marc's go).
  One commit."
- **A1**: "Phase A1 on `dejuce/audiofile-sweep-1`: one-shot reader/writer
  flips per spec §A1 file list. Add FileInfo::isFloat first (with test).
  Split A1a/A1b if long. Keep resamplers/MidiFile/thumbnails untouched
  (§Out of scope). Then prepare the PR-1 description and stop."
- **A2**: "Phase A2 on new branch `dejuce/audiofile-sweep-2` (verify PR-1
  merged first): BufferedFileReader per spec §B1 + §A2, then flip
  PlaybackEngine + MasteringPlayer. Deterministic tests, no sleeps. Dev-box
  audible smoke. One commit."
- **A3**: "Phase A3 on `dejuce/audiofile-sweep-2`: IFileWriteSink +
  WriterDrainPool per §B2/§B3, re-base LameMp3Writer, flip RecordManager /
  BounceEngine / RtBounceSink per §A3. Run the bounce-test harness. One
  commit."
- **A4**: "Phase A4 on `dejuce/audiofile-sweep-2`: residue sweep + docs per
  §A4. Prepare the PR-2 description (bench debts from §Owed). Stop."
