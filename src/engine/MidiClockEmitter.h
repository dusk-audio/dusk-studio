#pragma once

#include "../foundation/MidiBuffer.h"
#include <cstdint>

namespace duskstudio
{
// Generates MIDI Clock (F8) + transport (FA / FC) bytes for a per-block
// MidiBuffer. The caller routes the buffer to a MIDI output port for
// delivery on its background thread. Audio-thread safe - the emitter holds
// only POD state and writes timestamped bytes into a pre-allocated MidiBuffer.
//
// Sample-accurate placement: F8 ticks fire on a monotonic sample clock
// that advances independent of the transport playhead. This means a
// stop/start round-trip preserves clock continuity (the master's clock
// doesn't restart) - downstream slaves expect that. Transport bytes
// (FA / FC) are edge-triggered on isRolling transitions.
//
// Convention: emit FA (Start) on any false -> true rolling transition.
// FB (Continue from current position) isn't differentiated in v1 -
// most masters just send FA; users with picky slaves can wire Continue
// later if needed.
class MidiClockEmitter
{
public:
    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        clockPhase = 0.0;
        lastRolling = false;
    }

    // Audio-thread reset (sample-rate change or user disabling the
    // emitter). Forgets the clock phase so the next emission starts
    // cleanly.
    void reset() noexcept
    {
        clockPhase = 0.0;
        lastRolling = false;
    }

    // Audio-thread entry. blockStartSample is the engine's monotonic
    // sync clock (NOT transport playhead - the playhead jumps on
    // loop / scrub and would discontinue clock); bpm is the engine's
    // active tempo. Writes F8 (Clock) bytes at the right sample offsets
    // into `out`, plus FA (Start) / FC (Stop) on rolling-flag edges.
    void generateBlock (std::int64_t blockStartSample,
                        int numSamples,
                        float bpm,
                        bool isRolling,
                        dusk::MidiBuffer& out) noexcept;

private:
    double sr = 48000.0;
    // Fractional tick clock. Advancing by the exact samplesPerClock keeps
    // the long-term rate correct; rounding only at emission. An int64 that
    // advanced by llround(samplesPerClock) per tick accumulated the same
    // rounding error every tick (+0.5 sample at any x.5 interval ≈ 0.05 %
    // tempo error — slaves drifted ~90 ms over a 3-minute song).
    double clockPhase = 0.0;
    bool lastRolling = false;
};
} // namespace duskstudio
