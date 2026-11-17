#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

// The real-time side of following a device clock that can move under an open
// stream, as a PipeWire graph does when something forces another quantum or
// rate on it. Allocation- and lock-free; the PipeWire backend runs it on its
// data thread, and tests drive it with no graph.
namespace duskstudio::device
{
// A block size and sample rate a device clock runs at. Zero means none.
struct DeviceClock
{
    int    bufferSize = 0;
    double sampleRate = 0.0;

    bool isSet() const noexcept { return bufferSize > 0 && sampleRate > 0.0; }
};

// Hands a device cycle to fn in consecutive blocks of at most maxBlock samples,
// as fn (in, out, numSamples), with every channel pointer moved to the block's
// first sample. A callback prepared for maxBlock samples then sees only blocks
// it can take, back to back, so its own position and capture stay continuous.
// in and out are the cycle's channel pointer arrays and are advanced in place.
template <class Fn>
void forEachSubBlock (const float** in, int numIn, float** out, int numOut,
                      int numSamples, int maxBlock, Fn&& fn) noexcept
{
    const int block = maxBlock > 0 ? maxBlock : numSamples;
    for (int done = 0; done < numSamples;)
    {
        const int len = std::min (block, numSamples - done);
        fn (in, out, len);
        for (int ch = 0; ch < numIn; ++ch)
            if (in[ch] != nullptr) in[ch] += len;
        for (int ch = 0; ch < numOut; ++ch)
            if (out[ch] != nullptr) out[ch] += len;
        done += len;
    }
}

// Compares the clock of every device cycle with the one the stream was started
// at, and publishes a change once it has lasted. Each atomic has one writer:
// the message thread writes the prepared clock, the device thread the others.
class ClockWatch
{
public:
    // Cycles a new clock has to hold before it counts as moved, so one odd
    // cycle is never followed.
    static constexpr int kLastingCycles = 4;

    // Only while no device thread runs: forgets everything seen.
    void reset() noexcept
    {
        prepared.store (0, std::memory_order_relaxed);
        moved.store (0, std::memory_order_relaxed);
        latestClock.store (0, std::memory_order_relaxed);
        candidate = published = 0;
        heldCycles = 0;
    }

    // Message thread, before the callback is (re)started at this clock.
    void prepare (int bufferSize, int sampleRate) noexcept
    {
        prepared.store (pack (bufferSize, sampleRate), std::memory_order_release);
    }

    // The block size the running callback was prepared for, 0 before prepare.
    int preparedBufferSize() const noexcept
    {
        return (int) (std::uint32_t) prepared.load (std::memory_order_acquire);
    }

    // Device thread, once per cycle.
    void observe (int bufferSize, int sampleRate) noexcept
    {
        const auto now = pack (bufferSize, sampleRate);
        if (now == 0) return;
        latestClock.store (now, std::memory_order_release);

        if (now == prepared.load (std::memory_order_acquire))
        {
            candidate = 0;
            heldCycles = 0;
            if (published != 0)
            {
                published = 0;
                moved.store (0, std::memory_order_release);
            }
            return;
        }
        if (now != candidate)
        {
            candidate = now;
            heldCycles = 0;
        }
        if (heldCycles < kLastingCycles) ++heldCycles;
        if (heldCycles >= kLastingCycles && published != now)
        {
            published = now;
            moved.store (now, std::memory_order_release);
        }
    }

    // The clock of the newest cycle, unset before the first one.
    DeviceClock latest() const noexcept { return unpack (latestClock.load (std::memory_order_acquire)); }

    // The clock the device has settled on when it is not the prepared one.
    DeviceClock movedTo() const noexcept
    {
        const auto m = moved.load (std::memory_order_acquire);
        if (m == 0 || m == prepared.load (std::memory_order_acquire)) return {};
        return unpack (m);
    }

private:
    static std::uint64_t pack (int bufferSize, int sampleRate) noexcept
    {
        if (bufferSize <= 0 || sampleRate <= 0) return 0;
        return ((std::uint64_t) (std::uint32_t) sampleRate << 32) | (std::uint32_t) bufferSize;
    }

    static DeviceClock unpack (std::uint64_t v) noexcept
    {
        if (v == 0) return {};
        return { (int) (std::uint32_t) v, (double) (std::uint32_t) (v >> 32) };
    }

    static_assert (std::atomic<std::uint64_t>::is_always_lock_free);

    std::atomic<std::uint64_t> prepared { 0 }, moved { 0 }, latestClock { 0 };
    std::uint64_t candidate = 0, published = 0;   // device thread only
    int heldCycles = 0;                           // device thread only
};
} // namespace duskstudio::device
