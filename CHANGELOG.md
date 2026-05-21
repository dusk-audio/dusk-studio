# Changelog

All notable changes to Dusk Studio. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/). Pre-1.0 entries are
back-filled from `git log`; once tags exist this file is the
canonical source.

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
