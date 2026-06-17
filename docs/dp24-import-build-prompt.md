# Build Prompt — TASCAM DP-24/DP-32 Song Importer for Dusk Studio

Hand this whole file to a Dusk Studio development session. It contains (A) the
task, (B) the reverse-engineered DP-24/DP-32 file-format spec, (C) the exact
Dusk Studio types to target, (D) a tiered implementation plan, (E) tests, and
(F) a calibration harness for the still-unsolved timeline-offset decoding.

The goal: import a raw TASCAM DP-24/DP-24SD/DP-32 **song folder** (read directly
off the SD card — the device is dead and cannot run its own Export) into a Dusk
Studio session, with each recorded track landing on its own track, correct
format, correct stereo pairing, and (where decodable) correct timeline position.

---

## A. Context / why this is needed

The user's DP-24 died. The supported way to move a DP project to a DAW is the
on-device **AudioDepot → Export** (renders each track to a full-song-length,
silence-padded WAV aligned at song-zero). That path is closed. So we must read
the raw song folder off the card and reconstruct the session ourselves.

Key consequence, verified from research: because the device's own export pads
each track to full length aligned at song-zero, **placing each whole-take
fragment at timeline 0 is the officially-correct result for any track that is a
single continuous recording.** Per-fragment timeline offsets only matter for
*fragmented* tracks (multiple punch-in clips comped onto one track). Ship the
zero-aligned importer first; treat offset reconstruction as a refinement.

---

## B. DP-24/DP-32 file-format spec (reverse-engineered)

A song is a folder under `MUSIC/<SongName>/` containing:

| File | Role | Confidence |
|---|---|---|
| `ZZ####_N.wav` | recorded audio fragments | **certain** |
| `song.sys` (2996 B) | mixer scene (faders/pan/EQ/dyn), stored twice | high (structure), medium (value mapping) |
| `edltable.sys` (~11.5 MB) | header + waveform-overview cache + edit table | header **certain**; placement table **unsolved** |
| `<SongName>.WAV` / `<SongName>_z.wav` | stereo master mixdown / its undo backup | certain — **ignore for track import** |
| `._*` files | macOS AppleDouble sidecars (created by the Mac) | **ignore** |

### B.1 Audio fragments — `ZZ####_N.wav`  (CERTAIN)

- Standard RIFF/WAVE PCM. Read SR/bit-depth/channels/length from the header; do
  **not** assume — it varies per song:
  - Little Angel: mono, 48 kHz, 24-bit.
  - DEMO_SONG: mono, 44.1 kHz, 16-bit.
- `ZZ####` = a **sequential recording-fragment ID** assigned in creation order
  (0000, 0001, …). It is **NOT a track number.** Every record pass / punch-in /
  bounce / take creates a new fragment. Indices can have gaps (deleted frags).
- `_N` = the **channel** of that fragment: `_1` = mono / left, `_2` = right.
  A **stereo** recording produces a `_1`+`_2` pair of equal byte-size, different
  audio. A mono recording produces `_1` only.
  - **Pairing rule:** group files by the `ZZ####` index. If both `_1` and `_2`
    exist for an index → one stereo fragment (combine to a 2-ch region). Else →
    mono fragment. (Verified: Little Angel pairs ZZ0006/0007/0009; Whiskey Spin
    has 7 pairs; DEMO has 0 pairs.)
- The fragment WAV holds the audio for one continuous recording on one track.
  Its on-disk length = the recorded length (NOT padded to song length).

### B.2 `edltable.sys` container  (HEADER CERTAIN)

Little-endian throughout. Header:

| Offset | Type | Meaning | Example (LA / DEMO) |
|---|---|---|---|
| 0x00 | char[4] | `"TEAC"` magic | — |
| 0x08 | char[] | `"DP-24"` | — |
| 0x18 | char[] | `"DP-24_EDL"` | — |
| 0x38 | u32 | flag/version (=1) | 1 / 1 |
| 0x3C | u32 | **byte offset to waveform-overview start** (scales with fragment count) | 0x3DF8 / 0x51F8 |
| 0x40 | u32 | sample rate | 48000 / 44100 |
| 0x44 | u32 | ~constant (~1.966M) | 1966104 / 1966096 |
| 0x48 | u32 | 0x00FFFFFF constant | — |
| 0x4C | u32 | **overview decimation = SR/100** (samples per overview column) | 480 / 441 |
| 0x54 | u32 | overview bytes-per-column-record (=192) | 192 |
| 0x58 | u32 | overview column count (=60000) | 60000 |
| 0x5C | u32 | 192×60000 = 11,520,000 (fixed overview size in bytes) | 11520000 |

Layout: `[header + front region]  [overview: 192 B × 60000 cols]  [tail]`.

- **Front region** `0x100 … (0x3C ptr)`: size ≈ `0xA8 + N×80` bytes, where N is a
  song-scalar (193 for LA, 257 for DEMO). Holds a few position-like values
  (locate/loop/IN-OUT points, in overview-column units) — NOT a usable
  per-fragment placement table in the two songs analyzed.
- **Overview** (starts at the 0x3C pointer): 60000 column-records × 192 bytes.
  Each column covers `decimation` samples and stores peak min/max for up to 24
  tracks (~8 B/track). This is the scrub cache — **not needed for import; skip
  it.** (Most of the 11.5 MB file is this.)
- **Tail** (after overview): per-track byte-flag table (mute/solo/arm-style
  packed bytes, e.g. `0x0101`, `05 05 03 01`). Not offsets.

> **UNSOLVED:** the `fragment → track# → timelineStart → sourceOffset` mapping
> was not located as plain ints in LA vs DEMO. See §F for how to finish it.

### B.3 `song.sys` — mixer scene  (STRUCTURE HIGH, VALUES MEDIUM)

- 2996 bytes, begins `"DP-24   "`. Contains the mixer scene **twice** (two
  near-identical blocks ≈ 0x14–0x35x and 0x770–0xb2x — likely current + saved
  snapshot; treat the first as authoritative).
- Per-channel 16-byte strip records; default fader word `0x2A40`, with at least
  one channel differing → real recallable values exist.
- An 8-entry EQ/dynamics block (`… 03 32 14 04 00 00 02 19 0a 4b 14 1e 05 1e …`)
  = per-input-channel EQ/comp triples.
- Research (flowernert/dp24sd-hacking) reports param ranges: **fader 0–127
  (max +6 dB = 0x7F), pan −63..+63, aux 0–127**, value stored twice, no
  timestamp. NOTE: that repo also reports the DP-24SD firmware *stores but does
  not recall* fader/pan on load — so mixer recall is a nice-to-have, not
  fidelity-critical. **Do NOT block track import on song.sys decoding.** Implement
  mixer recall as a separate, optional, well-tested pass and gate it behind a
  checkbox ("Import mixer settings (experimental)").

---

## C. Target Dusk Studio types (verified in this repo)

- `src/engine/FileImporter.h` — use this; it already does the heavy lifting:
  ```cpp
  duskstudio::fileimport::AudioImportRequest {
      juce::File source; juce::File audioDir; int trackIndex;
      double sessionSampleRate; int targetChannels; juce::int64 timelineStart; };
  AudioImportResult importAudio(const AudioImportRequest&);  // decodes,
      // channel-conforms, resamples to session SR, writes 24-bit WAV into the
      // session audio dir, returns an AudioRegion. Message-thread only.
  ```
  Caller commits the returned `AudioRegion` onto `Track::regions` and signals the
  engine's regions-changed mechanism (follow how the existing import path in
  `src/ui/MainComponent.cpp` / `ImportTargetPicker` does it).
- `src/session/Session.h`:
  - `struct AudioRegion { juce::File file; juce::int64 timelineStart, lengthInSamples,
    sourceOffset; int numChannels; float gainDb; bool muted; std::vector<TakeRef> previousTakes; … }`
  - `struct Track { juce::String name; juce::Colour colour; ChannelStripParams strip;
    std::atomic<int> mode{Mono|Stereo|Midi}; std::vector<AudioRegion> regions; … }`
  - `struct ChannelStripParams { … std::atomic<float> pan; std::atomic<bool> mute; … }`
    (confirm the fader-dB field name in the struct before writing to it).
- Stereo fragment → set `Track::mode = Stereo`, region `numChannels = 2`
  (or let `FileImporter` conform two mono sources — pick one and keep it
  consistent; simplest is to interleave `_1`/`_2` into a 2-ch temp WAV before
  calling `importAudio`).

---

## D. Implementation plan (tiered — ship Tier 1 first)

Create a new module `src/engine/Dp24Importer.{h,cpp}` — a **pure parser** with no
engine/UI coupling, fully unit-testable. It produces a plain struct; a thin
orchestrator drives `FileImporter` + builds `Track`s.

```cpp
namespace duskstudio::dp24 {
struct Fragment { int zzIndex; juce::File mono1, mono2; bool stereo;
                  juce::int64 lengthSamples; double sampleRate; int bitDepth; };
struct ImportedTrack { juce::String name; std::vector<Fragment> fragments;
                       juce::int64 timelineStart = 0;     // 0 until §F solved
                       float faderDb = 0.0f; float pan = 0.0f; bool mute = false; };
struct Dp24Song { double sampleRate; int bitDepth;
                  std::vector<ImportedTrack> tracks;
                  juce::String warnings; };  // human-readable caveats
Dp24Song parseSongFolder (const juce::File& folder);   // never throws; fills warnings
}
```

**Tier 1 — CORRECT-NOW (build + ship this first):**
1. Scan folder for `ZZ####_N.wav`; ignore `._*`, `<Song>.WAV`, `<Song>_z.wav`,
   `*.sys` for audio purposes.
2. Group by `ZZ####`; pair `_1`/`_2` into stereo fragments per §B.1.
3. Read each fragment's true SR/bit-depth/length from its RIFF header. If a
   fragment's SR differs from the session, `importAudio` resamples it.
4. **Map fragments → tracks 1:1** (one fragment = one track) at `timelineStart=0`.
   This is device-export-correct for single-take tracks. Order tracks by
   `ZZ####`. Name them `"DP24 Track 01"`, … (rename later from song.sys if
   solved).
5. For each, call `importAudio` (targetChannels = 1 or 2), commit the region,
   create the `Track`. Stereo fragment → `Track::mode = Stereo`.
6. Surface `Dp24Song::warnings` to the user (e.g. "N fragments imported as N
   tracks at song start; timeline offsets and track grouping not reconstructed").

**Tier 2 — MIXER RECALL (optional, behind a checkbox):** decode `song.sys`
first-snapshot strip records → `faderDb`/`pan`/`mute` per track using §B.3
ranges. Ship only after the calibration test (§F) passes on the corpus. Apply to
`Track::strip`.

**Tier 3 — TIMELINE OFFSETS (R&D, behind "experimental"):** once §F cracks the
placement table, set per-fragment `timelineStart`/`sourceOffset`, merge multiple
fragments of the same track into one `Track` (active take = region; alternates →
`previousTakes`).

### UI wiring
- Add **File → Import → "Import TASCAM DP-24 Song…"** in `src/ui/MainComponent.cpp`.
- Folder picker → `parseSongFolder` → confirmation dialog showing track count,
  format, stereo pairs, and the warnings → run the import on the message thread
  with a progress spinner (fragments can be 10–40 MB each).
- Reuse `MultiImportTargetPicker` patterns where sensible.

### Robustness
- `parseSongFolder` must never throw; bad/short WAV headers → skip with a warning.
- Enforce the existing `kMaxImportSamplesPerChannel` guard.
- Handle gappy `ZZ` indices and `_2`-without-`_1` (warn, treat as mono `_2`).

---

## E. Tests (match existing `tests/` conventions, e.g. `file_importer_audio.cpp`)

Add `tests/dp24_importer_parse.cpp` and register it in `CMakeLists.txt`:
1. **Synthetic fixture**: generate a tiny folder with `ZZ0000_1.wav`,
   `ZZ0001_1.wav`+`ZZ0001_2.wav` (stereo pair, equal size), a stub `song.sys`,
   and an `._ZZ0000_1.wav` sidecar. Assert: 2 tracks; track 2 is stereo; sidecar
   + master WAV ignored; lengths/SR read from headers.
2. **Gappy indices**: ZZ0000, ZZ0002, ZZ0005 → 3 tracks, ordered.
3. **SR mismatch**: fragment at 44.1k into a 48k session → region length scales
   by resample ratio (mirror `file_importer_audio.cpp`).
4. **Never-throws**: truncated/garbage `edltable.sys` and `.wav` → parse returns
   with warnings, no crash.
5. (Tier 2) **song.sys decode** golden test once calibrated (see §F).

Real-corpus smoke (not in CI — paths are machine-local) available to the dev:
`/Volumes/DP-24/MUSIC/Little Angel`, `…/SONG_0001`, `…/Test_sig`;
`/Users/marckorte/Downloads/DEMO_SONG`;
`/Volumes/Files/Library/Tascam Backups/**/MUSIC/*` (Whiskey Spin, Gray 1, etc.).

---

## F. Calibration harness for the UNSOLVED parts (offsets + mixer values)

Two things remain to be reverse-engineered. Use this corpus + ground truth.

**Ground truth available:**
- **DEMO_SONG** = factory demo: every track is a full-length take aligned at
  song-zero → *all* timeline offsets should decode to 0 and lengths to the full
  song. Use it as the "all-zero" reference.
- **Test_sig** (on the card) = a deliberate test signal → predictable content.
- WAV headers give each fragment's exact sample length = a known quantity to
  search for in `edltable.sys`.

**Method (offsets):**
1. Build a small standalone tool (or a disabled GTest) that, for each song,
   reads every fragment's exact sample length and computes candidate encodings:
   raw samples, `samples/decimation` (columns), bytes, frames, etc.
2. Scan the **front region** (`0x100 … 0x3C-ptr`) and **tail** for record arrays
   whose entries correlate with (fragment index, known length, and — for
   non-DEMO songs — nonzero start). The discriminator: DEMO offsets are all 0,
   real songs (Little Angel) are not. Diff the two structurally.
3. Cross-check across ≥5 corpus songs before trusting any field. A field is
   "solved" only when it predicts known lengths/zero-offsets on every song.
4. Watch for: positions in overview-column units (÷ `SR/100`); 24 fixed track
   slots vs N-sized fragment array; little-endian 32-bit; the front-region
   scalar N (193/257) whose meaning is still unknown.

**Method (song.sys mixer values):** create songs (if a working DP unit is ever
available) or use existing ones with visibly different fader/pan, and map the
16-byte strip record bytes to dB using the 0–127 / 0x7F=+6 dB and pan ±63
ranges. Until verified on ≥3 songs, keep Tier 2 behind its checkbox.

**Do not invent offsets.** If a fragment's placement can't be decoded with
confidence, leave `timelineStart = 0` and record a warning. Zero-aligned is the
device-export-correct fallback; a wrong guessed offset is worse than zero.

---

## G. Acceptance criteria

- [ ] "Import TASCAM DP-24 Song…" imports Little Angel: 11 tracks (3 stereo),
      correct SR/bit-depth, audio audibly correct, each at song start.
- [ ] DEMO_SONG imports as 28 mono tracks, all aligned, audibly the demo.
- [ ] Whiskey Spin imports with its 7 stereo pairs as stereo tracks.
- [ ] Sidecars / master WAV / `.sys` never imported as audio.
- [ ] Parser never throws on malformed input; warnings surfaced in the UI.
- [ ] Unit tests in §E pass in CI.
- [ ] (Stretch) Tier 2 mixer recall and/or Tier 3 offsets, each behind a flag,
      shipped only after corpus calibration.
