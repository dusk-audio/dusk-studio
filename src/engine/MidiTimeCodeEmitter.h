#pragma once

#include "MidiTimeCodeReceiver.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace duskstudio
{
// Generates MTC (MIDI Time Code) quarter-frame + full-frame sysex
// bytes into a per-block MidiBuffer. Companion to MidiClockEmitter —
// Clock encodes tempo, MTC encodes absolute SMPTE time. Audio-thread
// safe; only POD state. Caller multiplexes the buffer onto the same
// MidiOutput::sendBlockOfMessages route as Clock.
//
// Quarter-frame cadence: 4 QFs per frame × frameRate Hz. At 30 fps
// that's 120 QF/sec → samplesPerQF = sr / 120 ≈ 400 samples at 48 k.
// 8 QFs assemble one full SMPTE value (the value sampled at the START
// of the 8-QF sequence — which is 2 frames behind wall time when QF7
// lands on the wire).
//
// Standard 2-frame transmission-delay compensation (emit side):
// the slave receives QF7 and decodes the value carried in the
// sequence, then applies its own +2 frame offset to align with wall
// time. For that to match the master's display, the master encodes
// `currentFrames - 2` at sequence-start; by QF7 wall-time advances
// 2 frames, slave decodes (currentFrames - 2) + 2 == currentFrames.
// Symmetric with MidiTimeCodeReceiver.
//
// Full-frame sysex (F0 7F 7F 01 01 hh mm ss ff F7) fires on:
//   - transport rolling-edge false→true (Locate + Play)
//   - frame-rate change mid-emission
//   - mid-sequence playhead jump > kJumpDetectFrames frames from the
//     predicted continuous value (user click on ruler)
// Sysex carries an instantaneous value (no -2 offset) — the slave
// skips its +2 compensation for sysex.
class MidiTimeCodeEmitter
{
public:
    using FrameRate = MidiTimeCodeReceiver::FrameRate;

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    // Audio-thread reset: source/output change, transport stop, or
    // user toggling emission. Forgets QF phase + frame-rate + last
    // playhead so the next emission starts cleanly with a full-frame
    // sysex locate.
    void reset() noexcept
    {
        nibbleIdx = 0;
        sequenceFrames = 0;
        nextQuarterFrameSample = 0;
        lastRolling = false;
        lastEmittedRate = (int) MidiTimeCodeReceiver::kDefaultRate;
        needFullFrameSysex = true;
    }

    // Audio-thread entry. blockStartSample is the engine's monotonic
    // sync clock (shared with MidiClockEmitter + MidiSyncReceiver).
    // playheadSamples is transport.getPlayhead() in absolute samples;
    // converted to SMPTE frames at `rate`. `out` is the shared
    // MidiBuffer (Clock + MTC multiplex on the same buffer).
    void generateBlock (juce::int64 blockStartSample,
                        int numSamples,
                        juce::int64 playheadSamples,
                        bool isRolling,
                        FrameRate rate,
                        juce::MidiBuffer& out) noexcept;

private:
    // Detect-and-locate threshold: if the live playhead differs from
    // the linear-projection of the previous QF's sequenceFrames by
    // more than this many frames, the user must have jumped (ruler
    // click, marker locate, etc.) — emit a fresh full-frame sysex
    // and restart the QF sequence so slaves resync without drift.
    // 4 not 2: sequenceFrames is stale by up to 2 frames across one
    // 8-QF sequence (refreshed only at nibble 0), so liveFrames -
    // sequenceFrames - 2 climbs to ~2 during normal playback.
    // Threshold > 2 + some rounding margin = 4.
    static constexpr juce::int64 kJumpDetectFrames = 4;

    void emitQuarterFrame (juce::int64 atSample,
                            int nibble,
                            juce::int64 frames,
                            FrameRate rate,
                            juce::MidiBuffer& out) noexcept;
    void emitFullFrameSysex (juce::int64 atSample,
                              juce::int64 frames,
                              FrameRate rate,
                              juce::MidiBuffer& out) noexcept;

    double sr = 48000.0;

    // QF emission state. nibbleIdx is which nibble (0..7) we'll
    // emit next; sequenceFrames is the SMPTE-frame value being
    // encoded across the 8 nibbles of the CURRENT sequence (frozen
    // at sequence start to satisfy the 2-frame transmission-delay
    // compensation; refreshed on nibble 0 of each new sequence).
    int         nibbleIdx              = 0;
    juce::int64 sequenceFrames         = 0;
    juce::int64 nextQuarterFrameSample = 0;

    bool lastRolling          = false;
    int  lastEmittedRate      = (int) MidiTimeCodeReceiver::kDefaultRate;
    bool needFullFrameSysex   = true;   // armed on reset / start / rate change / jump

    // Captured at generateBlock entry for the emit helpers — they
    // need to convert absolute sample positions into per-block
    // MidiBuffer offsets, which is `sample - blockStart`.
    juce::int64 lastBlockStart = 0;
};
} // namespace duskstudio
