#include <catch2/catch_test_macros.hpp>

#include "engine/AudioWorkerPool.h"

#include <array>
#include <cstddef>
#include <cstdint>

// The engine runs a block whose busy strips fall in one lane on the audio
// thread, and the others through the pool, choosing per block from what the
// lanes left in the strips. So one strip's plain state is written by a worker
// in one block, read and written by the caller in the next, and by the worker
// again after that.
// The pool's dispatch and join have to order every one of those accesses; this
// case makes all of them, and ThreadSanitizer reports any it finds unordered.
// It runs real worker threads, as the other pool cases do.

using duskstudio::AudioWorkerPool;

TEST_CASE ("AudioWorkerPool: blocks the caller runs itself between dispatches share the lanes' state in order",
           "[worker-pool]")
{
    static constexpr int kWorkers = 3, kLanes = kWorkers + 1, kStrips = 24, kBlocks = 600;
    std::array<std::uint64_t, kStrips> state {};
    const auto runLane = [&state] (int lane)
    {
        for (int s = kStrips * lane / kLanes; s < kStrips * (lane + 1) / kLanes; ++s)
            state[(std::size_t) s] = state[(std::size_t) s] * 3 + (std::uint64_t) s + 1;
    };

    AudioWorkerPool pool;
    pool.start (kWorkers, runLane);
    REQUIRE (pool.laneCount() == kLanes);

    std::array<std::uint64_t, kStrips> expected {};
    int dispatched = 0;
    for (int b = 0; b < kBlocks; ++b)
    {
        // Decided from the state the last block left, as the engine decides.
        std::uint64_t mix = 0;
        for (const auto value : state) mix ^= value;
        if (((mix >> 3) ^ (std::uint64_t) b) % 3 != 0)
        {
            pool.runBlock();
            ++dispatched;
        }
        else
        {
            for (int lane = 0; lane < kLanes; ++lane) runLane (lane);
        }
        for (int s = 0; s < kStrips; ++s)
            expected[(std::size_t) s] = expected[(std::size_t) s] * 3 + (std::uint64_t) s + 1;
    }
    pool.stop();

    REQUIRE (state == expected);
    REQUIRE (dispatched > 0);
    REQUIRE (dispatched < kBlocks);
}
