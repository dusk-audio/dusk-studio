#pragma once

#include "TransportPosition.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

namespace dusk
{
// One publisher, with any number of readers. A lifecycle publisher must first
// park the audio publisher. Atomic payload words keep an overtaken read legal;
// their release/acquire pairs make its final stamp check reject mixed blocks.
class AtomicTransportPosition
{
public:
    void publish (const TransportPosition* position) noexcept
    {
        const auto previous = stamp.load (std::memory_order_relaxed);
        stamp.store (previous + 1, std::memory_order_relaxed);
        if (position != nullptr)
        {
            store (0, position->bpm);
            store (1, position->ppqPosition);
            store (2, position->timeInSeconds);
            store (3, position->timeInSamples);
            store (4, position->loopStartSamples);
            store (5, position->loopEndSamples);
            store (6, position->ppqLoopStart);
            store (7, position->ppqLoopEnd);
            store (8, position->timeSignatureNumerator);
            store (9, position->timeSignatureDenominator);
            words[10].store (kPresent | (position->isPlaying ? kPlaying : 0u)
                                       | (position->isRecording ? kRecording : 0u)
                                       | (position->isLooping ? kLooping : 0u), std::memory_order_release);
        }
        else
            for (auto& word : words) word.store (0, std::memory_order_release);
        stamp.store (previous + 2, std::memory_order_release);
    }

    std::optional<TransportPosition> read() const noexcept
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const auto before = stamp.load (std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            std::array<std::uint64_t, 11> captured {};
            for (std::size_t index = 0; index < captured.size(); ++index)
                captured[index] = words[index].load (std::memory_order_acquire);
            if (stamp.load (std::memory_order_relaxed) != before) continue;
            if ((captured[10] & kPresent) == 0) return std::nullopt;
            TransportPosition position;
            position.bpm = value<double> (captured[0]);
            position.ppqPosition = value<double> (captured[1]);
            position.timeInSeconds = value<double> (captured[2]);
            position.timeInSamples = value<std::int64_t> (captured[3]);
            position.loopStartSamples = value<std::int64_t> (captured[4]);
            position.loopEndSamples = value<std::int64_t> (captured[5]);
            position.ppqLoopStart = value<double> (captured[6]);
            position.ppqLoopEnd = value<double> (captured[7]);
            position.timeSignatureNumerator = value<int> (captured[8]);
            position.timeSignatureDenominator = value<int> (captured[9]);
            position.isPlaying = (captured[10] & kPlaying) != 0;
            position.isRecording = (captured[10] & kRecording) != 0;
            position.isLooping = (captured[10] & kLooping) != 0;
            return position;
        }
        return std::nullopt;
    }

private:
    template <typename T>
    void store (std::size_t index, T valueToStore) noexcept
    {
        static_assert (sizeof (T) <= sizeof (std::uint64_t), "transport value exceeds its atomic word");
        std::uint64_t bits = 0;
        std::memcpy (&bits, &valueToStore, sizeof (T));
        words[index].store (bits, std::memory_order_release);
    }

    template <typename T>
    static T value (std::uint64_t bits) noexcept
    {
        T result {};
        std::memcpy (&result, &bits, sizeof (T));
        return result;
    }

    static_assert (std::atomic<std::uint64_t>::is_always_lock_free,
                   "transport publication must not lock on the audio thread");
    static constexpr std::uint64_t kPlaying = 1, kRecording = 2, kLooping = 4, kPresent = 8;
    std::array<std::atomic<std::uint64_t>, 11> words {};
    std::atomic<std::uint64_t> stamp { 0 };
};
} // namespace dusk
