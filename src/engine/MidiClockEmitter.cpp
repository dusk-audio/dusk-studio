#include "MidiClockEmitter.h"

namespace duskstudio
{
void MidiClockEmitter::generateBlock (juce::int64 blockStartSample,
                                       int numSamples,
                                       float bpm,
                                       bool isRolling,
                                       juce::MidiBuffer& out) noexcept
{
    if (numSamples <= 0 || sr <= 0.0) return;

    // Transport edges. FA = Start (from beginning) on rising edge, FC
    // = Stop on falling edge. Both land at sample offset 0 of the
    // current block - sub-block placement isn't worth the complexity
    // here because slaves react with their own latency anyway.
    if (isRolling != lastRolling)
    {
        const juce::uint8 statusByte = isRolling ? (juce::uint8) 0xFA   // Start
                                                  : (juce::uint8) 0xFC; // Stop
        out.addEvent (juce::MidiMessage (statusByte), 0);
        lastRolling = isRolling;
        if (isRolling)
        {
            // Snap the next clock to the start sample so the first F8
            // after Start lands on the downbeat. Without this, the
            // first tick could drift up to a full samplesPerClock
            // depending on where the phase sat from prior runs.
            clockPhase = (double) blockStartSample;
        }
    }

    if (bpm <= 0.0f) return;
    const double samplesPerClock = (sr * 60.0) / ((double) bpm * 24.0);
    if (samplesPerClock <= 0.0) return;

    const juce::int64 blockEnd = blockStartSample + (juce::int64) numSamples;
    // First emission: if the emitter has no phase yet (clockPhase
    // <= blockStart - 2 * samplesPerClock for a "stale" gap), realign
    // to blockStart so we don't burst many catch-up clocks.
    if (clockPhase + samplesPerClock * 2.0 < (double) blockStartSample)
        clockPhase = (double) blockStartSample;

    // Round only at emission; the phase itself advances by the exact
    // fractional interval so the average tick rate matches the tempo.
    while ((juce::int64) std::llround (clockPhase) < blockEnd)
    {
        const int offset = (int) ((juce::int64) std::llround (clockPhase)
                                   - blockStartSample);
        if (offset >= 0 && offset < numSamples)
            out.addEvent (juce::MidiMessage ((juce::uint8) 0xF8), offset);
        clockPhase += samplesPerClock;
    }
}
} // namespace duskstudio
