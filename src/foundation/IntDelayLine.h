#pragma once

#include <algorithm>
#include <vector>

// Single-channel integer delay line, bit-for-bit equivalent to JUCE
// dsp::DelayLine<float, DelayLineInterpolationTypes::None>: setDelay truncates
// to whole samples (floor), push writes at the write head and decrements it
// mod totalSize, pop reads at (readPos + delayInt) mod totalSize and decrements
// readPos the same way. totalSize = max(4, maxDelay + 2) matches JUCE so the
// index arithmetic - and therefore the delayed output - is identical. JUCE's
// DelayLine is per-instance-multichannel; Dusk only ever uses one channel per
// instance, so this drops the channel argument.
namespace dusk::audio
{
class IntDelayLine
{
public:
    void setMaximumDelayInSamples (int maxDelay) noexcept
    {
        totalSize = std::max (4, maxDelay + 2);
        buffer.assign ((size_t) totalSize, 0.0f);
        reset();
    }

    void setDelay (int newDelayInSamples) noexcept
    {
        delayInt = std::clamp (newDelayInSamples, 0, totalSize - 2);
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
        readPos  = 0;
    }

    void pushSample (float sample) noexcept
    {
        buffer[(size_t) writePos] = sample;
        writePos = (writePos + totalSize - 1) % totalSize;
    }

    float popSample() noexcept
    {
        const float result = buffer[(size_t) ((readPos + delayInt) % totalSize)];
        readPos = (readPos + totalSize - 1) % totalSize;
        return result;
    }

private:
    std::vector<float> buffer;
    int totalSize = 4;
    int writePos  = 0;
    int readPos   = 0;
    int delayInt  = 0;
};
} // namespace dusk::audio
