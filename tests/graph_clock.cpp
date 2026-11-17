#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/device/GraphClock.h"

#include <atomic>
#include <thread>
#include <vector>

// The real-time half of following a PipeWire graph whose quantum or rate moves
// under an open stream: a longer cycle is handed over in blocks the callback
// was prepared for, and a clock that settles somewhere new is published once.

using duskstudio::device::ClockWatch;
using duskstudio::device::forEachSubBlock;
using Catch::Matchers::WithinAbs;

namespace
{
// A callback prepared for `prepared` samples that renders a running sample
// counter and copies its input through, the way the engine's transport and
// capture advance block by block.
struct CountingCallback
{
    int prepared = 0;
    std::vector<int> blocks;
    float position = 0.0f;
    std::vector<float> captured;
    bool oversized = false;

    void operator() (const float* const* in, float* const* out, int numSamples)
    {
        blocks.push_back (numSamples);
        if (numSamples > prepared) oversized = true;
        for (int i = 0; i < numSamples; ++i)
        {
            out[0][i] = position;
            out[1][i] = -position;
            position += 1.0f;
            captured.push_back (in[0][i]);
        }
    }
};
} // namespace

TEST_CASE ("A cycle longer than the prepared block reaches the callback in blocks it can take",
           "[audio][device][pipewire]")
{
    static constexpr int kPrepared = 256;

    SECTION ("a whole multiple of the block")
    {
        static constexpr int kCycle = 1024;
        std::vector<float> input (kCycle), left (kCycle, -1.0f), right (kCycle, -1.0f);
        for (int i = 0; i < kCycle; ++i) input[(std::size_t) i] = (float) (1000 + i);
        const float* in[] = { input.data() };
        float* out[] = { left.data(), right.data() };

        CountingCallback cb;
        cb.prepared = kPrepared;
        forEachSubBlock (in, 1, out, 2, kCycle, kPrepared,
                         [&cb] (const float* const* i, float* const* o, int n) { cb (i, o, n); });

        REQUIRE (cb.blocks == std::vector<int> { 256, 256, 256, 256 });
        REQUIRE_FALSE (cb.oversized);
        for (int i = 0; i < kCycle; ++i)
        {
            REQUIRE_THAT (left[(std::size_t) i], WithinAbs ((float) i, 0.0));
            REQUIRE_THAT (right[(std::size_t) i], WithinAbs ((float) -i, 0.0));
            REQUIRE_THAT (cb.captured[(std::size_t) i], WithinAbs (input[(std::size_t) i], 0.0));
        }
    }

    SECTION ("a remainder shorter than the block")
    {
        static constexpr int kCycle = 1000;
        std::vector<float> input (kCycle, 0.5f), left (kCycle), right (kCycle);
        const float* in[] = { input.data() };
        float* out[] = { left.data(), right.data() };

        CountingCallback cb;
        cb.prepared = kPrepared;
        forEachSubBlock (in, 1, out, 2, kCycle, kPrepared,
                         [&cb] (const float* const* i, float* const* o, int n) { cb (i, o, n); });

        REQUIRE (cb.blocks == std::vector<int> { 256, 256, 256, 232 });
        REQUIRE_THAT (left.back(), WithinAbs (999.0f, 0.0));
        REQUIRE (cb.captured.size() == (std::size_t) kCycle);
    }

    SECTION ("a cycle within the block passes whole")
    {
        static constexpr int kCycle = 128;
        std::vector<float> input (kCycle), left (kCycle), right (kCycle);
        const float* in[] = { input.data() };
        float* out[] = { left.data(), right.data() };

        CountingCallback cb;
        cb.prepared = kPrepared;
        forEachSubBlock (in, 1, out, 2, kCycle, kPrepared,
                         [&cb] (const float* const* i, float* const* o, int n) { cb (i, o, n); });
        REQUIRE (cb.blocks == std::vector<int> { 128 });
    }

    SECTION ("a channel with no buffer stays without one")
    {
        static constexpr int kCycle = 512;
        std::vector<float> left (kCycle);
        const float* in[] = { nullptr };
        float* out[] = { left.data(), nullptr };
        std::vector<const float*> seenIn;
        std::vector<float*> seenOut;
        forEachSubBlock (in, 1, out, 2, kCycle, kPrepared,
                         [&] (const float* const* i, float* const* o, int)
                         {
                             seenIn.push_back (i[0]);
                             seenOut.push_back (o[1]);
                         });
        REQUIRE (seenIn == std::vector<const float*> { nullptr, nullptr });
        REQUIRE (seenOut == std::vector<float*> { nullptr, nullptr });
    }
}

TEST_CASE ("A graph clock that settles somewhere new is published once it has lasted",
           "[audio][device][pipewire]")
{
    ClockWatch watch;
    watch.prepare (256, 48000);

    SECTION ("the prepared clock is never a change")
    {
        for (int i = 0; i < 20; ++i) watch.observe (256, 48000);
        REQUIRE_FALSE (watch.movedTo().isSet());
        REQUIRE (watch.latest().bufferSize == 256);
        REQUIRE_THAT (watch.latest().sampleRate, WithinAbs (48000.0, 1e-9));
        REQUIRE (watch.preparedBufferSize() == 256);
    }

    SECTION ("a longer quantum counts after it has held")
    {
        for (int i = 1; i < ClockWatch::kLastingCycles; ++i)
        {
            watch.observe (1024, 48000);
            REQUIRE_FALSE (watch.movedTo().isSet());
        }
        watch.observe (1024, 48000);
        const auto moved = watch.movedTo();
        REQUIRE (moved.bufferSize == 1024);
        REQUIRE_THAT (moved.sampleRate, WithinAbs (48000.0, 1e-9));
        REQUIRE (watch.preparedBufferSize() == 256);
    }

    SECTION ("a new rate counts the same way")
    {
        for (int i = 0; i < ClockWatch::kLastingCycles; ++i) watch.observe (256, 44100);
        REQUIRE_THAT (watch.movedTo().sampleRate, WithinAbs (44100.0, 1e-9));
        REQUIRE (watch.movedTo().bufferSize == 256);
    }

    SECTION ("one odd cycle is not followed")
    {
        watch.observe (2048, 48000);
        for (int i = 0; i < 20; ++i) watch.observe (256, 48000);
        REQUIRE_FALSE (watch.movedTo().isSet());
    }

    SECTION ("a clock that goes back before it lasted is forgotten")
    {
        for (int i = 1; i < ClockWatch::kLastingCycles; ++i) watch.observe (1024, 48000);
        watch.observe (256, 48000);
        watch.observe (1024, 48000);
        REQUIRE_FALSE (watch.movedTo().isSet());
    }

    SECTION ("a clock that goes back after it lasted withdraws the change")
    {
        for (int i = 0; i < ClockWatch::kLastingCycles; ++i) watch.observe (1024, 48000);
        REQUIRE (watch.movedTo().isSet());
        watch.observe (256, 48000);
        REQUIRE_FALSE (watch.movedTo().isSet());
    }

    SECTION ("a second move replaces the first")
    {
        for (int i = 0; i < ClockWatch::kLastingCycles; ++i) watch.observe (1024, 48000);
        for (int i = 0; i < ClockWatch::kLastingCycles; ++i) watch.observe (512, 96000);
        REQUIRE (watch.movedTo().bufferSize == 512);
        REQUIRE_THAT (watch.movedTo().sampleRate, WithinAbs (96000.0, 1e-9));
    }

    SECTION ("preparing at the moved clock settles it")
    {
        for (int i = 0; i < ClockWatch::kLastingCycles; ++i) watch.observe (1024, 44100);
        watch.prepare (1024, 44100);
        REQUIRE_FALSE (watch.movedTo().isSet());
        REQUIRE (watch.preparedBufferSize() == 1024);
        watch.observe (1024, 44100);
        REQUIRE_FALSE (watch.movedTo().isSet());
    }

    SECTION ("a cycle with no rate is ignored")
    {
        for (int i = 0; i < 20; ++i) watch.observe (1024, 0);
        REQUIRE_FALSE (watch.movedTo().isSet());
        REQUIRE (watch.latest().bufferSize == 0);
    }

    SECTION ("reset forgets the stream")
    {
        for (int i = 0; i < ClockWatch::kLastingCycles; ++i) watch.observe (1024, 48000);
        watch.reset();
        REQUIRE_FALSE (watch.movedTo().isSet());
        REQUIRE_FALSE (watch.latest().isSet());
        REQUIRE (watch.preparedBufferSize() == 0);
    }
}

TEST_CASE ("A graph clock moved on the device thread is followed from the message thread",
           "[audio][device][pipewire][ipc]")
{
    // The device thread observes every cycle while the message thread polls
    // for a moved clock and prepares at it, as start() does after a stop. Run
    // under ThreadSanitizer this holds the two sides to their one writer each.
    ClockWatch watch;
    watch.prepare (256, 48000);
    std::atomic<bool> stop { false };

    std::thread device ([&watch, &stop]
    {
        for (long cycle = 0; ! stop.load (std::memory_order_acquire); ++cycle)
        {
            if (cycle < 2000)      watch.observe (256, 48000);
            else if (cycle < 4000) watch.observe (1024, 48000);
            else                   watch.observe (1024, 44100);
            (void) watch.preparedBufferSize();
            std::this_thread::yield();
        }
    });

    // Rates are whole numbers of hertz, so they compare as integers here.
    int preparedBlock = 256, preparedRate = 48000, follows = 0;
    bool movedToPrepared = false;
    static constexpr long kMaxSpins = 50000000;
    for (long spins = 0; spins < kMaxSpins; ++spins)
    {
        if (const auto moved = watch.movedTo(); moved.isSet())
        {
            const int movedRate = (int) moved.sampleRate;
            movedToPrepared = movedToPrepared || (moved.bufferSize == preparedBlock && movedRate == preparedRate);
            watch.prepare (moved.bufferSize, movedRate);
            preparedBlock = moved.bufferSize;
            preparedRate = movedRate;
            ++follows;
        }
        if (preparedBlock == 1024 && preparedRate == 44100) break;
        std::this_thread::yield();
    }
    stop.store (true, std::memory_order_release);
    device.join();

    REQUIRE_FALSE (movedToPrepared);
    REQUIRE (preparedBlock == 1024);
    REQUIRE (preparedRate == 44100);
    REQUIRE (follows >= 1);
    REQUIRE (watch.preparedBufferSize() == 1024);
    REQUIRE_FALSE (watch.movedTo().isSet());
}
