#pragma once

#include <cmath>

// Stateful windowed-sinc resampler, the algorithm JUCE's
// WindowedSincInterpolator implements: a 200-sample history ring is fed one
// input sample per whole step of the read position, and each output sample is
// 201 taps of a Hann-windowed sinc kernel, read from a shared lookup table at
// the fractional position. History carries across process() calls, so a stream
// is resampled block by block; reset() drops it at a discontinuity (seek, new
// file). Algorithmic latency is 100 input samples.
//
// The table is built once, on the first construction, and process() itself
// neither allocates nor locks. 201 taps per output sample is still an offline
// cost - this is the import and conform resampler, not one for a block
// callback. One instance per channel; sharing one smears their histories.
namespace dusk::audio
{
namespace detail
{
// The kernel JUCE ships as a 10001-entry float literal, generated here rather
// than copied. Hann-windowed sinc over 100 zero crossings, sampled 99 steps
// per crossing while the reader below steps 100 - so the kernel comes back 1%
// narrower than it was built, and the last taps run off the end of the window
// into the zero tail. Both belong to this filter's response rather than being
// slips to correct: the table exists to match JUCE's output sample for sample.
struct WindowedSincTable
{
    static constexpr int kCrossings             = 100;
    static constexpr int kBuildStepsPerCrossing = 99;
    static constexpr int kWindowEnd             = kCrossings * kBuildStepsPerCrossing;
    // One past the furthest entry the reader's 100-steps-per-crossing indexing
    // can reach, so its trailing lookups land on the zero tail, not past it.
    static constexpr int kSize                  = 10001;

    WindowedSincTable() noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        values[0] = 1.0f;
        for (int i = 1; i <= kWindowEnd; ++i)
        {
            const double x    = (double) i / (double) kBuildStepsPerCrossing;
            const double sinc = std::sin (pi * x) / (pi * x);
            const double hann = 0.5 * (1.0 + std::cos (pi * x / (double) kCrossings));
            values[(size_t) i] = (float) (sinc * hann);
        }
    }

    float values[kSize] {};
};

inline const float* windowedSincTable() noexcept
{
    static const WindowedSincTable table;
    return table.values;
}
} // namespace detail

class WindowedSincInterpolator
{
public:
    WindowedSincInterpolator() noexcept : table (detail::windowedSincTable()) {}

    // Latency of the interpolation itself, in input samples. Divide by the
    // speed ratio for the latency a resampling pass actually shows.
    static constexpr float getBaseLatency() noexcept { return (float) kCrossings; }

    void reset() noexcept
    {
        for (auto& sample : history)
            sample = 0.0f;
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
    static constexpr int kCrossings            = 100;
    static constexpr int kHistorySize          = kCrossings * 2;
    static constexpr int kReadStepsPerCrossing = 100;

    // Taps run from -kCrossings to +kCrossings around the read position, so the
    // kernel is sampled at whole-sample spacing and only the position of the
    // first tap has to be looked up: every later tap moves a whole crossing
    // through the table, mirroring at the sign change. The tap that lands
    // exactly on the read position needs no special case, because the table
    // opens at exactly 1.0f.
    float valueAtOffset (float offset) const noexcept
    {
        float result           = 0.0f;
        int   samplePosition   = writeIndex;
        float firstFrac        = 0.0f;
        float lastSincPosition = -1.0f;
        int   index            = 0;
        int   sign             = -1;

        for (int i = -kCrossings; i <= kCrossings; ++i)
        {
            const float sincPosition = (1.0f - offset) + (float) i;

            if (i == -kCrossings || (sincPosition >= 0.0f && lastSincPosition < 0.0f))
            {
                const float indexFloat   = std::abs (sincPosition) * (float) kReadStepsPerCrossing;
                const float indexFloored = std::floor (indexFloat);
                index     = (int) indexFloored;
                firstFrac = indexFloat - indexFloored;
                sign      = sincPosition < 0.0f ? -1 : 1;
            }

            if (sincPosition < (float) kCrossings && sincPosition > -(float) kCrossings)
            {
                const float lower = table[index];
                const float upper = table[index + 1];
                result += history[(size_t) samplePosition] * (lower + firstFrac * (upper - lower));
            }

            if (++samplePosition == kHistorySize)
                samplePosition = 0;

            lastSincPosition = sincPosition;
            index += kReadStepsPerCrossing * sign;
        }

        return result;
    }

    const float* table;
    float        history[kHistorySize] {};
    int          writeIndex   = 0;
    double       subSamplePos = 1.0;
};
} // namespace dusk::audio
