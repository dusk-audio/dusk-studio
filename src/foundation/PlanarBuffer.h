#pragma once

#include "VectorOps.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
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

    // setSize refuses more than this many floats of padded storage (4 GiB), so
    // a length derived from session data - a join spanning hours, a corrupt
    // region length - comes back false instead of throwing out of an undo
    // action. An allocation the machine refuses under that ceiling comes back
    // false the same way.
    static constexpr std::int64_t kMaxTotalSamples = 1LL << 30;

    static constexpr std::int64_t strideFor (int samples) noexcept
    {
        const std::int64_t n = samples > 0 ? (std::int64_t) samples : 0;
        return ((n + kSamplesPerLine - 1) / kSamplesPerLine) * kSamplesPerLine;
    }

    static constexpr std::int64_t storageFor (int channels, int samples) noexcept
    {
        return (channels > 0 ? (std::int64_t) channels : 0) * strideFor (samples);
    }

    bool setSize (int newNumChannels, int newNumSamples)
    {
        const int newChannels = std::max (0, newNumChannels);
        const int newSamples  = std::max (0, newNumSamples);
        const std::int64_t total = storageFor (newChannels, newSamples);
        if (total > kMaxTotalSamples) return refuse();

        try
        {
            storage.assign ((std::size_t) total, 0.0f);
            pointers.resize ((std::size_t) newChannels);
        }
        catch (const std::bad_alloc&)    { return refuse(); }
        catch (const std::length_error&) { return refuse(); }

        channelCount = newChannels;
        sampleCount  = newSamples;
        const std::int64_t stride = strideFor (sampleCount);
        for (int c = 0; c < channelCount; ++c)
            pointers[(std::size_t) c] = storage.data() + (std::size_t) c * (std::size_t) stride;
        return true;
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
    static constexpr std::int64_t kSamplesPerLine = 16;   // 64 bytes

    bool refuse() noexcept
    {
        channelCount = 0;
        sampleCount  = 0;
        storage.clear();
        pointers.clear();
        return false;
    }

    std::vector<float>  storage;
    std::vector<float*> pointers;
    int channelCount = 0;
    int sampleCount  = 0;
};
} // namespace dusk::audio
