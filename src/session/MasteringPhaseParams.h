#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace duskstudio
{
struct MasteringPhaseSettings
{
    static constexpr int kMinBands = 4, kMaxBands = 64;
    static constexpr float kMinQ = 0.25f, kMaxQ = 8.0f;
    static constexpr float kMinAveragingMs = 50.0f, kMaxAveragingMs = 5000.0f;

    int bands = 16;
    float q = 2.0f;
    float averagingMs = 500.0f;

    MasteringPhaseSettings sanitized() const noexcept
    {
        return { std::clamp (bands, kMinBands, kMaxBands),
                 std::isfinite (q) ? std::clamp (q, kMinQ, kMaxQ) : 2.0f,
                 std::isfinite (averagingMs)
                     ? std::clamp (averagingMs, kMinAveragingMs, kMaxAveragingMs) : 500.0f };
    }
};

struct MasteringPhaseParams
{
    std::atomic<int> bands { 16 };
    std::atomic<float> q { 2.0f };
    std::atomic<float> averagingMs { 500.0f };

    MasteringPhaseSettings snapshot() const noexcept
    {
        return MasteringPhaseSettings { bands.load (std::memory_order_relaxed),
                                        q.load (std::memory_order_relaxed),
                                        averagingMs.load (std::memory_order_relaxed) }.sanitized();
    }
};
} // namespace duskstudio
