#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace duskstudio
{
class PlaybackReadCapture
{
public:
    static constexpr int kTracks = 24;
    static constexpr int kRegionsPerTrack = 15;
    static constexpr int kRegionSeconds = 8;
    static constexpr std::size_t kMaxStorageBytes = 400 * 1024 * 1024;

    struct Statistics
    {
        std::uint64_t callbacks = 0, completeCallbacks = 0, missingTrackCallbacks = 0;
        std::uint64_t duplicateReads = 0, invalidReads = 0, protocolErrors = 0;
        bool overflow = false;
    };

    struct Expectations
    {
        int sampleRate = 0;
        std::int64_t loopStart = -1, loopEnd = -1;
        std::vector<std::int64_t> rollingSeeks;
        bool requireLoopWrap = false;
    };

    struct Result
    {
        Statistics capture;
        std::uint64_t checkedValues = 0, mismatchedValues = 0, nonFiniteValues = 0;
        std::uint64_t timelineErrors = 0, loopWraps = 0, committedSeeks = 0;
        double maximumError = 0.0;
        int firstWrongTrack = -1, firstWrongChannel = -1;
        std::int64_t firstWrongFrame = -1;

        bool passed() const noexcept
        {
            return capture.callbacks > 0 && capture.completeCallbacks == capture.callbacks
                && ! capture.overflow && capture.missingTrackCallbacks == 0
                && capture.duplicateReads == 0 && capture.invalidReads == 0 && capture.protocolErrors == 0
                && checkedValues > 0 && mismatchedValues == 0 && timelineErrors == 0;
        }
    };

    // Prepare and analyze only while the owning device callback is detached and drained.
    // The callback is the sole writer of sample storage, records and statistics.
    bool prepare (std::size_t framesPerTrack, std::size_t callbacks, std::uint32_t trackMask)
    {
        constexpr auto validMask = (std::uint32_t { 1 } << kTracks) - 1;
        if (framesPerTrack == 0 || callbacks == 0 || trackMask == 0 || (trackMask & ~validMask) != 0)
            return false;
        std::size_t tracks = 0;
        for (auto bits = trackMask; bits != 0; bits &= bits - 1) ++tracks;
        if (framesPerTrack > kMaxStorageBytes / (2 * sizeof (float) * tracks)
            || callbacks > kMaxStorageBytes / (sizeof (Block) + tracks * sizeof (Read)))
            return false;
        const auto sampleCount = framesPerTrack * tracks * 2;
        const auto readCount = callbacks * tracks;
        const auto bytes = sampleCount * sizeof (float) + callbacks * sizeof (Block) + readCount * sizeof (Read);
        if (bytes > kMaxStorageBytes) return false;
        std::vector<float>().swap (samples);
        std::vector<Block>().swap (blocks);
        std::vector<Read>().swap (reads);
        samples.assign (sampleCount, 0.0f);
        blocks.assign (callbacks, {});
        reads.assign (readCount, {});
        expectedMask = trackMask;
        blockCount = readCountUsed = samplesUsed = 0;
        statistics = {};
        active = false;
        capturing.store (false, std::memory_order_release);
        return true;
    }

    void setCapturing (bool enabled) noexcept { capturing.store (enabled, std::memory_order_release); }

    void beginCallback (int numSamples) noexcept
    {
        if (active) ++statistics.protocolErrors;
        active = false;
        if (numSamples <= 0 || ! capturing.load (std::memory_order_acquire)) return;
        ++statistics.callbacks;
        if (blockCount >= blocks.size()) { statistics.overflow = true; return; }
        auto& block = blocks[blockCount++];
        block = {};
        block.numSamples = numSamples;
        block.firstRead = readCountUsed;
        active = true;
    }

    void read (int track, std::int64_t start, std::int64_t loopStart, std::int64_t loopEnd,
               const float* left, const float* right, int numSamples) noexcept
    {
        if (! active) return;
        auto& block = blocks[blockCount - 1];
        if (track < 0 || track >= kTracks || (expectedMask & (std::uint32_t { 1 } << track)) == 0)
        {
            ++statistics.invalidReads;
            return;
        }
        const auto bit = std::uint32_t { 1 } << track;
        if ((block.seenMask & bit) != 0) { ++statistics.duplicateReads; return; }
        block.seenMask |= bit;
        if (left == nullptr || right == nullptr || numSamples != block.numSamples)
        {
            ++statistics.invalidReads;
            return;
        }
        if (block.readCount == 0)
        {
            block.start = start;
            block.loopStart = loopStart;
            block.loopEnd = loopEnd;
        }
        else if (block.start != start || block.loopStart != loopStart || block.loopEnd != loopEnd)
        {
            ++statistics.invalidReads;
            return;
        }
        const auto count = static_cast<std::size_t> (numSamples);
        if (readCountUsed >= reads.size() || count > (samples.size() - samplesUsed) / 2)
        {
            statistics.overflow = true;
            return;
        }
        reads[readCountUsed++] = { samplesUsed, track };
        std::memcpy (samples.data() + samplesUsed, left, count * sizeof (float));
        std::memcpy (samples.data() + samplesUsed + count, right, count * sizeof (float));
        samplesUsed += count * 2;
        ++block.readCount;
    }

    void endCallback() noexcept
    {
        if (! active) return;
        if (blocks[blockCount - 1].seenMask == expectedMask) ++statistics.completeCallbacks;
        else ++statistics.missingTrackCallbacks;
        active = false;
    }

    std::size_t allocatedBytes() const noexcept
    {
        return samples.capacity() * sizeof (float) + blocks.capacity() * sizeof (Block) + reads.capacity() * sizeof (Read);
    }

    static std::int16_t fixturePcm16 (int track, int region, int channel, std::int64_t frame) noexcept
    {
        auto bits = static_cast<std::uint64_t> (frame)
            + static_cast<std::uint64_t> (track * kRegionsPerTrack + region) * std::uint64_t { 0x9e3779b97f4a7c15ULL }
            + static_cast<std::uint64_t> (channel + 1) * std::uint64_t { 0xd1b54a32d192ed03ULL };
        bits = (bits ^ (bits >> 30)) * std::uint64_t { 0xbf58476d1ce4e5b9ULL };
        bits = (bits ^ (bits >> 27)) * std::uint64_t { 0x94d049bb133111ebULL };
        bits ^= bits >> 31;
        return static_cast<std::int16_t> (static_cast<int> (bits >> 48) - 32768);
    }

    Result analyze (const Expectations& expected) const
    {
        Result result;
        result.capture = statistics;
        if (active || capturing.load (std::memory_order_acquire)) ++result.capture.protocolErrors;
        if (expected.sampleRate <= 0 || expected.sampleRate > 192000
            || ! validLoop (expected.loopStart, expected.loopEnd))
        {
            ++result.timelineErrors;
            return result;
        }
        const auto regionFrames = std::int64_t { kRegionSeconds } * expected.sampleRate;
        const auto sessionFrames = regionFrames * kRegionsPerTrack;
        std::int64_t nextStart = -1;
        std::size_t nextSeek = 0;
        for (std::size_t index = 0; index < blockCount; ++index)
        {
            const auto& block = blocks[index];
            if (block.readCount == 0) continue;
            if (block.start < 0 || block.start >= sessionFrames
                || block.loopStart != expected.loopStart || block.loopEnd != expected.loopEnd
                || (block.loopEnd >= 0 && block.loopEnd > sessionFrames))
            {
                ++result.timelineErrors;
                continue;
            }
            if (nextStart >= 0 && block.start != nextStart)
            {
                if (nextSeek < expected.rollingSeeks.size() && block.start == expected.rollingSeeks[nextSeek])
                {
                    ++nextSeek;
                    ++result.committedSeeks;
                }
                else ++result.timelineErrors;
            }
            const auto end = block.start + block.numSamples;
            const bool looping = expected.loopStart >= 0 && block.start < expected.loopEnd;
            const auto loopLength = looping ? expected.loopEnd - expected.loopStart : 0;
            nextStart = end;
            if (looping && end >= expected.loopEnd)
            {
                result.loopWraps += static_cast<std::uint64_t> (1 + (end - expected.loopEnd) / loopLength);
                nextStart = expected.loopStart + (end - expected.loopEnd) % loopLength;
            }
            for (std::size_t readIndex = block.firstRead; readIndex < block.firstRead + block.readCount; ++readIndex)
            {
                const auto& captured = reads[readIndex];
                for (int sample = 0; sample < block.numSamples; ++sample)
                {
                    auto frame = block.start + sample;
                    if (looping && frame >= expected.loopEnd)
                        frame = expected.loopStart + (frame - expected.loopEnd) % loopLength;
                    if (frame >= sessionFrames) { ++result.timelineErrors; continue; }
                    double gain = 1.0;
                    if (looping)
                        for (auto seam = expected.loopEnd - block.start; seam < block.numSamples; seam += loopLength)
                        {
                            const auto distance = static_cast<std::int64_t> (sample) - seam;
                            if (distance >= -64 && distance < 64)
                                gain *= 0.5 * (1.0 - std::cos (3.14159265358979323846 * std::abs (distance) / 64.0));
                        }
                    for (int channel = 0; channel < 2; ++channel)
                    {
                        const auto wanted = static_cast<double> (fixturePcm16 (captured.track,
                            static_cast<int> (frame / regionFrames), channel, frame % regionFrames)) / 32768.0 * gain;
                        const auto actual = samples[captured.offset + static_cast<std::size_t> (channel * block.numSamples + sample)];
                        const bool finite = std::isfinite (actual);
                        const double error = finite ? std::abs (actual - wanted) : std::numeric_limits<double>::infinity();
                        ++result.checkedValues;
                        if (! finite) ++result.nonFiniteValues;
                        result.maximumError = std::max (result.maximumError, error);
                        if (error > 3.0e-7)
                        {
                            ++result.mismatchedValues;
                            if (result.firstWrongTrack < 0)
                            {
                                result.firstWrongTrack = captured.track;
                                result.firstWrongChannel = channel;
                                result.firstWrongFrame = frame;
                            }
                        }
                    }
                }
            }
        }
        if (nextSeek != expected.rollingSeeks.size() || (expected.requireLoopWrap && result.loopWraps == 0))
            ++result.timelineErrors;
        return result;
    }

private:
    struct Block
    {
        std::int64_t start = 0, loopStart = -1, loopEnd = -1;
        std::size_t firstRead = 0, readCount = 0;
        std::uint32_t seenMask = 0;
        int numSamples = 0;
    };
    struct Read { std::size_t offset = 0; int track = 0; };

    static bool validLoop (std::int64_t start, std::int64_t end) noexcept
    {
        return (start == -1 && end == -1) || (start >= 0 && end > start && end - start >= 128);
    }

    std::vector<float> samples;
    std::vector<Block> blocks;
    std::vector<Read> reads;
    Statistics statistics;
    std::atomic<bool> capturing { false };
    std::uint32_t expectedMask = 0;
    std::size_t blockCount = 0, readCountUsed = 0, samplesUsed = 0;
    bool active = false;
};
}
