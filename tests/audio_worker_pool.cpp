#include <catch2/catch_test_macros.hpp>

#include "engine/AudioWorkerPool.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

// The pool's join/quiesce protocol is the unit under test, so these tests run
// real worker threads — the one sanctioned exception to the no-threads test
// guideline. CI has no rtprio limit, so workers exercise the non-RT fallback
// path, which is exactly the configuration where the old unbounded yield-spin
// join could deadlock.

using duskstudio::AudioWorkerPool;

namespace
{
struct LaneCounters
{
    std::array<std::atomic<int>, 8> runs {};
    void hit (int lane) { runs[(size_t) lane].fetch_add (1, std::memory_order_relaxed); }
    int  get (int lane) const { return runs[(size_t) lane].load (std::memory_order_relaxed); }
};

// Spin until pred() holds or a generous deadline expires, then REQUIRE it —
// turns a dispatch/thread-startup regression into a fast test failure instead
// of an unbounded spin that hangs CI.
template <typename Pred>
void requireBefore (Pred pred, int timeoutMs = 2000)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! pred() && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::yield();
    REQUIRE (pred());
}

// quiesce() is intentionally unbounded in production (a wedged worker has no
// safe recovery). In the tests that exercise its draining, back it with a
// deadlock watchdog: if it hasn't returned within timeoutMs the protocol has
// regressed into a hang, so abort with a clear message — a fast, obvious CI
// failure instead of letting the whole job time out minutes later. The main
// thread is blocked inside quiesce() during a hang, so only an external thread
// can react, and the only way for it to fail fast is to terminate the process.
void quiesceWithWatchdog (AudioWorkerPool& pool, int timeoutMs = 5000)
{
    std::atomic<bool> done { false };
    std::thread watchdog ([&]
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (! done.load (std::memory_order_acquire)
               && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (10);
        if (! done.load (std::memory_order_acquire))
        {
            std::fprintf (stderr, "AudioWorkerPool::quiesce() did not return within %d ms "
                                  "- deadlock regression.\n", timeoutMs);
            std::fflush (stderr);
            std::abort();
        }
    });
    pool.quiesce();
    done.store (true, std::memory_order_release);
    watchdog.join();
}
} // namespace

TEST_CASE ("AudioWorkerPool: every lane runs exactly once per block over a soak",
           "[worker-pool]")
{
    AudioWorkerPool pool;
    LaneCounters counters;
    pool.start (3, [&] (int lane) { counters.hit (lane); });

    constexpr int kBlocks = 2000;
    for (int b = 0; b < kBlocks; ++b)
        pool.runBlock();

    for (int lane = 0; lane < 4; ++lane)   // 3 workers + the caller lane
        REQUIRE (counters.get (lane) == kBlocks);

    pool.stop();
}

TEST_CASE ("AudioWorkerPool: quiesce is a prompt no-op when idle", "[worker-pool]")
{
    AudioWorkerPool pool;
    LaneCounters counters;

    SECTION ("inactive pool (zero workers)")
    {
        pool.start (0, [&] (int lane) { counters.hit (lane); });
        pool.quiesce();
        REQUIRE (counters.get (0) == 0);
    }

    SECTION ("started but never dispatched")
    {
        pool.start (3, [&] (int lane) { counters.hit (lane); });
        pool.quiesce();
        for (int lane = 0; lane < 4; ++lane)
            REQUIRE (counters.get (lane) == 0);

        pool.runBlock();
        for (int lane = 0; lane < 4; ++lane)
            REQUIRE (counters.get (lane) == 1);
    }

    SECTION ("after healthy blocks")
    {
        pool.start (3, [&] (int lane) { counters.hit (lane); });
        pool.runBlock();
        pool.quiesce();
        pool.runBlock();
        for (int lane = 0; lane < 4; ++lane)
            REQUIRE (counters.get (lane) == 2);
    }

    pool.stop();
}

TEST_CASE ("AudioWorkerPool: quiesce drains lanes orphaned by a dead dispatcher",
           "[worker-pool]")
{
    // Simulates the force-killed I/O thread: dispatchForTest signals the
    // workers and never joins. The lanes block on a gate (a worker mid-plugin);
    // quiesce must wait for them, and the pool must dispatch cleanly afterwards.
    // Wait until every worker is INSIDE its job before quiescing — a worker that
    // hasn't woken yet when quiesce begins is legitimately suppressed (the safe
    // direction), which is covered by the partial-orphan test below.
    AudioWorkerPool pool;
    LaneCounters counters;
    juce::WaitableEvent gate { /*manualReset*/ true };
    std::atomic<int> entered { 0 };
    pool.start (3, [&] (int lane)
    {
        entered.fetch_add (1, std::memory_order_relaxed);
        gate.wait();
        counters.hit (lane);
    });

    pool.dispatchForTest();   // all 3 signalled, no join — dispatcher "dies" here

    // Safety net: if requireBefore() below throws (REQUIRE miss), the workers
    // are still blocked in gate.wait() inside job_, so the pool destructor's
    // quiesce() would wait on them forever — a CI hang instead of a clean
    // failure. This guard releases the gate on ANY scope exit so the workers
    // finish and quiesce can drain. (A plain std::thread here would be worse:
    // destroyed joinable during unwinding, it calls std::terminate.)
    struct GateGuard { juce::WaitableEvent& g; ~GateGuard() { g.signal(); } } gateGuard { gate };

    requireBefore ([&] { return entered.load (std::memory_order_relaxed) >= 3; });

    std::thread releaser ([&] { juce::Thread::sleep (50); gate.signal(); });
    quiesceWithWatchdog (pool);   // must block until all 3 orphaned lanes complete
    releaser.join();

    for (int lane = 0; lane < 3; ++lane)
        REQUIRE (counters.get (lane) == 1);

    pool.runBlock();          // next dispatch is clean — no straggler ABA
    for (int lane = 0; lane < 3; ++lane)
        REQUIRE (counters.get (lane) == 2);
    REQUIRE (counters.get (3) == 1);   // caller lane never ran during the orphan

    pool.stop();
}

TEST_CASE ("AudioWorkerPool: quiesce on a partially-signalled orphan skips undispatched jobs",
           "[worker-pool]")
{
    // Dispatcher died after signalling only worker 0. The old done==numWorkers
    // quiesce design hangs forever here; the epoch/ack design must return, run
    // lane 0 exactly once, and NOT run lanes 1-2 (their wake comes from quiesce,
    // whose inputs may already be freed device buffers).
    AudioWorkerPool pool;
    LaneCounters counters;
    pool.start (3, [&] (int lane) { counters.hit (lane); });

    pool.dispatchForTest (1);
    requireBefore ([&] { return counters.get (0) >= 1; });   // worker 0 finished its lane
    quiesceWithWatchdog (pool);

    REQUIRE (counters.get (0) == 1);
    REQUIRE (counters.get (1) == 0);
    REQUIRE (counters.get (2) == 0);

    pool.runBlock();          // pool is healthy after the drain
    for (int lane = 0; lane < 4; ++lane)
        REQUIRE (counters.get (lane) == (lane == 0 ? 2 : 1));

    pool.stop();
}
