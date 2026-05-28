#pragma once

#include "MidiTimeCodeReceiver.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace duskstudio
{
// MTC emitter — companion to MidiClockEmitter. QFs at 4 × frameRate Hz
// (120 QF/s at 30 fps; ~400 samples per QF at 48 k). 8 QFs assemble
// one SMPTE value, the value sampled at sequence START — 2 frames
// behind wall time when QF7 lands.
//
// 2-frame transmission-delay compensation: master encodes
// (currentFrames - 2) at sequence start; by QF7 wall-time has
// advanced 2 frames; slave decodes value + 2 == currentFrames.
// Symmetric with MidiTimeCodeReceiver.
//
// Full-frame sysex (F0 7F 7F 01 01 hh mm ss ff F7) fires on:
//   - rolling false->true edge (Locate + Play)
//   - frame-rate change mid-emission
//   - mid-sequence playhead jump > kJumpDetectFrames
// Sysex is instantaneous (no -2) — slave skips its +2.
class MidiTimeCodeEmitter
{
public:
    using FrameRate = MidiTimeCodeReceiver::FrameRate;

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        reset();
    }

    // Source/output change, transport stop, or toggle. Next emission
    // starts with a full-frame sysex locate.
    void reset() noexcept
    {
        nibbleIdx = 0;
        sequenceFrames = 0;
        nextQuarterFrameSample = 0;
        lastRolling = false;
        lastEmittedRate = (int) MidiTimeCodeReceiver::kDefaultRate;
        needFullFrameSysex = true;
    }

    // blockStartSample = engine's monotonic sync clock (shared with
    // MidiClockEmitter + MidiSyncReceiver). `out` is the shared
    // MidiBuffer (Clock + MTC mux on the same buffer).
    void generateBlock (juce::int64 blockStartSample,
                        int numSamples,
                        juce::int64 playheadSamples,
                        bool isRolling,
                        FrameRate rate,
                        juce::MidiBuffer& out) noexcept;

private:
    // > 2 + rounding margin. sequenceFrames is stale by up to 2 frames
    // across one 8-QF sequence (refreshed only at nibble 0) so live -
    // sequence - 2 climbs to ~2 during normal play; 4 = jump.
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

    // sequenceFrames is frozen at sequence start (2-frame TDC), refreshed
    // on nibble 0 of each new sequence.
    int         nibbleIdx              = 0;
    juce::int64 sequenceFrames         = 0;
    juce::int64 nextQuarterFrameSample = 0;

    bool lastRolling          = false;
    int  lastEmittedRate      = (int) MidiTimeCodeReceiver::kDefaultRate;
    bool needFullFrameSysex   = true;

    // Cached so emit helpers can do (sample - blockStart) for the
    // MidiBuffer offset.
    juce::int64 lastBlockStart = 0;
};
} // namespace duskstudio
