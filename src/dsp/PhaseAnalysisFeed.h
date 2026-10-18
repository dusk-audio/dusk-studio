#pragma once

#include "../foundation/SpscIndexFifo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace duskstudio
{
class PhaseAnalysisFeed
{
public:
    struct Chunk
    {
        int samples = 0;
        std::uint64_t sequence = 0;
        std::uint64_t generation = 0;
        double sampleRate = 0.0;
    };

    explicit PhaseAnalysisFeed (int capacityOrder = 16)
        : frames (static_cast<size_t> (1u << std::clamp (capacityOrder, 2, 20))),
          fifo (static_cast<int> (frames.size())) {}

    // Producer only, quiesced against push. Storage and FIFO indices remain
    // untouched so a consumer can finish reading an earlier device generation.
    void prepare (double rate) noexcept
    {
        sampleRate = std::isfinite (rate) && rate > 0.0 ? rate : 44100.0;
        markDiscontinuity();
    }

    void markDiscontinuity() noexcept
    {
        ++generation;
        sequence = 0;
    }

    void push (const float* left, const float* right, int samples) noexcept
    {
        if (samples <= 0 || left == nullptr || right == nullptr || sampleRate == 0.0)
            return;
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (samples, start1, size1, start2, size2);
        const auto first = sequence;
        sequence += static_cast<std::uint64_t> (samples);
        for (int i = 0; i < size1 + size2; ++i)
        {
            const auto slot = static_cast<size_t> (i < size1 ? start1 + i : start2 + i - size1);
            frames[slot] = { left[i], right[i], first + static_cast<std::uint64_t> (i),
                             generation, sampleRate };
        }
        fifo.finishedWrite (size1 + size2);
    }

    // Consumer only. A chunk never crosses a dropped-sample gap, seek or device
    // generation. Metadata shares the samples' release/acquire publication.
    Chunk read (float* left, float* right, int maximum) noexcept
    {
        if (maximum <= 0 || left == nullptr || right == nullptr)
            return {};
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (maximum, start1, size1, start2, size2);
        if (size1 + size2 == 0)
            return {};
        const auto& first = frames[static_cast<size_t> (start1)];
        Chunk chunk { 0, first.sequence, first.generation, first.sampleRate };
        for (int i = 0; i < size1 + size2; ++i)
        {
            const auto slot = static_cast<size_t> (i < size1 ? start1 + i : start2 + i - size1);
            const auto& frame = frames[slot];
            if (frame.generation != chunk.generation
                || frame.sequence != chunk.sequence + static_cast<std::uint64_t> (i))
                break;
            left[i] = frame.left;
            right[i] = frame.right;
            ++chunk.samples;
        }
        fifo.finishedRead (chunk.samples);
        return chunk;
    }

    int pendingSamples() const noexcept { return fifo.getNumReady(); }

    void discardPending() noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (static_cast<int> (frames.size()), start1, size1, start2, size2);
        fifo.finishedRead (size1 + size2);
    }

private:
    struct Frame
    {
        float left = 0.0f, right = 0.0f;
        std::uint64_t sequence = 0, generation = 0;
        double sampleRate = 0.0;
    };

    std::vector<Frame> frames;
    dusk::SpscIndexFifo fifo;
    std::uint64_t sequence = 0, generation = 0;
    double sampleRate = 0.0;
};
} // namespace duskstudio
