#include "Metronome.h"
#include <cmath>

namespace duskstudio
{
void Metronome::prepare (double sampleRate)
{
    sr = sampleRate;
    clickLength = (int) (sampleRate * 0.005);  // 5 ms click
    if (clickLength < 4) clickLength = 4;
    reset();
}

void Metronome::reset() noexcept
{
    clickPos = -1;
    lastBeatIdx = std::numeric_limits<juce::int64>::min();
    lastBeatSeeded = false;
}

void Metronome::process (juce::int64 playheadStart, bool transportRolling,
                          float* L, float* R, int numSamples,
                          bool forceEnable) noexcept
{
    if (sr <= 0.0 || L == nullptr || R == nullptr) return;
    const bool effectiveEnabled = forceEnable
                                  || enabled.load (std::memory_order_relaxed);
    if (! effectiveEnabled)
    {
        // Disabled - but if a click was already sounding, finish it so we
        // don't hard-cut audio. New clicks are NOT triggered.
        if (clickPos < 0) return;
    }

    // If the transport isn't rolling, drop the seeded-beat anchor so the
    // next Play / seek re-seeds without firing a stray click.
    if (! transportRolling) lastBeatSeeded = false;

    const float bpm = bpm_.load (std::memory_order_relaxed);
    if (bpm <= 0.0f) return;

    const double samplesPerBeat = sr * 60.0 / (double) bpm;
    if (samplesPerBeat < 1.0) return;

    const int   bpb  = juce::jmax (1, beatsPerBar.load (std::memory_order_relaxed));
    const float vol  = juce::Decibels::decibelsToGain (
                         volumeDb.load (std::memory_order_relaxed));

    for (int i = 0; i < numSamples; ++i)
    {
        const juce::int64 absSample = playheadStart + i;

        // Beat-edge detection: trigger a click whenever we cross from
        // "before beat N" to "at beat N or beyond" while the transport
        // is rolling. Negative playheads (count-in) still tick.
        if (transportRolling && effectiveEnabled)
        {
            const juce::int64 beatIdx = (absSample >= 0)
                ? (juce::int64) ((double) absSample / samplesPerBeat)
                : (juce::int64) std::ceil ((double) absSample / samplesPerBeat) - 1;

            if (! lastBeatSeeded)
            {
                // First sample after start/seek: seed without firing -
                // we don't want a stray click on Play if the playhead
                // happens to land on a beat boundary.
                lastBeatIdx = beatIdx;
                lastBeatSeeded = true;
            }
            else if (beatIdx != lastBeatIdx)
            {
                // We've crossed (at least one) beat boundary in this
                // sample. Start a new click; ignore the rare case of
                // multiple boundaries in one sample at very high BPM -
                // BPM > sampleRate × 60 isn't musical anyway.
                clickPos = 0;
                const int beatInBar = (int) (((beatIdx % bpb) + bpb) % bpb);
                // Downbeat (beat 0) accent: higher pitch + slightly louder
                // via envelope below.
                clickFreq = (beatInBar == 0) ? 1200.0f : 880.0f;
                lastBeatIdx = beatIdx;
            }
        }

        // Render the in-flight click sample.
        if (clickPos >= 0 && clickPos < clickLength)
        {
            const float t = (float) clickPos / (float) sr;
            const int rampLen = juce::jmax (1, clickLength / 4);
            float env = 1.0f;
            if (clickPos < rampLen)
                env = (float) clickPos / (float) rampLen;
            else if (clickPos > clickLength - rampLen)
                env = (float) (clickLength - clickPos) / (float) rampLen;

            const float accent = (clickFreq > 1000.0f) ? 1.4f : 1.0f;
            const float s = std::sin (2.0f * juce::MathConstants<float>::pi
                                       * clickFreq * t)
                             * env * vol * accent;
            L[i] += s;
            R[i] += s;

            ++clickPos;
            if (clickPos >= clickLength) clickPos = -1;
        }
    }
}
} // namespace duskstudio
