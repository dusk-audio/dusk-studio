#include <catch2/catch_test_macros.hpp>

#include "engine/AudioWorkerPool.h"
#include "support/AllocationObserver.h"
#include "support/ThreadWakeups.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

// A block dispatches only as many worker lanes as it has work for, and the
// rest stay asleep. These cases run real worker threads, as the other pool
// cases do. The lanes write plain fields: a lane run twice, run when its block
// did not dispatch it, or still running when the join returned is a data race
// ThreadSanitizer reports, besides failing the counts.

using duskstudio::AudioWorkerPool;
using duskstudio::test::eventually;

TEST_CASE ("AudioWorkerPool: a block wakes only the worker lanes it dispatches", "[worker-pool]")
{
    static constexpr int kWorkers = 5, kBlocks = 40;
    std::array<int, kWorkers + 1> runs {};
    AudioWorkerPool pool;
    pool.start (kWorkers, [&runs] (int lane) { ++runs[(std::size_t) lane]; });

    for (int dispatched = 0; dispatched <= kWorkers; ++dispatched)
    {
        const auto runsBefore = runs;
        std::array<std::uint32_t, kWorkers> wakesBefore {};
        for (int lane = 0; lane < kWorkers; ++lane)
            wakesBefore[(std::size_t) lane] = pool.workerWakeCountForTest (lane);

        std::size_t allocations = 0;
        {
            const duskstudio::test::AllocationObserver observer;
            for (int b = 0; b < kBlocks; ++b)
                pool.runBlock (dispatched);
            allocations = observer.count();
        }
        CHECK (allocations == 0);

        // A lane counts a wake once it is done with it, which can be after the
        // join has returned.
        const auto woken = [&pool, &wakesBefore] (int lane)
        {
            return (int) (pool.workerWakeCountForTest (lane) - wakesBefore[(std::size_t) lane]);
        };
        REQUIRE (eventually ([&woken, dispatched]
        {
            for (int lane = 0; lane < dispatched; ++lane)
                if (woken (lane) != kBlocks) return false;
            return true;
        }));
        for (int lane = 0; lane < kWorkers; ++lane)
        {
            const int want = lane < dispatched ? kBlocks : 0;
            CHECK (runs[(std::size_t) lane] - runsBefore[(std::size_t) lane] == want);
            CHECK (woken (lane) == want);
        }
        CHECK (runs[kWorkers] - runsBefore[kWorkers] == kBlocks);
    }
    pool.stop();
}

TEST_CASE ("AudioWorkerPool: a stray latch on a lane a block leaves asleep neither runs it nor ends the join",
           "[worker-pool]")
{
    // quiesce() wakes a lane it finds short of the current epoch, and a lane
    // it finds still running its job keeps that latch once it is done. Here
    // the caller leaves such a latch on lane 2 while a block that dispatched
    // only lane 0 is in flight, and lane 0 stays in flight until lane 2 has
    // woken and dealt with it. Lane 2 sees the block's new epoch: had it taken
    // the lane count from anywhere but the word that carried that epoch, it
    // would run its job and count itself done, and the join would return with
    // lane 0 still running.
    static constexpr int kWorkers = 3, kStray = 2;
    AudioWorkerPool pool;
    std::array<int, kWorkers + 1> runs {};
    bool stray = false, strayHandled = false, laneZeroFinished = false;
    std::uint32_t strayWakesBefore = 0;
    pool.start (kWorkers, [&] (int lane)
    {
        ++runs[(std::size_t) lane];
        if (! stray)
            return;
        if (lane == kWorkers)
            pool.wakeLaneForTest (kStray);
        if (lane == 0)
        {
            strayHandled = eventually ([&pool, &strayWakesBefore]
            {
                return pool.workerWakeCountForTest (kStray) != strayWakesBefore;
            });
            laneZeroFinished = true;
        }
    });

    pool.runBlock();
    strayWakesBefore = pool.workerWakeCountForTest (kStray);
    stray = true;
    pool.runBlock (1);
    REQUIRE (laneZeroFinished);
    REQUIRE (strayHandled);
    REQUIRE (runs == std::array<int, kWorkers + 1> { 2, 1, 1, 2 });

    stray = false;
    pool.runBlock();
    REQUIRE (runs == std::array<int, kWorkers + 1> { 3, 2, 2, 3 });
    pool.stop();
}

TEST_CASE ("AudioWorkerPool: blocks of every size between stray latches and quiesced dead dispatches run each lane once",
           "[worker-pool]")
{
    // Blocks dispatch a varying number of lanes. Between them the caller
    // leaves stray latches on lanes, and now and then a dispatcher dies with
    // its lanes running and quiesce() drains them, which leaves a latch on any
    // lane it finds mid-job: the latches the next, smaller blocks meet.
    static constexpr int kWorkers = 4, kLanes = kWorkers + 1, kBlocks = 2000;
    std::array<std::uint64_t, kLanes> state {}, expected {};
    std::array<int, kLanes> runs {}, expectedRuns {};
    int dispatchedNow = 0, strayLane = -1;
    bool deadDispatch = false, misrun = false;

    AudioWorkerPool pool;
    pool.start (kWorkers, [&] (int lane)
    {
        if (deadDispatch)
        {
            // Long enough for quiesce() to find the lane still running.
            for (int i = 0; i < 2000; ++i)
                std::this_thread::yield();
            return;
        }
        if (lane < kWorkers && lane >= dispatchedNow)
            misrun = true;
        state[(std::size_t) lane] = state[(std::size_t) lane] * 3 + (std::uint64_t) lane + 1;
        ++runs[(std::size_t) lane];
        if (lane == kWorkers && strayLane >= 0)
            pool.wakeLaneForTest (strayLane);
    });

    std::uint32_t mix = 0x9e3779b9u;
    for (int b = 0; b < kBlocks; ++b)
    {
        mix ^= mix << 13;
        mix ^= mix >> 17;
        mix ^= mix << 5;
        if (b % 97 == 96)
        {
            deadDispatch = true;
            pool.dispatchForTest();
            pool.quiesce();
            deadDispatch = false;
        }
        dispatchedNow = (int) (mix % (std::uint32_t) kLanes);
        strayLane = (mix >> 8) % 3 == 0 ? (int) ((mix >> 12) % (std::uint32_t) kWorkers) : -1;
        pool.runBlock (dispatchedNow);
        for (int lane = 0; lane < kLanes; ++lane)
        {
            if (lane < kWorkers && lane >= dispatchedNow)
                continue;
            expected[(std::size_t) lane] = expected[(std::size_t) lane] * 3 + (std::uint64_t) lane + 1;
            ++expectedRuns[(std::size_t) lane];
        }
    }
    pool.stop();

    REQUIRE_FALSE (misrun);
    REQUIRE (runs == expectedRuns);
    REQUIRE (state == expected);
}
