#pragma once

#include <algorithm>

// Stateful 4th-order (5-point) Lagrange resampler, the algorithm JUCE's
// LagrangeInterpolator implements: a 5-sample history ring is fed one input
// sample per whole step of the read position, and each output sample is the
// degree-4 polynomial through those 5 samples evaluated at the fractional
// position. History carries across process() calls, so a stream is resampled
// block by block; reset() drops it at a discontinuity (seek, new file).
// Algorithmic latency is 2 input samples.
//
// No allocation and no branching on buffer size - safe on the audio thread.
// One instance per channel; sharing one across channels smears their histories.
namespace dusk::audio
{
class LagrangeInterpolator
{
public:
    LagrangeInterpolator() noexcept { reset(); }

    void reset() noexcept
    {
        std::fill (std::begin (history), std::end (history), 0.0f);
        writeIndex   = 0;
        subSamplePos = 1.0;
    }

    // Produces `numOutputSamples` from `input`, consuming `speedRatio` input
    // samples per output sample, and returns how many were actually consumed.
    // The carried fractional position can pull in up to two samples beyond
    // speedRatio * numOutputSamples, so size `input` with that headroom.
    int process (double speedRatio,
                 const float* input,
                 float* output,
                 int numOutputSamples) noexcept
    {
        int numUsed = 0;
        double pos = subSamplePos;

        for (int i = 0; i < numOutputSamples; ++i)
        {
            while (pos >= 1.0)
            {
                history[(size_t) writeIndex] = input[numUsed++];
                if (++writeIndex == kHistorySize)
                    writeIndex = 0;
                pos -= 1.0;
            }

            output[i] = valueAtOffset ((float) pos);
            pos += speedRatio;
        }

        subSamplePos = pos;
        return numUsed;
    }

private:
    static constexpr int kHistorySize = 5;

    // The oldest history sample sits at writeIndex; the five samples span node
    // positions -2..+2 and `offset` in [0, 1) is measured from node 0, which is
    // where the two-sample latency comes from.
    float valueAtOffset (float offset) const noexcept
    {
        float y[kHistorySize];
        int index = writeIndex;
        for (int k = 0; k < kHistorySize; ++k)
        {
            y[k] = history[(size_t) index];
            if (++index == kHistorySize)
                index = 0;
        }

        const float d[kHistorySize] = { offset + 2.0f, offset + 1.0f, offset,
                                        offset - 1.0f, offset - 2.0f };
        // Reciprocals of the Lagrange denominators for nodes -2..+2.
        constexpr float scale[kHistorySize]
            = { 1.0f / 24.0f, -1.0f / 6.0f, 1.0f / 4.0f, -1.0f / 6.0f, 1.0f / 24.0f };

        float result = 0.0f;
        for (int k = 0; k < kHistorySize; ++k)
        {
            float c = y[k] * scale[k];
            for (int j = 0; j < kHistorySize; ++j)
                if (j != k)
                    c *= d[j];
            result += c;
        }
        return result;
    }

    float  history[kHistorySize] {};
    int    writeIndex   = 0;
    double subSamplePos = 1.0;
};
} // namespace dusk::audio
