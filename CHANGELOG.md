# Changelog

All notable changes to Dusk Studio. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Pre-1.0 entries are
back-filled from `git log`; once tags exist this file is the
canonical source.

## [0.11.0] - 2026-06-12

Built to a production-grade bar; shipped as a Beta. 1.0.0 is reserved for
the public stable declaration.

### Added

- **Out-of-process plugin hosting by default.** Third-party plugins run in
  a sandboxed child process on all three OSes — a crashing plugin no longer
  takes the session down. Plugin instantiation moved off-thread, so loading
  a heavy synth doesn't freeze the UI.
- **Automatic cross-track plugin delay compensation (PDC).** Tracks with
  latency-reporting inserts stay sample-aligned with the rest of the mix.
- **Undo, broadly.** Piano-roll note edits, automation breakpoint edits,
  tape-strip menu edits, tempo-map edits, and track / bus / aux rename +
  colour changes are all undoable.
- **Tempo editing from the ruler.** Right-click the bar ruler to add / edit
  tempo points, drag markers left/right to move them, wider hit zones, and
  the bar-1 anchor stays protected. The separate Grid edit mode is gone.
- **Timeline interaction pass.** Left-click moves the playhead (snapped to
  grid when Snap is on), drag on the ruler sets loop / punch ranges, and the
  Snap on/off + grid-resolution control is back in the tape-strip header.
- **MIDI Learn for per-track EQ frequency + Q** (band gain was already
  mappable).
- **Arrow-key focus across the 24 channel strips**, plus stage / bank
  keyboard shortcuts, tooltips, and an in-app shortcut reference.
- **Song-section display.** The transport shows the current section name
  (from markers) next to the clock.
- **Opt-in parallel strip DSP** across worker threads
  (`DUSKSTUDIO_AUDIO_WORKERS`) for heavyweight sessions on many-core
  machines.
- **Multi-output routing (Tascam-style cue/monitor sends).** The audio device
  can open more than two outputs (Audio settings). Each AUX return lane can be
  routed to its own physical output pair for an independent headphone / cue mix,
  fed by the existing per-channel pre-fader sends; the tap is taken before the
  return fader and mute, so those still govern only the fold into the master.
- **Selectable main output pair.** The master mix can be sent to any open output
  pair (Audio settings → Main output), freeing 1-2 for a control-room or cue
  feed. An aux routed to the master's pair sums in.
- **DP-24-style multiband compressor presets.** Nine genre starting points
  (Basic CD, Pop, Pop Rock 1/2, Rock 1/2, Classic, Dance, R&B Hip Hop) in the
  mastering comp header, mapping the three-band chart onto the multiband comp
  with the high-mid band disabled.
- **Piecewise tempo map (Grid edit mode).** Songs can change tempo over time:
  add / drag tempo points on the bar ruler in Grid mode; the bar ruler, grid
  snap, region-editor ruler, BPM control, piano roll, MIDI scheduler and
  metronome all follow the map.
- **Limiter Mode + Stereo-link.** Mastering limiter gains a Mode selector
  (Modern / Transparent / Punchy) shaping hold + release, and a Stereo-link
  toggle (linked GR vs per-channel).
- **Punch pre-roll / post-roll enable toggles** — arm playback before the
  in-point and auto-stop after the out-point.
- **Open a session from the command line / file manager.** Positional session
  path on launch and a running-instance hand-off, with the `.desktop` MIME
  association restored.
- **Portable Linux tarball + installer** replacing the AppImage: a
  run-in-place program directory plus an `install.sh` that registers the
  launcher / icon / MIME (the Reaper model).
- **Raspberry Pi (arm64) build-check CI.**

### Changed

- **Sessions are portable.** Audio file paths are stored relative to the
  session folder; loading re-roots stale absolute paths and lists any file
  it genuinely can't find instead of silently playing silence. Renaming the
  session folder or moving it between machines now just works.
- **Disengaged EQ / comp / tape sections skip their DSP** with click-free
  toggles — lighter CPU on mostly-clean mixes.
- **Take history capped at 8 per region** (was 20).
- **Stage tabs reordered** to RECORDING | MIXING | MASTERING | AUX.
- **Import... renamed to Import Audio or MIDI...**
- **Mastering limiter rebuilt as a true-peak brickwall limiter.** 4×
  oversampled lookahead limiting with a hard inter-sample-peak ceiling,
  monotonic-min gain envelope, and hold + smooth release — replaces the
  previous sample-peak design that pumped like a compressor.
- **Limiter "Threshold" control now makes sense.** Pulling the handle down
  drives more signal into the ceiling (louder, denser) instead of only
  attenuating.
- **Mastering layout rebalanced.** The starved limiter panel gets more width,
  the loudness readouts (TP / M / S / I) group into one cluster, and Export
  master moves up to the header beside Load latest mixdown.
- **Tape-strip track-label column auto-sizes** to the longest track name
  instead of clipping at a fixed width.
- Release CI (Linux / macOS / Windows) gates publishing on the test suite;
  version metadata corrected (24-channel, 0.11.0).

### Fixed

- **Data-integrity hardening.** Session save is now genuinely atomic (a
  crash mid-save can no longer lose both the old and new file); recovering
  from an autosave immediately persists the recovered state instead of
  silently discarding it on quit; a fast stop + re-record within the same
  second can no longer overwrite the previous take's audio.
- **Crash fixes.** Stopping the transport during dense playback could
  free disk readers under the audio callback; quitting with the audio
  settings, plugin scan, or a bounce in progress could tear down dialogs
  after the engine was gone. Both classes closed.
- **Real-time safety.** MIDI clock / MTC and per-track MIDI output are
  delivered through a dedicated thread instead of locking inside the audio
  callback (periodic xruns while rolling with MIDI out engaged); the
  Mastering player no longer reads from disk on the audio thread.
- **Send-effect tails.** Aux reverb / delay tails are no longer cut off
  the moment the dry signal stops.
- **Loading an older session over a newer one** no longer inherits the
  previous session's hardware-insert enable, automation mode, or markers.
- **The chosen audio device setup persists across restarts.**
- **Changing the oversampling factor actually re-prepares the DSP** without
  a full device restart.
- **Declining to save on quit discards the autosave**, so the next launch
  doesn't offer to "recover" the work you chose to drop.
- **Smoother playhead band repaint** (driven by the display vblank).
- **Windows MSI creates Start Menu + desktop shortcuts.**
- **AUX tab no longer flashes plugin-editor windows when opened.**
- **Region editor's Draw tool paints automation without moving the region**,
  and edit-tool cursor glyphs match the OS cursor size.
- **Non-fatal X11 errors raised by plugin editors are swallowed** instead
  of terminating the app (Linux).
- **Mastering comp display.** Gain-reduction meters are now live during
  playback, and applying a preset refreshes the band knobs / crossovers (the
  embedded panel previously only synced on a manual band-click).
- **Audio settings layout** no longer overlaps the Main-output row with the
  sample-rate / buffer controls when many outputs are enabled.
- **Packaging scripts hardened**: gated the destructive `rm -rf` install dir,
  empty-VERSION and missing-source guards, and a special-character-safe rewrite
  of the desktop `Exec=` line.
- **Tempo-map real-time safety**: tempo-map edits during playback are now
  thread-safe and can't corrupt playback, and the bar-1 anchor is protected
  from deletion.
- **File import** retries transient file-lock opens (Windows).

## [0.10.0-beta.1] - 2026-05-29 — first beta

Architectural-audit sprint: every "Working" cell in the README's
Status table is now also reviewed for real-time safety, lifecycle
correctness, cross-platform CI coverage, and documentation parity.
24 commits since 0.9.0, 148 Catch2 tests green across Linux
(amd64 + arm64), macOS, and Windows.

### Added

- **MIDI binding targets (H3, Phase 5a + 5b)** — 10 new mappable
  destinations covering parts of the mixer that previously had no
  MIDI Learn surface:
  - Per-track toggles: EQ on/off, compressor on/off, hardware
    insert bypass
  - Bus EQ band gain (packed bus × band, 3 bands per bus)
  - Master Pultec EQ: low boost, high boost
  - Master bus compressor: threshold, makeup, ratio
  Bank-relative variants of every per-track target keep an 8-fader
  controller useful for the 24-track surface.
- **Accessibility (H4, Phase 4a)** — every channel-strip slider
  now has an accessibility title + domain-formatted value text.
  VoiceOver and Orca read e.g. *"Track 1 Fader, -4.2 dB"* on
  focus. Static text-input dialogs (rename region, rename marker,
  edit label) render inside the main window via the EmbeddedModal
  framework — no native popups that escape the screen reader's
  focus tree.
- **Hot-unplug detection (H5, Phase 4b)** — AudioEngine
  subscribes to AudioDeviceManager change broadcasts; an
  unexpected device disappearance force-stops the transport,
  surfaces a UI alert ("device disconnected"), and leaves the
  session intact in memory + on disk. Reconnect or pick a new
  device and playback resumes.
- **macOS in-process plugin editor + OOP DSP (C6, Phase 3c)** —
  dual-load shell pattern: parent process loads the plugin's
  editor in-process while the OOP child keeps running the DSP.
  Eliminates cross-process NSView reparenting (a research
  problem) and gives Mac users a working OOP plugin pipeline
  without the fragile CARemoteLayer dance. Bidirectional
  parameter mirror over IPC keeps the in-process editor and the
  OOP DSP instance in sync; an atomic loop-breaker flag prevents
  echo storms.
- **Linux arm64 build** — CI now matrices Linux on amd64 +
  arm64 (Raspberry Pi 4 / 5 territory). JUCE's NEON-side
  SIMDNativeOps<long long> alias is patched the same way the SSE
  side is.
- **ThreadSanitizer CI** — Linux sanitizer workflow runs the
  Catch2 suite under TSan on every PR + push. Standing JUCE-
  internal patterns (WaitableEvent / TimeSliceThread /
  AbstractFifo) are suppressed via `tools/tsan_suppressions.txt`;
  Dusk Studio code stays fully instrumented.
- **Windows tests workflow** — Catch2 suite now builds + runs on
  Windows MSVC every push. The IPC-stub round-trip test is
  temporarily hidden on Windows pending a named-pipe timing
  investigation.
- **Schema versioning + migration** — `SessionSerializer`
  format-version is now 2 (was 1). Loaders read v1 + migrate
  forward; tests pin both the safety branch (unknown lower
  version refused) and the v1 → v2 advance.
- **Hash-based autosave dedup (M8)** — autosave timer compares a
  32-bit hash of the volatile-state-stripped JSON instead of a
  full juce::String equality scan. Memory + CPU win on 24-track
  sessions.
- **Patreon + GitHub Sponsors pricing tiers** documented in
  README + MANUAL: $3/$5/$10 Patreon recurring, $27 one-time
  (current major), $49 lifetime (current + next major).
- **First-time-launch walkthrough** in MANUAL for unsigned
  binaries — per-OS macOS Gatekeeper + Windows SmartScreen
  bypass steps.
- **Windows ASIO-first device selection** — the AudioEngine
  pre-registers Windows backends in preference order (ASIO →
  WASAPI exclusive → WASAPI shared → DirectSound), so the default
  device lands on the lowest-latency backend that actually has
  devices. A machine with no ASIO driver falls through to WASAPI
  exclusive instead of an empty device list — onboard-audio users
  still get a usable low-latency default.
- **Out-of-process plugin scanning** — third-party plugin
  discovery (VST3 / LV2 / AU) runs in the `dusk-studio-plugin-host`
  child via a new `--scan` mode, so a plugin that crashes or hangs
  during discovery takes down only the child. The parent times the
  scan out (30 s), kills a hung child, and blacklists the file, so
  a single bad plugin can no longer crash the app on first scan.
  Backed by a dead-man's-pedal that quarantines a culprit if the
  app itself dies mid-scan. Falls back to in-process scanning when
  the host binary is absent.

### Fixed

- **Audio-thread parameter writes (C1, Phase 2)** —
  `PluginSlot::setParamNormalised` no longer calls
  `param->setValue` directly from the audio thread. Real plugins
  observed in the wild (Diva, Massive X, Spitfire BBC SO) take
  std::mutex inside their parameter-change paths. The audio
  thread now pushes into a 256-deep SPSC FIFO; a 30 Hz
  juce::Timer on PluginSlot drains the queue and calls
  setValueNotifyingHost on the message thread. Eliminates a
  long-standing class of CPU-spike-under-MIDI bug.
- **Atomic memory-ordering pairs (C2)** — `syncOutputIdx` audio-
  thread reader switched from relaxed to acquire so it pairs
  cleanly with the message-thread release stores in
  rebuildMidiOutputBank + AudioSettingsPanel.
- **RecordManager UAF on forced teardown (C4 + C5, Phase 1)** —
  `stopRecording` now bails the writer / midiCapture teardown
  when the bounded audioInFlight drain exceeds 1000 yields; the
  next startRecording gates on audioInFlight == 0 to avoid
  overwriting an in-flight slot. Closes a stuck-audio-thread
  UAF.
- **PluginSlot lifetime races (Phase 4c)** — five findings
  closed: ParamWrite epoch-guarded for plugin replace, OOP
  fallback in applyParamWriteOnMessageThread, dtor leak path
  when shell wrapper is outstanding, stale shellInstance on
  plugin swap, ChildParamListener atomic per-param state, shared
  SinkState lifetime for callAsync lambdas. Plus
  ChannelStripComponent shellEditor leak on factory failure.
- **macOS Authenticode helper, Windows SmartScreen bypass docs**
  match shipping behaviour (binaries are unsigned).
- **CI cross-platform issues** — macOS missing JUCE include,
  arm64 GCC `juce::String += unsigned int` ambiguity, MSVC
  M_PI undeclared (Catch2 transitive `<cmath>`), MSVC strict
  lambda capture for constexpr locals, donor-version-dependent
  test assertions hidden via Catch2 `[.]` tag.
- **Gain-reduction meter on silent tracks** — the per-track
  silent-skip fast path now zeroes the GR meter atom on its early
  return, so a silent track reads 0 dB reduction instead of
  holding the last computed value (matches the comp-bypass path).
- **ALSA RT priority with unlimited RLIMIT_RTPRIO** —
  `RLIM_INFINITY` previously narrowed to -1 under a signed cast and
  selected the *lowest* SCHED_RR priority (the opposite of intent);
  it is now treated as the top priority, with unsigned comparison
  otherwise, and logged as "infinity"/"unknown" rather than a
  narrowed int.
- **Plugin editors hidden under modals (all platforms)** — the
  macOS shell-editor and Windows foreign-HWND editor wrappers are
  now tagged so a settings / quit / bounce modal hides them.
  EmbeddedModal's hide is nesting-safe via a per-editor token, so
  closing modals out of order no longer re-shows an editor still
  covered by another modal.
- **Global shortcuts while a modal is open** — Home
  (return-to-zero), `.` (stop + rewind) and F11 (fullscreen) now
  reach the transport from a focused modal or call-out popup,
  alongside the existing Space / R. Destructive edit keys (Delete,
  clipboard, split, nudge) remain gated so they can't act on the
  arrangement hidden behind the modal.

### Changed

- **MANUAL audited** for every user-visible Phase 4 + 5 change.
  New sections: Installing Dusk Studio (per-OS first-launch
  walkthrough), Audio device disconnected mid-session
  (Troubleshooting), Accessibility.
- **README rewritten** with tier table, first-launch link,
  GitHub Issues bug-reporting line. Status-table cleanup
  (mac-OOP-editor now "Working" via the in-process shell;
  notarised DMG still deferred).

### Build & packaging

- **Windows MSI codesigning** (H10, Phase 6) — `scripts/
  package-windows.ps1` learned `-SigningPfxPath` + `-Signing
  PfxPassword` params (alongside the legacy thumbprint path).
  Signs every produced `.exe` BEFORE cpack assembles the MSI so
  the embedded binaries are signed inside the installer payload,
  then signs the MSI itself. windows-build.yml decodes a
  SIGNING_CERT_BASE64 secret into a temp file under
  $env:RUNNER_TEMP, runs the script with the PFX + password,
  zero-overwrites + deletes the temp file on completion
  (success OR failure). Default timestamp authority switched to
  digicert. NOTE: project policy as of v0.10 is to ship
  unsigned — signing infrastructure exists in CI but does not
  fire until secrets are configured.
- **Linux arm64 matrix** added to linux-build.yml. Tests still
  amd64-only to keep CI runtime reasonable.
- **Windows ASIO required for release builds** —
  `DUSKSTUDIO_REQUIRE_ASIO` turns a missing ASIO SDK into a hard
  configure error for release-class builds (single-config
  CMAKE_BUILD_TYPE = Release/RelWithDebInfo/MinSizeRel, and the
  Visual Studio multi-config generator whenever a release config is
  buildable), so a shipping Windows binary can't silently fall back
  to WASAPI-only latency. SDK auto-discovered at `../asiosdk` /
  `$ASIOSDK_PATH` / `-DASIOSDK_PATH=`; override with
  `-DDUSKSTUDIO_REQUIRE_ASIO=OFF` for a deliberate ASIO-less dev
  build (warns + falls back to WASAPI).
- **Plugin-scan wire protocol** extracted to a shared header
  (`PluginScanProtocol.h`) consumed by both the host child and the
  parent, with 5 Catch2 round-trip / crash-path tests.

## [0.9.0] - 2026-05-21 — beta, road to 1.0

### Added
- Phase 4 MIDI tempo semantics
    - `MidiRegion.tempoLock` + `MidiRegion.recordedAtBPM` fields
    - `applyTempoChange(Session&, newBpm, sampleRate)` retimes
      tempo-locked MIDI regions on BPM change; float regions keep
      sample positions and rebuild musical length
    - BPM-change confirmation alert that summarises retime impact
      before applying (skips when no MIDI / no automation)
    - FileImporter anchors imported MIDI regions to the session's
      current BPM so subsequent retime is correct
- `FadeShape::RaisedCosine` + 64-sample raised-cosine punch
  crossfade — replaces the previous 10 ms linear fade for
  click-mask boundaries
- Piano roll: triplet quantize variants (1/8T, 1/16T, 1/32T)
- Transport: `.` (period) stops + rewinds playhead to 0
- `docs/KEYBINDINGS.md` — complete keyboard reference

### Build & packaging
- Version sourced from top-level `VERSION` file
- `scripts/bump-version.sh` writes version + AppStream `<release>`
- CPack generators for `deb`, `rpm`, `WIX`, `DragNDrop`
- `scripts/package-appimage.sh` — Linux AppImage one-shot
- `scripts/package-linux.sh deb|rpm` — distro packages
- `scripts/package-windows.ps1` — MSI via WIX, optional signtool
- `scripts/package-macos.sh` — signed + notarized DMG
- `packaging/macos/entitlements.plist` — Hardened Runtime config
  for OOP plugin host

### Fixed
- Audio thread reads `tempoBpm` with `memory_order_acquire` on the
  MIDI scheduling path; pairs with `applyTempoChange`'s release
  store so a new BPM observation implies new region positions

### Tests
- `tests/session_midi_tempolock_roundtrip.cpp` — serializer
  round-trip + legacy session migration anchor
- `tests/session_apply_tempo_change.cpp` — 120↔60 retime, round
  trip 120-96-120, clamp 30..300
- `tests/transport_state_machine.cpp` — defaults, state
  transitions, playhead arithmetic, loop/punch round-trip
- `FadeShape::RaisedCosine` zero-slope-at-endpoints + 0.5-at-midpoint

### Deferred to post-1.0
- Piano roll: keyboard-strip preview audition (needs RT-safe MIDI
  inject FIFO)
- Piano roll: explicit Overdub toggle (recording already
  history-stacks; toggle is UX)
- macOS cross-process NSView plugin editor embedding (Mac plugin
  windows float as separate native windows; spec-acknowledged
  limitation)

## Pre-0.9 — captured from git log

See `git log` for the full history. Phase milestones:

- **Phase 1a** (live mixer): 16 strips, 4 aux, master Pultec + bus
  comp + tape sat
- **Phase 1b** (send-bus plugin hosting): aux plugin slots, OOP
  scaffolding
- **Phase 2** (recording + session): multitrack, atomic JSON
  save/load, autosave, transport
- **Phase 3** (arrangement + automation): markers, region edits,
  take history, fader/pan/aux/mute automation (Off/Read/Write/Touch),
  bounce/export, undo, **OOP plugin audio path on all 3 OSes**,
  console automation parity (aux + master)
- **MTC** (Phases 1-3): MIDI Time Code receiver + emitter +
  transport chase
- **Rename**: Focal → Dusk Studio (5 phases across identifiers,
  packaging, CMake, namespaces)

---

[0.9.0]: https://github.com/dusk-audio/dusk-studio/releases/tag/v0.9.0
