# **Dusk Studio: Digital Portastudio DAW — Implementation Prompt**

## **What You're Building**

Dusk Studio is a deliberately constrained DAW for everyone, designed for ADHD users who experience option paralysis in full-featured DAWs. It looks and feels like operating a high-end analog portastudio (think Tascam Model 2400 / DP-24), but with the non-destructive convenience of digital. The product philosophy: **if it wouldn't exist as a physical control on a console, it probably doesn't belong here.**

The target user can record, mix, and master a complete song without ever leaving this application. No unlimited tracks. No plugin chains. No menu diving. Everything is visible, tactile, and committed.

## **Architecture Overview**

**Framework**: JUCE (C++17). The entire application is a single JUCE project.

**DSP**: Extracted directly from the existing Dusk Audio plugin codebase (not hosted as plugins — compiled in as DSP classes):

* **4K EQ** — 4-band British console EQ on every channel strip
* **Multi-Comp** (FET + Opto modes) — compressor on every channel strip
* **Multi-Q** (Pultec mode) — master bus Pultec-style EQ
* **TapeMachine** — master bus tape saturation
* **Shared AnalogEmulation library** — saturation, tube warmth throughout

**Audio backend**: JACK / PipeWire / ALSA via JUCE's `AudioDeviceManager`. PipeWire is the primary target (modern Linux default).

## **Signal Flow (Fixed Topology — Never Changes)**

```
Audio Input (Mono/Stereo)          MIDI Input (keyboard/controller)
       │                                      │
       │                              ┌───────┴────────┐
       │                              │ MIDI Recording │ ← captures note/CC events
       │                              │ MIDI Playback  │ → plays back from regions
       │                              └───────┬────────┘
       │                                      │ note/CC events
       │                                      ▼
       │                              ┌────────────────┐
       │                              │  Instrument    │ ← one VST3/LV2 per channel
       │                              │  Plugin        │   (user's choice)
       │                              └───────┬────────┘
       │                                      │ audio output
       ▼                                      ▼
┌──────────────────────────────────────────────┐
│  Channel 1-24 (audio path is identical       │
│  regardless of whether source is mic or      │
│  instrument plugin)                          │
│                                              │
│  High-Pass Filter — Fixed-slope HPF          │
│  4K EQ (4-band)   — From 4K EQ plugin DSP    │
│  Compressor       — From Multi-Comp (FET/Opto)│
│                                              │
│  Send 1 Level ─────► Effect Bus 1            │
│  Send 2 Level ─────► Effect Bus 2            │
│                                              │
│  Pan Knob                                    │
│  Bus Assign 1-4   — Toggle buttons           │
│  Channel Fader                               │
│  Mute / Solo                                 │
└────────┬─────────────────────────────────────┘
         │
         └─────────────────► Aux Bus 1-4 (stereo)
         │                         │
         │                    EQ + Bus Compressor per aux
         │                         │
         ▼                         ▼
┌──────────────────────────────────────┐
│            Master Bus                │
│                                      │
│  Pultec-Style EQ (from Multi-Q)      │
│  Bus Compressor (from Multi-Comp)    │
│  Tape Saturation (from TapeMachine)  │
│  Master Fader (L/R)                  │
│  LED Meters                          │
└─────────────────┬────────────────────┘
                  ▼
              Audio Output
```

Effect Bus 1 and 2 host a single VST3/LV2 plugin each (user's choice — typically a reverb and a delay). Their returns sum into the master bus. This is the ONLY place third-party plugins are hosted (besides MIDI instrument slots).

## **UI Layout**

The UI is a single window, split into two always-visible zones. No tabs, no hidden panels, no right-click context menus.

### **Top Zone: Tape Strip (Arrangement)**

A horizontal timeline showing:

* Colored rectangular blocks for audio/MIDI regions (one row per track, 24 rows)
* A playhead line
* **Marker lane** along the top edge — named markers (Intro, Verse 1, Chorus, Bridge, etc.) that you can click to jump to, or assign via keyboard shortcut during playback. Like the LOCATE/MARK function on a Tascam DP-24. Markers are created by pressing a "MARK" button (or key) at the current playhead position, then typing a name.
* Loop in/out brackets (visual indicators, set via dedicated buttons)
* Minimal interaction: click to place playhead, drag regions to move, split/delete regions. NO waveform editing (no zoom-to-sample, no pencil tool, no individual-sample manipulation). NO automation lanes.
* Track labels on the left edge (click to rename, color-coded)

#### **Region operations on the tape strip (DP-24-style track edit)**

In addition to move / split / delete, the tape strip supports three minimal region operations. All are non-destructive — the underlying WAV files are never modified — and all happen with single gestures, not in a separate editor.

* **Region trim (drag-edge)**: drag the left or right edge of an audio region to shorten it. Trimming the right edge decreases `lengthInSamples` only. Trimming the left edge increases `sourceOffset` and `timelineStart` by the same amount and decreases `lengthInSamples`, so the region appears to shrink from the left while the underlying file stays put. Trim is reversible: drag the edge back outward and the trimmed-off audio reappears, bounded by the available samples in the source WAV. Region trim is the right answer for "punch out the cough at the end of the take" without any waveform view. There is no time-stretch — trim only changes which slice of the source file plays back.
* **Take history per region**: punch-in already creates a new WAV per take (`track01_take02.wav`, etc.). The current take is the one referenced by `AudioRegion.audioFilePath`; previous takes for the same range are tracked on the region as `previousTakes: std::vector<TakeRef>` (each entry stores the file path + the source offset / length that was active when that take was current). The region shows a small numeric badge in its top-left corner displaying the active take number when more than one exists. Clicking the badge cycles to the next take; right-clicking the badge opens an explicit picker listing all takes with a short audition button each. (This is the second exception to the "no right-click menus" rule, alongside marker delete.) Take swap is undoable. Take files are never auto-deleted — they accumulate in the session's `audio/` folder until the user clears them via Session → "Clean unused takes."
* **Region copy/paste between markers (range copy)**: select an in/out range — either by setting loop in/out, by setting punch in/out, or by clicking two markers in sequence (Cmd/Ctrl + click for range select). Cmd/Ctrl + C copies all regions across all tracks (or only selected tracks) inside that range to a session-local clipboard, including their relative timeline offsets and any take history. Cmd/Ctrl + V pastes at the playhead, preserving inter-track offsets. This is the modern equivalent of the DP-24 TRACK EDIT copy/paste-between-locate-points workflow, useful for doubling a chorus, repeating a verse, or bouncing-by-copy when you want a section in two places without re-recording. Pasted regions create new region records but reference the same underlying WAV files (no duplication on disk). Paste is undoable.

The `AudioRegion` struct in the Session Model carries `sourceLength` (so trim is just a setter) and `previousTakes` (the take-history list). Its `TakeRef` companion struct stores the file path + offset/length each previous take was recorded with, so swapping takes restores exactly the slice that was active before. See the Session Model section above for the full struct definitions.

### **Bottom Zone: Console (Mixer)**

Modeled after the Tascam Model 2400 layout:

* **24 channel strips** (left to right, organized as three banks of 8 — 1-8 / 9-16 / 17-24 — to match standard control surfaces. Mackie Control, FaderPort 16, X-Touch Compact, etc., all map cleanly across the banks via the BANK 1 / BANK 2 / BANK 3 switch), each containing:
  * Input selector (Mono/Stereo/MIDI toggle at top)
  * LED meter (peak + RMS, along the top or side of the strip)
  * HPF knob
  * 4-band EQ section (4 frequency knobs + 4 gain knobs, or stacked like the Tascam)
  * Compressor section (threshold, ratio, attack, release + FET/Opto switch)
  * Send 1 knob + Send 2 knob
  * Pan knob
  * Bus assign buttons (1-4)
  * Mute + Solo buttons
  * Channel fader (vertical slider)
* **4 Aux bus strips** (narrower, to the right of channels):
  * EQ (simplified 3-band)
  * Bus compressor (threshold, ratio, attack, release)
  * Fader
  * Mute
  * Solo (aux-bus solo behaves identically to channel solo — see Solo semantics below)
* **Master strip** (rightmost, wider):
  * Pultec EQ controls
  * Bus compressor controls
  * Tape saturation amount knob
  * Master L/R fader
  * Large stereo LED meter
  * **No solo button on master** (solo is a routing concept; the master is the output)

#### **Solo semantics (solo-in-place / SIP)**

Solo is **solo-in-place**. The rule is:

```
channelPasses = !mute && (anySoloActive ? thisChannelSoloed : true)
```

When any channel (or aux bus) has solo engaged, all non-soloed, non-muted sources are silenced at the output. Multiple solos sum — every soloed channel passes. Mute always wins over solo (a muted+soloed channel stays silent). Aux-bus solo behaves identically: soloing an aux bus silences all non-soloed channels and aux buses. The master strip has no solo button — solo is a routing decision and the master is downstream of it.

There is no AFL or PFL bus in v1; solo is purely SIP at the master.

### **Transport Bar (between tape strip and console, or at bottom)**

Physical-feeling transport controls:

* REW / FF / STOP / PLAY / RECORD buttons (large, obvious)
* Punch In / Punch Out buttons (with indicator lights)
* Loop toggle button
* **Click button** — toggles the metronome on/off (see "Metronome / click track" below)
* **MARK button** — drops a named marker at current playhead position
* **Marker jump** — left/right arrow buttons to jump between markers, or click markers in the tape strip
* BPM display with tap tempo button
* Timeline position display (bars:beats or minutes:seconds, togglable)
* Bounce/Export button (renders master output to stereo WAV/FLAC)

#### **Metronome / click track**

A simple click generator produces accent + beat samples at the session BPM and time signature, routed to a dedicated mute-able internal bus that sums into the master output (and would sum into a cue bus, were one ever added). Click on/off is a transport-bar button; volume is a small knob next to it. The click follows transport (plays during PLAY and RECORD, silent when stopped) and respects the loop range. **The click never records to disk** — it is generated live, post-everything, and is not part of any region or bounce by default. A separate "include click in bounce" checkbox in the export dialog allows recording it into a stereo bounce for practice tracks; default is off.

#### **Cue / headphone bus**

Dusk Studio's master output doubles as the headphone feed. The assumed Linux setup is one output device routing to both monitors and headphones (or the user's audio interface mirroring monitor and headphone outs). A separate cue bus with independent routing is **out of scope for v1**. Users with multi-output interfaces assign the master to whichever physical output drives the headphones via the audio device settings dialog.

### **MIDI Piano Roll (modal overlay)**

When a MIDI region is double-clicked, a piano roll editor opens as an overlay on top of the tape strip area. It provides:

* Note drawing, selection, move, resize, delete
* Velocity editing (bar graph below notes)
* Quantize button (with grid size selector: 1/4, 1/8, 1/16, 1/32, triplets)
* Snap to grid toggle
* Close button returns to the tape strip view
* This is the ONLY modal/overlay view in the entire application

## **Core Systems to Implement**

### **1. Audio Engine (`src/engine/`)**

The real-time audio callback processes the entire fixed signal graph every buffer:

```
AudioCallback (one buffer, e.g. 256 samples):

1. AUDIO TRACKS: Read playback audio from disk ring buffers
2. MIDI TRACKS: For each MIDI-mode channel:
   a. Collect MIDI events for this buffer window:
      - If PLAYING: read from MidiRegion (tick→sample conversion using BPM)
      - If RECORDING: merge incoming live MIDI + echo to region capture buffer
      - If MONITORING (stopped, but input active): pass live MIDI through
   b. Feed collected MIDI events to the instrument plugin
   c. Call instrument plugin's processBlock() — produces audio buffer
   d. This audio buffer now enters the channel strip identically to audio tracks
3. If recording audio tracks, capture input audio into record ring buffers
4. For each channel 1-24 (audio and MIDI tracks are identical from here):
   a. Process through channel strip DSP (HPF → EQ → Comp)
   b. Tap send levels, accumulate into effect bus buffers
   c. Apply pan + fader gain (read from automation if in Read/Touch mode)
   d. Accumulate into assigned aux bus buffers and/or master bus
   e. Apply solo/mute gating (SIP rule from "Solo semantics" above)
5. Process effect bus plugins, sum returns into master
6. Process each aux bus strip (EQ → Comp → Fader → solo/mute), sum into master
7. Sum metronome click into master (if enabled)
8. Process master bus chain (Pultec → Comp → Tape → Fader)
9. Output to audio device
10. If recording MIDI: push captured MIDI events to session model (non-RT thread)
```

**Disk streaming**: double-buffered ring buffers (`juce::AbstractFifo`) between real-time thread and background disk I/O thread. One read stream per playing track, one write stream per recording track. Use `juce::TimeSliceThread` for the background disk work.

**Ring buffer sizing**: each per-stream ring buffer is allocated at `bufferDurationSeconds × sampleRate × sizeof(float) × numChannels` bytes. The default buffer duration is 4.0 seconds, exposed as a configurable parameter (`diskBufferSeconds`, clamped to 2.0–8.0 seconds). A track is either reading (playback) or writing (record-armed and rolling) at any instant — never both — so the worst case is 24 stereo streams active simultaneously. At default settings (24 stereo streams, 48 kHz, 4 s): 24 × 2 × 4 × 48000 × 4 bytes ≈ 36 MB. This is allocated once at session load (or audio device initialization) — not per-buffer-callback. Validate the configured value before allocation: values below 2.0 s risk underruns on slow disks, values above 8.0 s waste memory with diminishing returns. Log the total memory budget at allocation time for diagnostics.

**Disk stream underrun/overrun policy**:

* **Read stream underrun** (slow disk or CPU stall): when a read stream (`juce::AbstractFifo` / `TimeSliceThread` backed) has insufficient buffered data, emit silence for the requested frames, increment a per-stream `underrunCount` atomic counter, and log a rate-limited error (at most once per second) including the stream ID and current `diskBufferSeconds`. Do not block or retry on the audio thread.
* **Write stream overrun** (recording buffer full): when a write stream's ring buffer is full, drop the oldest buffered samples (ring-buffer wrap) to make room for new data, increment a per-stream `overrunCount` atomic counter, and log a rate-limited warning. If `overrunCount` exceeds a configurable threshold (default: 10 within any 5-second window), expose a callback/flag (`onRecordingOverrun`) to pause recording or notify the UI so the user can take action (e.g., increase `diskBufferSeconds` or reduce track count).
* These behaviors are enforced after validating/clamping `diskBufferSeconds` (2.0–8.0) and should be included in allocation-time diagnostics alongside the total memory budget log.

**Latency compensation**: offset recorded regions by the audio buffer size so they align on playback. Apply this at region creation time.

**Send-bus plugin latency**: effect bus plugins (Effect Bus 1-2) may report latency via `getLatencyInSamples()`. This latency is intentionally *not* compensated — the portastudio design treats send effects as live processing (like outboard gear patched into an aux return). Compensating send/return paths would require delaying the dry bus to match, adding complexity and increasing overall system latency. If a plugin introduces perceptible latency (>5ms), the user should choose a lower-latency alternative or accept the timing offset as part of the effect character. Document this in the UI help text for effect bus slots.

**Buffer size**: let the user choose (64-2048), but default to 256 samples. The fixed DSP graph has predictable CPU cost — no plugin chain surprises.

### **2. Session Model (`src/session/`)**

```
struct Session {
    String name;
    File sessionDirectory;        // all WAVs live here
    double sampleRate;
    double bpm;
    TimeSignature timeSignature;  // numerator/denominator, used by metronome and bar math

    std::array<Track, 24> tracks;
    std::array<AuxBus, 4> auxBuses;
    std::array<EffectSend, 2> effectSends;  // plugin slot + return level
    MasterBus master;

    std::vector<Marker> markers;  // named timeline locations
    TransportState transport;
    int64_t playheadSamples;
    LoopRange loop;               // in/out points
    PunchRange punch;             // punch-in/out points (see invariants below)
};

struct Track {
    String name;
    Colour colour;
    InputMode inputMode;          // Mono, Stereo, MIDI
    int midiInputChannel;         // Internal 0-15, or -1 = omni (all channels).
                                  // Use userChannelToInternal(1-24) at UI boundaries,
                                  // internalChannelToUser(0-15) for display.
    String instrumentPluginId;    // only used if MIDI
    String instrumentPluginState; // serialized plugin state (base64)
    std::vector<Region> regions;  // sorted by timeline position (audio OR midi)
    ChannelStripParams strip;     // all knob values
    bool mute, solo;
    bool recordArmed;             // persisted per-session (see RECORD requirements)
    std::bitset<4> busAssign;
};

// Audio regions — reference a WAV file on disk
struct AudioRegion : Region {
    String audioFilePath;         // current take, relative to session directory
    int64_t sourceOffset;         // offset into the audio file (advanced by left-trim)
    int64_t sourceLength;         // bounded by file length minus sourceOffset; equal to
                                  // lengthInSamples for un-stretched audio (always, in v1)
    float gainTrim;
    std::vector<TakeRef> previousTakes;  // older takes available via the region's take badge
                                          // (see "Region operations on the tape strip")
};

// Reference to a previous take of an audio region, kept on the region itself
// so swapping takes restores the source offset/length that take was recorded with.
struct TakeRef {
    String audioFilePath;         // relative to session directory
    int64_t sourceOffset;         // sourceOffset at the time this take was active
    int64_t sourceLength;
    int64_t recordedAt;           // ms since session start, for ordering / display
};

// Global session constant — single PPQN used by recorder, sequencer, and import/export.
// On MIDI file import with a different PPQN, timestamps are resampled to this resolution.
static constexpr int kSessionPPQN = 480;

// MIDI regions — contain note/CC events inline
struct MidiRegion : Region {
    std::vector<MidiEvent> events;  // sorted by tick offset
    int64_t tickLength = 0;         // Length of the region in ticks at kSessionPPQN.
                                    // Drives the region's musical extent independently
                                    // of any specific event positions. See "MidiRegion
                                    // tickLength semantics" below.
    bool tempoLock = true;          // ON: events follow Session BPM (tick→sample
                                    //     converted at playback time using current BPM).
                                    // OFF: events are baked to sample positions at the
                                    //     BPM active when the flag was turned off; the
                                    //     region plays back like audio, immune to BPM.
                                    // See "BPM changes and existing MIDI regions" below.
    double bakedBPM = 0.0;          // BPM at which events were baked (only meaningful
                                    // when tempoLock == false). Zero otherwise.
    // All tick timestamps use kSessionPPQN. No per-region PPQN — the recorder,
    // sequencer, and overdub/merge paths all use the global constant.
};

struct Region {
    int64_t timelineStart;        // absolute position in samples. ALWAYS sample-locked:
                                  // the region's start point does not move when Session
                                  // BPM changes, regardless of type or tempoLock. For
                                  // MidiRegion with tempoLock=ON, only the *internal*
                                  // event offsets retime — the region boundary stays put.
    int64_t lengthInSamples;      // For MidiRegion with tempoLock=ON, this is recomputed
                                  // on BPM change as `tickLength × samplesPerTick(BPM)`.
                                  // For tempoLock=OFF or audio regions, it is immutable
                                  // under BPM changes (frozen at the moment of locking
                                  // for MIDI; immutable for audio).
    RegionType type;              // Audio or MIDI
};

struct MidiEvent {
    int64_t tickOffset;           // Relative to the region's start boundary (tick 0 at
                                  // timelineStart). Always in ticks at kSessionPPQN,
                                  // regardless of tempoLock. When tempoLock=ON, the
                                  // playback engine converts tickOffset → sample offset
                                  // each callback using the current Session BPM. When
                                  // tempoLock=OFF, the conversion is done once using
                                  // bakedBPM and the result is cached; subsequent BPM
                                  // changes do not re-convert.
    uint8_t status;               // note on/off, CC, pitch bend, etc.
    uint8_t data1;                // note number or CC number
    uint8_t data2;                // velocity or CC value
};

struct Marker {
    int64_t positionSamples;
    String name;                  // "Intro", "Verse 1", "Chorus", etc.
    Colour colour;                // optional, for visual distinction
};

struct LoopRange {
    int64_t inPoint;              // sample position, inclusive
    int64_t outPoint;             // sample position, exclusive
    bool enabled;
};

struct PunchRange {
    int64_t inPoint;              // sample position, inclusive
    int64_t outPoint;             // sample position, exclusive
    bool enabled;
};
```

**MidiRegion `tickLength` semantics** (resolves the prior ambiguity around region length under tempoLock=ON):

* `tickLength` is the canonical musical extent of the region, expressed in ticks at `kSessionPPQN`. It exists *separately* from event positions: a region can be 4 bars long even if the last note ends on beat 1.5 of bar 1.
* **On creation during recording**: when MIDI recording begins, a new `MidiRegion` is created with `tickLength` rounded up to the next bar boundary at the current BPM and time signature. As recording continues, `tickLength` extends to (current playhead tick) rounded up to the next bar boundary. When recording stops, `tickLength` is finalized — typically padded to the next bar to leave musical breathing room.
* **On manual edit**: when a user moves or adds an event past the current `tickLength` in the piano roll, `tickLength` extends to cover (rounded up to the next beat). The user can also drag the region's right edge in the tape strip to set `tickLength` directly.
* **lengthInSamples derivation**: with `tempoLock=ON`, `region.lengthInSamples = tickLength × samplesPerTick(currentBPM)` — recomputed whenever Session BPM changes. With `tempoLock=OFF`, `lengthInSamples` is frozen at the moment of locking (computed once with `bakedBPM`); `tickLength` is preserved on the region for reference but no longer drives playback length.
* **Empty region**: a freshly created MIDI region (no events yet) has `tickLength` set to one bar at the current time signature so it is visible in the tape strip and the piano roll has a default canvas to draw on.

**LoopRange validation invariants** (enforced by `Session::setLoopRange()` — never mutate `inPoint`/`outPoint` directly):

1. **Clamp to session bounds**: both points are clamped to `[0, sessionLengthSamples]` before any ordering check. Points beyond the current session length are pulled in, not rejected.
2. **Auto-swap out-of-order input**: if the caller passes `inPoint > outPoint`, the setter silently swaps them. UI gestures (drag-to-create on the timeline) naturally produce reversed ranges when the user drags right-to-left, so rejecting would be user-hostile.
3. **Minimum length**: `outPoint - inPoint` must be `>= kMinLoopSamples` (define as 64 samples — roughly 1.5 ms at 44.1k, below which the transport would thrash). If the requested range is shorter (including the degenerate `inPoint == outPoint` case), the setter extends `outPoint` to `inPoint + kMinLoopSamples`, then re-clamps to session bounds. If that still produces a sub-minimum range (session is shorter than `kMinLoopSamples`), the setter leaves the previous `LoopRange` untouched and returns false.
4. **Auto-disable on invalid**: if validation cannot produce a usable range (step 3 fallback), `enabled` is forced to `false` and a warning is logged. The transport's loop-playback code may therefore assume that whenever `LoopRange.enabled == true`, the invariants `0 <= inPoint < outPoint <= sessionLengthSamples` and `outPoint - inPoint >= kMinLoopSamples` both hold — no defensive checks needed in the audio callback.
5. **Session resize**: when the session length shrinks (e.g., after deleting tail regions) such that `outPoint > sessionLengthSamples`, `setLoopRange()` is re-invoked with the existing points to re-run validation. Loop points are never silently left pointing past the end.

**PunchRange validation invariants** (enforced by `Session::setPunchRange()` — same shape as LoopRange):

1. **Clamp to session bounds**: both points are clamped to `[0, sessionLengthSamples]` before any ordering check.
2. **Auto-swap out-of-order input**: if `inPoint > outPoint`, the setter silently swaps them. (Drag-to-create from right to left in the tape strip is a natural gesture.)
3. **Minimum length**: `outPoint - inPoint` must be `>= kMinPunchSamples` (= 64 samples). If shorter, extend `outPoint` to `inPoint + kMinPunchSamples`, re-clamp, and if that still fails, leave the previous `PunchRange` untouched and return false.
4. **Auto-disable on invalid**: same as LoopRange — `enabled` is forced to `false` and a warning is logged. The transport's punch-arming logic may assume the same invariants hold whenever `PunchRange.enabled == true`.
5. **Session resize**: re-run validation when session length shrinks past `outPoint`.

**Session directory structure:**

```
MySong/
├── session.json              # everything: track layout, regions, MIDI events, markers, automation
├── session.json.autosave     # written every 30s while dirty (see Autosave below)
├── audio/
│   ├── track01_take01.wav
│   ├── track01_take02.wav    # punch-in creates new file
│   ├── track03_take01.wav
│   └── ...
├── plugins/
│   ├── track05_instrument.xml  # instrument plugin state (full preset)
│   ├── send1_effect.xml        # effect send plugin state
│   └── send2_effect.xml
└── bounces/
    ├── MySong_mix1.wav
    └── MySong_mix2.wav
```

Note: MIDI event data is stored inline in `session.json` (not as separate .mid files), because MIDI regions are small and this keeps everything atomic. Audio regions reference external WAV files because those are large. Plugin states are saved as separate files so they can be large without bloating the session JSON.

**Atomic save pattern (`SessionSerializer::save`)**: to prevent corruption from crashes or power loss during write, `SessionSerializer` must use an atomic-write pattern: (1) write the JSON to a temporary file (`session.json.tmp` in the same directory), (2) call `fsync()` on the file descriptor to flush data to disk, (3) atomically rename the temp file to `session.json` (POSIX `rename()` is atomic on the same filesystem), (4) on error, clean up the temp file. `SessionSerializer::load` reads `session.json` normally — the atomic rename guarantees it is always complete. This pattern ensures that `session.json` is never partially written.

**Autosave / crash recovery**:

* While the session is dirty (any state change since the last clean save), an autosave timer writes `session.json.autosave` every **30 seconds** using the same atomic-write pattern (`session.json.autosave.tmp` → `fsync` → `rename` to `session.json.autosave`).
* On session **load**, if `session.json.autosave` exists and its mtime is newer than `session.json`, the user is presented with a recovery dialog: "An autosave was found that is newer than the last manual save. Recover the autosave (last modified at ...) or discard it?" with options **Recover**, **Discard**, **Keep both** (the latter writes the autosave to `session.json.autosave.recovered.<timestamp>` and continues with `session.json`).
* On a clean manual save, `session.json.autosave` is deleted.
* On a crash, the autosave is preserved untouched; next launch triggers the recovery dialog above.

**File format**: JSON. The session is so constrained (24 fixed tracks, fixed topology) that a simple JSON file describes everything. No need for a complex format.

### **3. Transport (`src/engine/`)**

State machine:

```
         ┌──────────┐
    ┌───►│ Stopped  │◄────────────────┐
    │    └────┬─────┘                 │
    │         │ Play                  │ Stop
    │         ▼                       │
    │    ┌──────────┐    Record  ┌────┴──────┐
    │    │ Playing  │───────────►│ Recording │
    │    └────┬─────┘            └────┬──────┘
    │         │                       │
    │         │ PunchIn enabled       │
    │         ▼                       │
    │    ┌────────────────┐           │
    └────┤ PunchedIn      │◄──────────┘
         └────────────────┘   PunchOut
```

* Locate(position): jump playhead to sample position. If playing, seamlessly continue from new position.
* Locate(marker): jump to a named marker's position. Keyboard shortcut or click.
* Marker navigation: "previous marker" / "next marker" buttons cycle through markers in timeline order.
* Loop: when enabled and playhead reaches loop out point, jump back to loop in point.
* Punch in/out: when armed, recording starts/stops at punch points during playback. Sample-accurate crossfades (64-sample raised cosine) at punch boundaries to prevent clicks. Punch range follows the `PunchRange validation invariants` above.
* Bounce: render the master bus output to a stereo WAV/FLAC file for the selected range (or full session).

**RECORD-arm requirement**: Pressing RECORD on the transport requires **at least one track to be record-armed** (each track has an arm button on its strip; arm state is persisted in `Track.recordArmed`). If the user presses RECORD with no armed tracks:

* The RECORD button flashes briefly (e.g., 3 fast pulses) to indicate the requirement.
* Transport state does not advance into Recording.
* A short non-modal toast appears: "Arm at least one track to record."

This prevents the common foot-gun of pressing RECORD and producing no recording. Arm state is per-track and saved to `session.json`.

### **4. Channel Strip DSP (`src/dsp/`)**

Extract DSP processing classes from existing Dusk Audio plugins. These are compiled directly into Dusk Studio — no plugin hosting overhead:

```
class ChannelStrip {
public:
    void prepare(double sampleRate, int blockSize);
    void process(juce::AudioBuffer<float>& buffer);
    void setParameters(const ChannelStripParams& params);

private:
    HighPassFilter hpf;           // juce::dsp::IIR with variable freq
    FourBandEQ eq;                // extracted from 4K EQ plugin
    DualModeCompressor comp;      // extracted from Multi-Comp (FET + Opto)
    juce::SmoothedValue<float> panValue, faderGain, send1Level, send2Level;
};
```

**Important**: the DSP classes from 4K EQ and Multi-Comp use oversampling internally. Keep this — it's what makes them sound good. The fixed topology means you can budget the CPU precisely: 24 channels × (EQ + Comp with oversampling) = known cost. On a modern x86_64 laptop this lands comfortably under ~45% CPU at 256 samples / 48 kHz with all 24 channels active (linear scale-up from the 16-channel ~30% baseline).

Master bus chain similarly extracts DSP from Multi-Q (Pultec mode) and TapeMachine.

### **5. MIDI System (`src/midi/`)**

MIDI is the one area where the portastudio metaphor bends — you can't punch a MIDI tape, but you can edit notes. The design balances "do it by ear" (record your performance live) with the reality that MIDI editing is expected and useful.

#### **5a. Signal Flow for MIDI Channels**

When a channel's input selector is set to MIDI:

```
MIDI Input Device (keyboard, pad controller, etc.)
    │
    │  ← filtered to track's MIDI channel (1-16, or omni)
    ▼
┌──────────────────────────────────┐
│  MIDI Engine                     │
│                                  │
│  LIVE: routes incoming MIDI ─────┼──► Instrument Plugin (generates audio)
│        events in real time       │           │
│                                  │           │ audio output
│  RECORDING: captures events ─────┼──► also to plugin (musician hears themselves)
│        with timestamps           │           │
│                                  │           │
│  PLAYBACK: reads events from ────┼──► Instrument Plugin (generates audio)
│        MidiRegion, dispatches    │           │
│        to plugin on schedule     │           │
└──────────────────────────────────┘            │
                                                ▼
                                    Channel Strip (HPF → EQ → Comp → ...)
                                    Same processing as any audio channel
```

The channel strip is completely unaware that its audio came from a plugin rather than a microphone. This is the key architectural insight — MIDI complexity stays in the MIDI layer, the mixer stays clean.

#### **5b. MIDI Recording and Playback**

**Recording:**

* When a MIDI channel is armed and the transport is recording, incoming MIDI events are captured into a new MidiRegion with tick-based timestamps (relative to region start, at 480 PPQN resolution).
* Ticks are derived from the session BPM and sample position. This means MIDI data is tempo-relative — if you change BPM later, MIDI regions stretch/compress musically.
* The instrument plugin receives events in real time during recording so the musician hears themselves through the channel strip (zero-additional-latency monitoring — the plugin is already in the audio graph).
* Audio output from the instrument plugin is NOT recorded to disk. It's always generated live. This means you can change instruments after recording.
* **All MIDI event types are captured**: note on/off, control change (CC — modwheel, sustain, expression, etc.), pitch bend, channel pressure, polyphonic aftertouch. The piano roll in v1 edits **notes only**; CCs and other event types play back unchanged but are not visible/editable in the piano roll. Users who want to suppress non-note events should filter them at the input source (most MIDI controllers can do this in their setup), or wait for a post-v1 MIDI-input-filter slot. This is a deliberate v1 trade-off — capturing a complete performance is more important than editability of every nuance.

**Playback:**

* The MIDI engine reads ahead from MidiRegions, converts tick positions to sample positions using the current BPM, and dispatches events to the instrument plugin at the correct time.
* Read-ahead buffer: convert the next ~1 second of MIDI events to sample-accurate timestamps each audio callback. This is computationally trivial compared to audio disk streaming.
* **Plugin-latency-aware look-ahead**: when an instrument plugin reports `getLatencyInSamples() = L > 0`, the MIDI engine reads ahead by an additional `L` samples relative to the playhead when dispatching events to that plugin. This means the *effective* read-ahead horizon is per-channel: `1 second + L_i` for each instrument slot `i`. Across active slots, the engine schedules its read-ahead pass to cover `max(L_i)` so all plugins are correctly fed. The 1-second baseline is comfortably more than any realistic instrument plugin latency, so in practice the read-ahead horizon does not need to grow — it just shifts the dispatch time per plugin. Compensation is required for sample-accurate alignment of plugin audio output with the timeline.

**Punch in/out for MIDI:**

* Works identically to audio punch — at the punch-in point, a new MidiRegion is created. Events from the old region before the punch point are preserved, the new recording replaces everything in the punched range.
* No crossfade needed (MIDI is discrete events, not continuous audio), but a note-off should be sent for any currently-sounding notes at the punch boundary to prevent hanging notes.

**Overdub mode (MIDI-specific):**

* In addition to standard punch (replace), MIDI channels get an overdub toggle. When overdub is ON during recording, new events are merged into the existing region rather than replacing it. This lets you layer drum parts or build up chords across multiple passes — a common and expected MIDI workflow.

**Punch + Overdub interaction:** Punch in/out and the overdub toggle can be enabled simultaneously. Three canonical rules govern the behavior:

1. **At punch-in (both modes)**: send note-off for all currently-sounding notes to prevent hanging notes across the region boundary.
2. **Overdub OFF + Punch** (replace mode): discard all original events inside `[punch-in, punch-out)`. Insert synthetic note-offs at punch-in for any notes that started before punch-in (truncating their sustain at the boundary). At punch-out, send note-off only for newly recorded notes ending at the boundary. Events strictly before punch-in and strictly after punch-out are preserved unchanged.
3. **Overdub ON + Punch** (merge mode): preserve all original events unchanged — original notes that sustain through the punched range continue undisturbed. New recorded events are added (merged) inside the punched range only. At punch-out, send note-off only for newly recorded notes ending at the boundary.

These rules are implemented in the region edit code that computes punch boundaries and note-off insertion. The `applyThinnedAutomation()` commit and thread-safety behavior described below is unchanged.

**BPM changes and existing MIDI regions:**

* MidiRegion event timestamps are tick-based (480 PPQN). By default, all MidiRegions follow Session BPM — changing BPM immediately retimes playback of all MIDI events (tick→sample conversion uses the current BPM).
* Because instrument plugin audio is generated live, a BPM change will alter MIDI-to-sample mapping. If audio tracks were recorded alongside MIDI at the original BPM, they will desync (audio is sample-locked, MIDI is tick-locked).
* Each MidiRegion has a `tempoLock` flag (default: ON = follows tempo). When OFF, the region's events are baked to sample positions at the BPM active when the flag was turned off, making them time-locked like audio.
* When the user changes Session BPM and unlocked MIDI regions exist alongside recorded audio, show a confirmation warning: "Changing BPM will retime MIDI regions but audio tracks remain at original timing. Continue?" with options to proceed, cancel, or bake all MIDI to audio first.
* Alternative workflows: (1) bake MIDI to audio (render in place) before changing BPM, (2) set tempoLock=OFF on regions that should keep original timing, (3) accept the desync for creative effect.

**Recommended workflow for mixed MIDI/audio sessions:**

1. **Default behavior:** New MIDI recordings have `MidiRegion.tempoLock = ON` so they automatically follow Session BPM changes unless explicitly locked.
2. **Best practice:** Finalize the Session BPM before recording audio. If BPM must change after audio is recorded, bake MIDI to audio (render in place) or set `tempoLock = OFF` on regions that should remain sample-locked before changing BPM.
3. **Typical scenario:** Drums (MIDI at 120 BPM) + guitar (audio). To move to 115 BPM: either (a) bake the drum MIDI to audio first, (b) set `tempoLock = OFF` on the drum regions so they stay at 120 timing, or (c) leave `tempoLock = ON` and accept the MIDI retiming (guitar audio stays at 120 timing, drums shift to 115).
4. **UI guidance for BPM change warning:** When the confirmation dialog triggers (see above), present three clear options: **Proceed** (retime unlocked MIDI, keep audio as-is), **Cancel**, or **Bake all MIDI to audio first** (render in place, then change BPM so everything is sample-locked). The dialog should include inline help text explaining `tempoLock` and baking so users understand the tradeoffs.

#### **5c. Instrument Plugin Hosting**

**Scope**: only three contexts need plugin hosting in the entire app:

1. Instrument plugins — one per MIDI-mode channel (up to 16)
2. Effect send plugins — exactly 2 (one per send bus)
3. That's it. No plugin chains anywhere.

**Implementation** using JUCE:

* `AudioPluginFormatManager` — registers VST3 and LV2 formats
* `KnownPluginList` — scanned at first launch, cached to disk, rescan on demand
* `AudioPluginInstance` — one per active slot

**Plugin scanning:**

* First launch: scan standard Linux VST3/LV2 paths (`~/.vst3/`, `/usr/lib/vst3/`, `/usr/lib/lv2/`, etc.)
* Cache results to `~/.config/Dusk Studio/plugin-cache.json`
* Separate instrument plugins from effects in the UI (instruments show for MIDI channel slots, effects show for send slots)
* Scan in a background thread — never block the UI. Show a progress indicator on first launch.
* Crash-resistant scanning: fork a child process per plugin to scan, so a crashing plugin doesn't take down the app. JUCE's `PluginListComponent` does this.

**Runtime plugin crash/hang protection:**

* **Time-budget check (primary mechanism):** Wrap all calls to `AudioPluginInstance::processBlock()` (for both instrument and effect slots) with a time-budget check. Before each call, record the current time; after return, check elapsed time against a configurable budget (default: 2× the buffer duration, e.g., ~11.6ms for 256 samples at 44.1k). If the plugin exceeds the budget, set the per-slot `std::atomic<bool> bypassed` flag and switch to a safe silent/pass-through path (instrument slots output silence, effect slots pass dry signal through). This is the only mechanism that runs purely on the audio thread and is fully real-time safe.
* **Crash isolation (preferred — out-of-process hosting):** For hard crashes (SIGSEGV, SIGFPE, abort), the recommended approach is out-of-process plugin hosting: each plugin slot runs in a separate child process communicating via shared memory or IPC. A crash in the child is detected by the parent (SIGCHLD / waitpid / process exit code) without any signal handling on the audio thread. The audio thread sees a broken pipe / missing shared-memory update and activates the bypass flag. This is the only approach that reliably isolates crashes across all platforms.
* **Crash isolation (fallback — NOT PRODUCTION-READY, development only):** If out-of-process hosting is not feasible, use platform-specific structured exception handling: SEH (`__try/__except`) on Windows, Mach exception ports on macOS. **All in-process fallback code must be guarded by `#ifdef ALLOW_INPROCESS_PLUGIN_FALLBACK`** so it cannot be shipped accidentally — this define should never be set in release builds. **This fallback is unsafe and unreliable**: handling SIGSEGV/SIGFPE in-process may leave the process in an undefined state — destructors may not run, memory may leak, and further crashes are likely. SEH and Mach exception ports are the least-bad in-process mechanisms but cannot guarantee safe recovery. **Do NOT use POSIX signal handlers (signal/sigaction) on the audio thread** — POSIX signal handlers are process-global, not thread-safe, and only `volatile sig_atomic_t` is guaranteed safe to modify in a signal handler per the C/C++ standards. `std::atomic<T>::store()`/`load()` are **not** async-signal-safe — the C++ standard makes no such guarantee, even for lock-free atomics. Any other work (logging, allocation, mutex, `std::atomic<bool>` operations) in a signal handler is undefined behavior. **Out-of-process hosting is a Phase 4 requirement** — the in-process fallback (SEH, Mach ports, `volatile sig_atomic_t`) is a partial mitigation only. `KnownPluginList` updates, `AsyncUpdater`/`MessageManager::callAsync` UI recovery, and the bypass flag are best-effort recovery mechanisms; after a plugin crash via in-process handling, the app may be in an inconsistent state and **may require a restart**. If a POSIX signal handler is used as an absolute last resort, it must: (a) only set a `volatile sig_atomic_t` bypass flag (not `std::atomic<bool>`), (b) perform no allocation, locking, logging, or UI work, (c) clearly document that this is platform-dependent, will not unwind correctly, and leaves the process in an undefined state.
* On the message thread (via `AsyncUpdater` or `MessageManager::callAsync`), detect the bypass flag (`std::atomic<bool>` for time-budget bypasses; `volatile sig_atomic_t` only if the in-process signal-handler fallback is active) and: (a) record the error type and timestamp in the `KnownPluginList` scan cache (so the user is warned if they try to load it again), (b) surface a non-blocking notification in the instrument/effect slot UI showing a red "(Crashed: PluginName)" label with "Reload" and "Unload" action buttons, (c) optionally unload the plugin instance on the message thread and attempt a controlled reload if the user clicks "Reload". **Note:** if the crash was handled via in-process fallback (not out-of-process), display an additional warning: "Plugin crash detected. The application may be in an unstable state. Save your session and restart recommended."
* All crash/hang detection state changes happen via `std::atomic<bool>` flags (for time-budget checks and cross-thread communication) read by the audio thread and written by the message thread — no locks cross the real-time boundary. The audio thread remains deterministic: it checks a single `std::atomic<bool>` per slot and either calls `processBlock()` or outputs silence/passthrough. (The separate `volatile sig_atomic_t` flag, if used by the in-process signal-handler fallback, is read on the message thread only to detect signal-handler-initiated bypasses.)

**Plugin selection UI:**

* When the user clicks the instrument slot on a MIDI channel, show a simple sorted list of instrument plugins (categorized by manufacturer if available). No search, no tags, no favorites — keep it simple. If the list gets long, alphabetical with a scroll is fine.
* When a plugin is selected, load it, apply its default state, and route MIDI to it immediately.
* Plugin editor: clicking the instrument slot again (or a dedicated "Edit" button) opens the plugin's own GUI in a floating window. This is the one place third-party UI is shown. The floating window has a close button and stays on top.

**Plugin state:**

* Saved per-session in `plugins/trackNN_instrument.xml` (the plugin's own serialization format via `getStateInformation()`).
* Restored on session load. If the plugin is missing (user doesn't have it installed), show the slot as "(Missing: PluginName)" in red and pass silence to the channel strip.

**Plugin latency:**

* Instrument plugins may report latency. Dusk Studio must compensate: offset the MIDI event dispatch by the plugin's reported latency so that the audio output aligns with the timeline. This is handled in the MIDI engine, not the channel strip. (See "Plugin-latency-aware look-ahead" under MIDI Playback above for the read-ahead horizon implications.)

#### **5d. MIDI Channel Strip UI Differences**

When a channel is in MIDI mode, the channel strip looks almost identical to an audio channel, with these differences at the top of the strip:

* **Input selector** shows "MIDI" (highlighted differently from Mono/Stereo)
* **MIDI channel selector** — small dropdown or knob: 1-16 or Omni
* **Instrument slot** — shows the loaded plugin name (e.g., "Dexed", "Vital", "ZynAddSubFX"). Click to change, click again to open the plugin editor.
* **MIDI activity indicator** — small LED that flashes on note-on events (confirms MIDI is reaching the track)
* **Overdub toggle** — small button specific to MIDI channels (for layered recording)

Everything below (HPF, EQ, Comp, Sends, Pan, Fader) is identical to audio channels.

#### **5e. Piano Roll Editor**

The piano roll opens as a modal overlay when double-clicking a MIDI region in the tape strip. It replaces the tape strip area (the console stays visible below).

**Layout:**

```
┌──────────────────────────────────────────────────────────┐
│ [Close X]  Track 5: "Synth Lead"   Quantize: [1/8 ▼]     │
│            Snap: [ON]  Grid: [1/16 ▼]  Overdub: [OFF]    │
├────┬─────────────────────────────────────────────────────┤
│    │ Beat 1    Beat 2    Beat 3    Beat 4    Beat 5      │
│ C5 │ ████                          ██████                │
│ B4 │          ████████                                   │
│ A4 │                    ████                             │
│ G4 │                              ██                     │
│... │                                                     │
│    │ (piano keyboard along left edge)                    │
├────┼─────────────────────────────────────────────────────┤
│Vel │ ██  ██    ████      ██  ██    ████                  │
│    │ (velocity bars, one per note)                       │
└────┴─────────────────────────────────────────────────────┘
```

**Interactions:**

* **Draw**: click in empty space to create a note at the grid position. Length = current grid size. Velocity = default (100) or last-used.
* **Select**: click a note to select it. Shift+click for multi-select. Drag a rectangle for box selection.
* **Move**: drag selected notes horizontally (time) or vertically (pitch). Snaps to grid if snap is ON.
* **Resize**: drag the right edge of a note to change its length.
* **Delete**: select notes and press Delete/Backspace.
* **Velocity**: drag the velocity bars below each note up/down. Or select notes and use a velocity slider.
* **Quantize**: select notes (or Select All), press Quantize button. Options: 1/4, 1/8, 1/16, 1/32, triplet variants. Quantize strength (50%, 75%, 100%) — keep it to a single slider, no advanced options.
* **Preview**: clicking on the piano keyboard along the left edge plays that note through the instrument plugin (auditioning).
* **Scroll/Zoom**: horizontal scroll follows the timeline. Horizontal zoom via pinch or +/- keys. Vertical scroll to see more octaves. No vertical zoom — fixed note height.

**What the piano roll does NOT have** (portastudio constraint):

* No CC lane editing in v1 (CCs and other non-note events are still recorded and played back, just not visible/editable here)
* No multiple MIDI regions open simultaneously
* No notation view
* No drum grid view (notes on a piano roll work fine for drums)
* No step input mode
* No MIDI effects/arpeggiator

**MIDI editing philosophy**: the piano roll exists for cleanup — fix wrong notes, tighten timing, adjust velocity. The performance should be captured live. The editor corrects mistakes; it doesn't compose for you.

#### **5f. External MIDI Output (Optional, Phase 5+)**

For users with hardware synths: a channel in MIDI mode could optionally route MIDI out to an external device instead of an instrument plugin. The external synth's audio would come back in through a separate audio input. This mirrors how a real portastudio with MIDI sync works. Implementation: the instrument slot shows "External: [MIDI Out Port] [Channel]" instead of a plugin name, and no instrument plugin is loaded — the channel strip expects audio from a physical input.

This is a nice-to-have, not a launch feature.

### **6. Markers System (`src/session/`)**

Markers are first-class transport features, not an afterthought:

* **Creating markers**: press the MARK button (or keyboard shortcut, e.g. M key) during playback or while stopped. A text field appears inline in the marker lane for naming. Default names auto-increment: "Marker 1", "Marker 2", etc.
* **Jumping to markers**: click a marker in the marker lane, OR use dedicated Prev/Next Marker buttons (keyboard shortcuts: comma/period or similar), OR use a dropdown list of all markers.
* **Editing markers**: double-click a marker to rename. Drag to reposition. Right-click (the ONE exception to "no right-click menus") or delete key to remove.
* **Visual display**: markers appear as flags/triangles in the marker lane at the top of the tape strip, with their names displayed. Color-coded optionally.
* **Bounce between markers**: select two markers as in/out points for bouncing a section.

### **7. Undo System (`src/session/`)**

Simple command pattern. Undoable actions are limited to:

* Region move, split, delete
* Marker create, move, rename, delete
* Knob/fader changes (group as single undo step per "gesture")

Recording is NOT undoable (portastudio philosophy — you committed to that take). But the audio file stays on disk, so a "take history" per track could surface previous recordings.

### **8. Automation System (`src/engine/`)**

Console-style "write and ride" automation, modeled after motorized fader workflows on real mixing consoles — you perform your mix moves in real time, just like riding faders on a Tascam or SSL. The audio region editor adds a per-parameter breakpoint lane (add/drag/delete points, or draw a freehand stroke with the Draw tool to lay a run of them) for cleaning up or hand-placing automation after the take. Segments stay linear between points; no spline/bezier shaping.

**Automatable parameters per channel:**

* Fader level
* Pan position
* Send 1 level
* Send 2 level
* Mute on/off
* Solo on/off

**Automatable parameters on aux buses and master:**

* Fader level
* Mute on/off
* Solo on/off (aux buses only; master has no solo)

**NOT automatable** (keeps it simple, matches the console metaphor):

* EQ parameters
* Compressor parameters
* Bus assignment
* HPF frequency

**Modes (per channel, selected via button on each strip):**

| Mode | Behavior |
| ----- | ----- |
| **Off** | No automation. Parameter stays where you set it. Default state. |
| **Read** | Plays back recorded automation. Fader/knob moves on screen to show the recorded values. User cannot override. |
| **Write** | Records ALL automatable parameters continuously while transport is playing. Overwrites any existing automation for those parameters in the played-over range. |
| **Touch** | Like Read, but when the user grabs a fader/knob, it switches to Write for that parameter. When released, it returns to Read after a short glide-back (configurable: immediate, 100ms, 500ms, 1s). This is the primary mixing mode. |

**Data model:**

```
enum class ParameterType { Continuous, Discrete };
// Continuous: faders, pan, send levels — interpolated between points.
// Discrete: mute, solo, on/off switches — instantaneous state changes, no interpolation.

struct AutomationLane {
    ParameterID paramId;          // e.g., Channel3_Fader, Channel7_Pan
    ParameterType paramType = ParameterType::Continuous;  // Controls thinning + interpolation
    std::vector<AutomationPoint> points;  // sorted by time
};

struct AutomationPoint {
    int64_t timeSamples;          // absolute timeline position
    float value;                  // normalized 0.0 - 1.0
    float recordedAtBPM;          // BPM when this point was created/recorded.
                                  // Used as canonical "old BPM" for retiming so
                                  // repeated BPM changes remain precise.
                                  // On-load migration: set to sessionBPM for
                                  // existing points that lack this field.
};
```

Automation data is stored per-track in the session JSON. Playback interpolates linearly between points. Write mode samples the current parameter value at regular intervals (e.g., every 64 samples / ~1.5ms at 44.1k) and thins redundant points on write to keep data compact.

**BPM changes and sample-locked automation**: AutomationPoint stores automation in absolute samples (`timeSamples`), so automation does NOT retime when the session BPM changes. This is the default and expected behavior — fader rides are tied to audio time, not musical time. However, if tempo-locked MIDI regions coexist with automation data, a BPM change will cause MIDI playback to shift relative to the automation (MIDI retimes musically, automation stays fixed in samples). When the user changes BPM and both automation data and unlocked MIDI regions exist, show a warning: "Automation is sample-locked and will not follow the BPM change. MIDI regions will retime but automation will remain at original positions." with three options:

* **(a) Retime automation** — for each point, compute `newTimeSamples = oldTimeSamples × (point.recordedAtBPM / newBPM)`, then update `point.recordedAtBPM = newBPM` after retiming. **Sanity check the formula direction**: at 48 kHz, beat 2 at 120 BPM sits at sample 48000; at 60 BPM the same musical instant should be at sample 96000. Computed: `48000 × (120 / 60) = 96000` ✓. The ratio is `oldBPM / newBPM` (i.e. `recordedAtBPM / newBPM`) — slower new tempo means later sample. This preserves the musical relationship between automation and MIDI and ensures repeated BPM changes remain precise (no compounding rounding error from using a global "old BPM" snapshot).
* **(b) Prompt** — ask per-track whether to retime (default).
* **(c) Leave as-is** — keep automation at original sample positions (do not update `recordedAtBPM`).

Store this as a **session-scoped** field on the session (`session.automationBpmBehavior`: `retime`, `prompt`, `leave`, default `prompt`) — it lives in `session.json`, not in a global preferences system. This keeps the "no settings sprawl" constraint intact while letting each project remember its preferred BPM-change behavior. After retiming, automation thinning/interpolation parameters remain unaffected — the retimed points maintain their original value and density. On session load, any points missing `recordedAtBPM` are migrated by setting it to the session BPM. Respect the `automationBpmBehavior` preference when deciding whether to retime per-track or globally.

**Automation thinning algorithm**: after a Write pass completes, apply a thinning pipeline that respects the lane's `paramType`:

1. **Fast pre-filter (delta + max-span)** [all lanes]: drop consecutive points where the value delta is below a threshold (default: 0.0005 normalized units) AND the time gap is below a max span (default: 2048 samples). This quickly removes near-duplicate samples with O(n) complexity. Runs for both continuous and discrete lanes.
2. **Curve-preserving simplification (Ramer-Douglas-Peucker)** [continuous lanes only]: run RDP on the surviving points with a perpendicular-distance tolerance (default: 0.001 normalized units). This preserves the shape of fader gestures while eliminating colinear interior points. **Discrete/binary lanes (Mute, Solo, etc.) are excluded from RDP** — RDP interpolation would produce intermediate values that break binary semantics. After pre-filtering, discrete lanes retain only instantaneous state-change points (value transitions).

Thinning parameters are **hard-coded constants**, not user settings: `kAutomationThinDelta = 0.0005f`, `kAutomationThinMaxSpan = 2048`, `kAutomationThinTolerance = 0.001f`. These values are deterministic and tuned once for the expected balance of file size vs gesture fidelity. Consistent with the "no settings sprawl" constraint, they are not exposed in any preferences UI — if they ever need adjustment, the change happens at the code level in a release.

**Thinning execution timing**: `handleWritePassComplete()` is invoked only when the transport stops or when Write mode is disengaged — it is NOT triggered after individual Touch mode gestures. Multiple short Touch gestures during a single playback pass do not each trigger the two-stage thinning pipeline; instead, the fast pre-filter + async Ramer-Douglas-Peucker work is deferred until the transport stops (or Write mode exits). The manual `optimizeAutomationCommand` remains available via menu/keyboard shortcut for full-session re-thinning at any time. `handleWritePassComplete()` runs the two-stage pipeline as follows:

1. The fast pre-filter (delta + max-span) runs synchronously on the message thread — it is O(n) and keeps the UI responsive immediately.
2. The heavier Ramer-Douglas-Peucker step is dispatched to a background worker thread. When complete, the thinned result is committed atomically on the message thread via `applyThinnedAutomation()` (single-threaded commit function that swaps the automation lane's point vector). This ensures thread-safety — the audio thread reads a stable snapshot, the background thread writes to a staging buffer, and the message thread performs the swap.

**Thinning timing**: hard-coded to run immediately after each Write pass (the default behavior described above). A manual `optimizeAutomationCommand` action is still available via menu/keyboard shortcut for re-thinning an entire session after bulk edits, but there is no user-facing preference for *when* thinning runs — consistent with the "no settings sprawl" constraint. Thinning/interpolation parameters remain unaffected by this choice.

**UI integration:**

* Each channel strip gets a small mode button (Off / R / W / T) that cycles through modes.
* When a channel is in Read or Touch mode and automation exists, the fader visually moves during playback — like a motorized fader.
* A thin automation "ribbon" can optionally appear below each track in the tape strip, showing the fader automation as a simple line. This is display-only — no mouse editing of the curve. If you want to change it, you ride the fader again.
* Keyboard shortcut to set all channels to a mode at once (e.g., "set all to Touch for a mix pass").

**The philosophy**: automation is a performance, not a drawing exercise. You mix the song by feel, multiple passes, riding the faders. Touch mode lets you fix just the parts that need fixing without re-doing the whole song. This is how professional engineers mixed before DAWs existed — and it's a more musical, ADHD-friendly workflow than clicking tiny dots on a screen.

### **9. Bounce / Export (`src/engine/`)**

Bouncing in Dusk Studio spans three modes, all reached from a single "Bounce / Export" dialog (opened by the transport-bar Bounce button or `Cmd/Ctrl + B`). One dialog, three radio-selected modes — no menu diving.

#### **9a. Modes**

* **Master mix** (default). Renders the master bus output to a single stereo file. Range is selectable: full session, between two markers, between loop in/out, or between punch in/out. This is the "give me the song" output.
* **Stems**. Renders each track to its own stereo file in `bounces/stems/<bouncename>/`, with the channel strip processing applied (HPF, EQ, comp, pan, fader) but **without** aux-bus or master-bus processing. A track is included if it is not muted (solo state is ignored — soloed-only stems would surprise the user when they expect all tracks). Each stem file is named `<bouncename>_track<NN>_<trackname>.wav`. Stems land at the session sample rate / bit depth (per the Bit depth technical decision). This is the modern equivalent of "give them the multitrack."
* **Render in place**. Renders a single selected region (audio or MIDI) through its channel strip to a new audio region that replaces the original on the timeline. Three canonical use cases:
  1. **Bake MIDI to audio** — commits an instrument plugin's output so subsequent BPM changes do not retime that part (already cross-referenced from the MIDI BPM-change section).
  2. **Commit channel strip processing** — for an audio region, render its strip's EQ/comp/saturation into the audio file so the strip can be reset (frees DSP for live tweaks elsewhere).
  3. **Portastudio-style ping-pong** — pick a multi-region selection across tracks (using range select), render to a single audio region that replaces them, freeing the source tracks. The "ping-pong" of cassette portastudios applied to disk-based recording.
  Render-in-place creates a new WAV in `audio/` named `track<NN>_render<NN>.wav`. The original region's `previousTakes` retains the unrendered source so the operation is reversible (swap to the previous take to undo the render).

#### **9b. Realtime vs offline**

Default to **offline rendering**. The fixed signal graph is overwhelmingly time-invariant — EQ, compressors, saturation, and the Pultec/tape master chain all process samples deterministically without depending on wall-clock time, so they render correctly at faster-than-realtime. Offline render uses a non-realtime `AudioPlayHead` and processes the whole graph as fast as the CPU allows.

A plugin (instrument or send) may declare it cannot run offline (`AudioProcessor::supportsDoublePrecisionProcessing()` is unrelated; the relevant signal is `getTailLengthSeconds()` plus an internal "needs realtime" hint that some plugins set via parameter or vendor extension). If any plugin in the active graph cannot guarantee correct offline behavior, the dialog auto-switches to **realtime mode** with an inline note: "Realtime render required — plugin '<Name>' does not support offline processing." The user can still proceed; the bounce just runs at 1× speed.

The render runs on a dedicated `juce::Thread` (not the audio thread). Progress is shown as a determinate bar with an ETA based on the elapsed-vs-remaining sample ratio. Cancel is non-blocking — the dialog cancels the thread on the next callback boundary; partial output is discarded.

#### **9c. Format and metadata**

* **Sample rate**: always the session sample rate. No upsample / downsample on bounce — the user can resample externally if they need 44.1k delivery from a 48k session.
* **Bit depth**: 24-bit WAV by default (matches the recording format and the dithering pipeline in the Bit depth technical decision). FLAC option in the dialog for archive / handoff. No 16-bit / no MP3 / no other lossy formats in v1 — those belong in an external converter, not the DAW.
* **Stereo only**: master mix and stems are always stereo (left/right). No surround. No mono master. (Mono sources are panned into stereo per the channel strip's pan; a mono source with pan centered produces an L=R bounce.)
* **Click**: excluded by default. The export dialog has an "Include click track" checkbox (default off) — useful for practice tracks but never on by default.
* **Filename**: defaults to `<sessionname>_<modename>_<NNN>.wav`, where `<NNN>` is an auto-incrementing 3-digit counter so consecutive bounces don't overwrite each other. The user can edit the filename before render.

#### **9d. Output paths**

```
<sessionDirectory>/bounces/
  <sessionname>_master_001.wav        # Master mix bounces
  <sessionname>_master_002.wav
  stems/
    <bouncename>_001/                 # Each stem bounce gets its own subfolder
      <bouncename>_track01_kick.wav
      <bouncename>_track02_snare.wav
      ...
```

(Render-in-place is not in `bounces/` — its output lives in `audio/` because it's a regular session asset, not an export artifact.)

#### **9e. State during bounce**

While a bounce is running:

* The audio engine continues live monitoring (so the user hears their session normally).
* The bounce thread reads from a snapshot of the session model (regions, automation, plugin state) taken at bounce start. Live edits during bounce do not affect the in-flight render.
* Recording is disabled during bounce (the RECORD button is greyed out). Trying to record cancels the bounce with a confirmation prompt.
* Saving the session is allowed during bounce; the bounce thread holds its snapshot independently.

## **Project Structure**

```
Dusk Studio/
├── CMakeLists.txt
├── CLAUDE.md                         # Project-specific instructions
├── src/
│   ├── Main.cpp
│   ├── DuskStudioApp.h/cpp
│   │
│   ├── engine/
│   │   ├── AudioEngine.h/cpp         # Real-time callback, routes everything
│   │   ├── DiskStreamer.h/cpp        # Ring-buffered WAV read/write
│   │   ├── Transport.h/cpp           # State machine + locate + loop
│   │   ├── RecordManager.h/cpp       # Manages take files, punch crossfades
│   │   ├── Metronome.h/cpp           # Click track generator
│   │   ├── AutomationEngine.h/cpp    # Write/Read/Touch mode, lane playback
│   │   └── Bouncer.h/cpp             # Master / Stems / Render-in-place rendering
│   │
│   ├── session/
│   │   ├── Session.h/cpp             # The model (tracks, regions, markers)
│   │   ├── Track.h/cpp
│   │   ├── Region.h/cpp
│   │   ├── Marker.h/cpp
│   │   ├── AutomationLane.h/cpp      # Per-parameter automation data
│   │   ├── SessionSerializer.h/cpp   # JSON save/load (with autosave)
│   │   └── UndoManager.h/cpp         # Command pattern
│   │
│   ├── dsp/
│   │   ├── ChannelStrip.h/cpp        # HPF + EQ + Comp per channel
│   │   ├── FourBandEQ.h/cpp          # Extracted from 4K EQ
│   │   ├── DualModeCompressor.h/cpp  # Extracted from Multi-Comp (FET/Opto)
│   │   ├── PultecEQ.h/cpp            # Extracted from Multi-Q
│   │   ├── BusCompressor.h/cpp       # For aux + master buses
│   │   ├── TapeEmulation.h/cpp       # Extracted from TapeMachine
│   │   └── HighPassFilter.h/cpp      # Simple biquad HPF
│   │
│   ├── midi/
│   │   ├── MidiEngine.h/cpp          # MIDI routing, recording, playback, tick→sample
│   │   ├── MidiRegionPlayer.h/cpp    # Reads MidiRegion events, dispatches to plugin
│   │   ├── MidiRecorder.h/cpp        # Captures live MIDI events with timestamps
│   │   ├── PluginHost.h/cpp          # VST3/LV2 scanning, loading, state management
│   │   ├── PluginSlot.h/cpp          # Single plugin instance wrapper (instrument or effect)
│   │   └── PianoRollEditor.h/cpp     # Minimal MIDI note editor (overlay UI)
│   │
│   └── ui/
│       ├── DuskStudioLookAndFeel.h/cpp  # Dark console aesthetic
│       ├── ConsoleView.h/cpp             # The mixer (16 ch + 4 aux + master)
│       ├── ChannelStripComponent.h/cpp   # Single channel strip widget
│       ├── AuxBusComponent.h/cpp         # Aux bus strip widget
│       ├── MasterBusComponent.h/cpp      # Master strip widget
│       ├── TapeStrip.h/cpp               # Arrangement / timeline view
│       ├── MarkerLane.h/cpp              # Marker display + interaction
│       ├── TransportBar.h/cpp            # Transport controls + BPM + position
│       ├── MeterBridge.h/cpp             # LED-style peak/RMS meters
│       ├── KnobComponent.h/cpp           # Console-style rotary knob
│       └── FaderComponent.h/cpp          # Vertical channel fader
│
├── resources/
│   ├── fonts/                        # Console-appropriate typeface
│   └── graphics/                     # Knob/fader/button textures if needed
│
└── tests/
    ├── EngineTests.cpp
    ├── SessionTests.cpp
    └── TransportTests.cpp
```

## **Implementation Phases**

Execute these in order. Each phase produces a usable, testable artifact. Do NOT skip ahead.

### **Phase 1a: Live Mixer (no recording, no plugin hosting)**

Build a JUCE standalone app that:

* Opens the default audio device (JACK/PipeWire)
* Shows 24 channel strips with HPF, 4-band EQ, compressor, send knobs, pan, fader
* Routes audio inputs through the channel strips and sums to master output
* Shows LED meters on each channel and the master bus
* Includes the 4 aux bus strips with EQ + compressor
* Includes the master strip with Pultec EQ + bus compressor + tape saturation
* Implements solo-in-place (SIP) per the Solo semantics section
* All DSP is extracted from the existing Dusk Audio plugin codebase (see the Shared DSP Cores note below)
* Send 1 / Send 2 levels are wired and route to silent buses (no plugin yet — that's Phase 1b)

**Verification**: open the app, connect a mic or instrument via JACK/PipeWire, hear processed audio through the channel strip, adjust EQ/compression in real time, see meters respond, solo a channel and confirm SIP behavior, hear master tape saturation + Pultec engagement.

### **Phase 1b: Send-bus Plugin Hosting**

Add to Phase 1a:

* `AudioPluginFormatManager` + `KnownPluginList` infrastructure
* Background, process-isolated plugin scan on first launch (cached to `~/.config/Dusk Studio/plugin-cache.json`)
* Plugin selection UI on the two send-bus strips (sorted list, no search)
* Single `PluginSlot` per send bus with the time-budget bypass mechanism
* Floating plugin editor window
* Plugin state save/restore against the (still in-memory) session

**Verification**: load a reverb plugin into Send 1, hear send returns mix into the master. Confirm a deliberately stalling plugin trips the time-budget bypass and silence/passthrough engages.

### **Phase 2: Multitrack Recorder**

Add to Phase 1b:

* Transport controls (play, stop, record)
* Disk streaming engine (ring-buffered read/write)
* Metronome / click track (engine-side, mute-able from the transport bar)
* Arm tracks for recording, record to WAV files. RECORD requires at least one armed track (see Transport section).
* Playback recorded audio through the mixer
* Session model with save/load (JSON, atomic write, autosave every 30s while dirty)
* Basic tape strip view (colored blocks for recorded regions)
* Latency compensation on recorded regions

**Verification**: record a 4-track song to a click, stop, play it back through the mixer, save the session, kill the process, reopen it, recover from autosave.

### **Phase 3: Arrangement + Markers + Automation**

Add to Phase 2:

* Marker system: create, name, jump-to, navigate, delete markers
* Region editing in the tape strip: move, split, delete
* Punch in/out recording with crossfades (PunchRange invariants per the Session Model section)
* Loop playback between two points (LoopRange invariants)
* Undo system for region and marker edits
* Region operations on the tape strip: non-destructive trim (drag region edges), take history per region (badge picker), copy/paste between markers (range copy)
* Bounce / Export dialog with three modes — Master mix, Stems, Render in place — defaulting to offline render at session sample rate / 24-bit WAV (per §9)
* **Fader automation**: Off/Read/Write/Touch modes per channel, records fader/pan/send/mute gestures during playback, plays them back with visual fader movement
* Automation mode buttons on each channel strip (Off / R / W / T)
* Optional automation ribbon display below tracks in the tape strip

**Verification**: record a song with verse/chorus structure, place markers at each section, punch in to fix a part. Trim the tail of one region (drag the right edge inward); confirm the underlying WAV is unchanged. Punch a new take, swap between takes via the region badge. Select the verse marker range, copy, paste at the second-verse marker — both verses play. Set channels to Touch mode, loop the chorus, ride the faders to mix it. Play back and watch the faders move. Bounce the final mix as a master, then bounce stems, and confirm both land in `bounces/` with the expected file layout.

### **Phase 4: MIDI**

Add to Phase 3:

* MIDI input mode on channels (input selector → MIDI)
* Plugin scanning expanded to instrument category (already scanning effects from Phase 1b)
* Instrument plugin hosting per MIDI channel (load, route MIDI, get audio)
* Plugin state save/restore per session (extends the Phase 1b save format)
* MIDI recording with tick-based timestamps (480 PPQN, tempo-relative); captures notes + CCs + pitch bend + aftertouch
* MIDI playback through instrument plugins (tick→sample conversion, plugin-latency-aware look-ahead)
* Real-time MIDI monitoring (hear yourself while playing, even when stopped)
* MIDI overdub mode (merge new events into existing region)
* MIDI punch in/out with hanging-note protection
* Piano roll editor overlay (draw, select, move, resize, delete notes + velocity + quantize) — notes-only editing in v1
* Plugin latency compensation for MIDI event dispatch
* MIDI channel filtering per track (1-16 or omni)
* MIDI activity LED on each MIDI channel strip

**Verification**: set track 5 to MIDI, load a synth (e.g., Vital), play keys and hear audio through the channel strip's EQ/comp. Record a 4-bar phrase, open the piano roll, quantize to 1/8, fix a wrong note, adjust velocities. Switch to overdub, layer a second pass of notes. Change the instrument plugin to a different synth — same MIDI, different sound. Save session, reopen, plugin and MIDI data restored.

### **Phase 5: Polish**

* Tap tempo
* Session templates ("New Song" → ready to record immediately)
* Keyboard shortcuts for everything (transport, markers, track arm, solo/mute)
* Resizable UI with the shared `ScalableEditorHelper` pattern
* Visual polish: console-dark color scheme, LED glow, fader shadows
* CPU meter
* Audio device settings dialog (buffer size, sample rate, I/O assignment)

## **Key Constraints (Enforce These)**

1. **24 channels maximum.** Do not make this configurable. The constraint is the feature. (24 is chosen so that three banks of 8 — 1-8 / 9-16 / 17-24 — map cleanly onto every standard control surface — Mackie Control, FaderPort 16, X-Touch Compact, BCF2000. Bank-switching is built into the control-surface protocol; on screen the full 24 strips remain visible.)
2. **Fixed signal chain.** No reordering EQ/comp. No adding/removing processors. No plugin chains on channels.
3. **No waveform editing.** Regions can be moved, split, and deleted. No fades, no time-stretching, no pitch-shifting, no crossfade editing.
4. **Console-style automation + breakpoint editing.** Fader, pan, mute, and send levels are automated by riding the control during playback (Write/Touch) and reading it back — motorized-fader style. The audio region editor adds a per-parameter breakpoint lane (add/drag/delete points, or draw a freehand stroke with the Draw tool to lay a run of them) for hand-placing or cleaning up automation after the take. Segments stay linear between points; no spline/bezier shaping. See the Automation section above for full spec.
5. **Everything visible.** No hidden panels, no tabs (except the MIDI piano roll overlay). If it's not on screen, it doesn't exist.
6. **No preferences/settings sprawl.** Audio device config and that's it. No themes, no layout customization, no plugin chain options.
7. **Portastudio philosophy.** Stay fixed, finite, and commit-first; if a feature mainly adds configurability or options, leave it out.

## **DSP Extraction Notes**

The existing Dusk Audio plugins are JUCE `AudioProcessor` subclasses with editors. For Dusk Studio, you need the **DSP cores only**, not the plugin wrappers or UIs:

* **4K EQ**: extract the 4-band filter processing (likely IIR biquads with oversampling). The parameter ranges and filter types define the character.
* **Multi-Comp**: extract the compressor DSP with its FET and Opto detector modes. You need the detection, gain reduction, and makeup gain logic.
* **Multi-Q**: extract the Pultec EQ mode specifically — the simultaneous boost/cut behavior on the low band is what makes it a Pultec.
* **TapeMachine**: extract the tape saturation/coloration DSP. This becomes the master bus character.
* **Shared AnalogEmulation**: use `TubeEmulation.h` and `WaveshaperCurves.h` directly.

Each extracted DSP class should have a simple interface:

```
void prepare(double sampleRate, int maxBlockSize);
void process(juce::AudioBuffer<float>& buffer);
void setParameter(ParameterID id, float value);
```

### **Shared DSP cores (single source of truth)**

The reusable DSP cores live in a new subfolder of the existing Dusk Audio plugins repo: `/home/marc/projects/plugins/plugins/shared/dsp-cores/`. Both Dusk Studio and the existing Dusk Audio plugins depend on those headers. This keeps a single source of truth — bug fixes propagate to both consumers without manual porting. Cores in scope: 4-band EQ filter chain, dual-mode (FET/Opto) compressor, Pultec tube EQ, Jiles-Atherton tape saturation, console saturation. The `shared/AnalogEmulation/` library is already header-only and parameter-agnostic; it is included as-is. Where a donor plugin's DSP is currently bound to APVTS atomic pointers (notably 4K EQ and the Multi-Comp mode classes), the extraction step decouples that into a parameter-struct API before lifting into `dsp-cores/`.

## **Technical Decisions**

* **Sample rate**: prefer 44.1k or 48k; also accept 88.2k and 96k. At audio device initialization (`AudioEngine::initialize`), check available hardware sample rates. Prefer 44100 or 48000 when available (in that order). If only 88200 or 96000 is available, accept it but show a non-blocking warning dialog: "Running at [rate]kHz — some plugins may not support this rate, and CPU usage will be higher." If none of {44100, 48000, 88200, 96000} is supported, show an error dialog and refuse to start. As a future option, add a user preference to allow any hardware rate with automatic internal resampling (with a latency/CPU warning in the settings UI).
* **Bit depth**: 32-bit float internal, record to 24-bit WAV files. When recording or exporting (`AudioFileWriter::writeWav`), always write 24-bit WAV: promote 16-bit inputs to 24-bit, dither 32-bit float down to 24-bit as a preprocessing step before writing. JUCE's `WavAudioFormat::createWriterFor` and `AudioFormatWriterOptions` do not provide built-in dithering, so `AudioFileWriter::writeWav` must apply triangular PDF (TPDF) dithering to the 32-bit float buffer before calling the writer — add ±1 LSB of uniform random noise to each sample, then quantize to 24-bit. The original imported file is never modified — the session stores/exports the converted 24-bit version.
* **Audio import**: when importing audio files (`Session::importAudioFile`), resample any file whose sample rate does not match the session rate using JUCE's `juce::LagrangeInterpolator`. Leave the original file untouched; store the resampled copy in the session directory.
* **Buffer sizes**: 64, 128, 256, 512, 1024, 2048. Default 256.
* **File format**: WAV for audio, JSON for session (including inline MIDI data).
* **Threading**: real-time audio thread (JUCE callback), disk I/O thread (`TimeSliceThread`), UI thread (JUCE message thread). Lock-free communication between audio and disk threads via `AbstractFifo`.
* **Dependencies**: JUCE only. No other libraries. All DSP is internal.
