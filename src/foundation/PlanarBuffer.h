#pragma once

#include "VectorOps.h"

#include <algorithm>
#include <vector>

// Owning planar float buffer: one allocation holding every channel, each
// channel padded to a whole 64-byte span so the channels sit at a uniform
// stride and a vector loop that rounds its tail up cannot reach the next
// channel's samples. setSize is the only operation that allocates - call it
// from prepare (or any message-thread setup), never from the audio thread.
//
// Channel pointers alias the storage, so copying one of these would leave the
// copy pointing into the original; the buffer is neither copyable nor movable.
namespace dusk::audio
{
class PlanarBuffer
{
public:
    PlanarBuffer() = default;
    PlanarBuffer (const PlanarBuffer&) = delete;
    PlanarBuffer& operator= (const PlanarBuffer&) = delete;

    void setSize (int newNumChannels, int newNumSamples)
    {
        channelCount = std::max (0, newNumChannels);
        sampleCount  = std::max (0, newNumSamples);
        stride       = ((sampleCount + kSamplesPerLine - 1) / kSamplesPerLine) * kSamplesPerLine;

        storage.assign ((std::size_t) channelCount * (std::size_t) stride, 0.0f);
        pointers.resize ((std::size_t) channelCount);
        for (int c = 0; c < channelCount; ++c)
            pointers[(std::size_t) c] = storage.data() + (std::size_t) c * (std::size_t) stride;
    }

    int numChannels() const noexcept { return channelCount; }
    int numSamples()  const noexcept { return sampleCount; }

    float*       channel (int c)       noexcept { return pointers[(std::size_t) c]; }
    const float* channel (int c) const noexcept { return pointers[(std::size_t) c]; }

    // Channel-pointer array, for readers, writers, and the JUCE buffers that
    // still wrap Dusk storage on the way into donor DSP.
    float* const*       data()      noexcept { return pointers.data(); }
    const float* const* data() const noexcept { return pointers.data(); }

    void clear() noexcept
    {
        for (int c = 0; c < channelCount; ++c)
            vecClear (pointers[(std::size_t) c], sampleCount);
    }

private:
    static constexpr int kSamplesPerLine = 16;   // 64 bytes

    std::vector<float>  storage;
    std::vector<float*> pointers;
    int channelCount = 0;
    int sampleCount  = 0;
    int stride       = 0;
};
} // namespace dusk::audio
