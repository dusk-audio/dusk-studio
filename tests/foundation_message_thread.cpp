#include <catch2/catch_test_macros.hpp>

#include "foundation/MessageThread.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <vector>

// The event seam is a thin wrapper over the platform message loop.
// ScopedJuceInitialiser_GUI brings the loop up and tears it down when the scope
// unwinds (including on a failed REQUIRE). The start/stop state and callAsync's
// defer-not-run-inline contract are checked everywhere; the actual-fire
// assertions pump a real loop and so run on Linux and Windows only - a headless
// macOS CI runner has no Aqua session, so runDispatchLoop returns without ever
// driving a juce::Timer tick.

namespace
{
struct CountingTimer final : dusk::Timer
{
    std::atomic<int> ticks { 0 };
    void timerCallback() override { ticks.fetch_add (1, std::memory_order_relaxed); }
};

#if ! defined (__APPLE__)
// Runs the dispatch loop until `done` holds or the deadline passes. This JUCE
// build has no runDispatchLoopUntil, and stopDispatchLoop latches the quit flag
// for the life of the MessageManager, so each test case gets exactly one pump;
// a second call in the same case would dispatch nothing. Itself a dusk::Timer,
// so it also exercises the seam.
struct LoopStopper final : dusk::Timer
{
    std::function<bool()> done;
    std::chrono::steady_clock::time_point deadline;

    void timerCallback() override
    {
        if (! done() && std::chrono::steady_clock::now() < deadline) return;
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
};

void pumpUntil (std::function<bool()> done, std::chrono::milliseconds timeout)
{
    LoopStopper stopper;
    stopper.done = std::move (done);
    stopper.deadline = std::chrono::steady_clock::now() + timeout;
    stopper.startTimer (10);
    juce::MessageManager::getInstance()->runDispatchLoop();
}
#endif
} // namespace

TEST_CASE ("dusk::Timer starts, stops, and fires on the message thread", "[foundation][events]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    CountingTimer timer;
    REQUIRE_FALSE (timer.isTimerRunning());

    timer.startTimerHz (60);
    REQUIRE (timer.isTimerRunning());

#if ! defined (__APPLE__)
    pumpUntil ([&] { return timer.ticks.load() > 0; }, std::chrono::seconds (5));
    REQUIRE (timer.ticks.load() > 0);
#endif

    timer.stopTimer();
    REQUIRE_FALSE (timer.isTimerRunning());
}

TEST_CASE ("a stopped dusk::Timer does not fire", "[foundation][events]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    CountingTimer timer;
    timer.startTimerHz (200);
    timer.stopTimer();
    REQUIRE_FALSE (timer.isTimerRunning());

#if ! defined (__APPLE__)
    pumpUntil ([] { return false; }, std::chrono::milliseconds (60));
#endif
    REQUIRE (timer.ticks.load() == 0);
}

TEST_CASE ("dusk::callAsync defers and dispatches on the message thread", "[foundation][events]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::atomic<bool> ran { false };
    const bool queued = dusk::callAsync ([&ran] { ran.store (true, std::memory_order_relaxed); });
    REQUIRE (queued);
    REQUIRE_FALSE (ran.load());   // deferred, not run inline

#if ! defined (__APPLE__)
    pumpUntil ([&ran] { return ran.load(); }, std::chrono::seconds (5));
    REQUIRE (ran.load());
#endif
}

TEST_CASE ("a termination signal routes a quit request to the message thread", "[foundation][events]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::atomic<bool> asked { false };
    dusk::QuitSignalHandler handler ([&asked] { asked.store (true, std::memory_order_relaxed); });
    REQUIRE (handler.isInstalled());

    REQUIRE (std::raise (SIGTERM) == 0);
    REQUIRE_FALSE (asked.load());   // latched in the handler, not called back from signal context

#if ! defined (__APPLE__)
    pumpUntil ([&asked] { return asked.load(); }, std::chrono::seconds (5));
    REQUIRE (asked.load());
#endif
}

#if ! defined (__APPLE__)
namespace
{
struct StampingTimer final : dusk::Timer
{
    std::vector<std::chrono::steady_clock::time_point>* stamps = nullptr;
    int fired = 0;
    void timerCallback() override
    {
        ++fired;
        stamps->push_back (std::chrono::steady_clock::now());
    }
};
} // namespace

TEST_CASE ("dusk::Timers started at different moments fire in shared passes", "[foundation][events][ipc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    static constexpr int kTimers = 12;
    std::vector<std::chrono::steady_clock::time_point> stamps;
    stamps.reserve (4096);
    std::vector<std::unique_ptr<StampingTimer>> timers;
    for (int i = 0; i < kTimers; ++i)
    {
        timers.push_back (std::make_unique<StampingTimer>());
        timers.back()->stamps = &stamps;
    }

    // Started one by one from the loop, a few milliseconds apart, so each would
    // keep its own phase on timers of its own.
    struct Starter final : dusk::Timer
    {
        std::vector<std::unique_ptr<StampingTimer>>* toStart = nullptr;
        std::size_t next = 0;
        void timerCallback() override
        {
            (*toStart)[next]->startTimerHz (30);
            if (++next == toStart->size()) stopTimer();
        }
    } starter;
    starter.toStart = &timers;
    starter.startTimer (3);

    const auto begun = std::chrono::steady_clock::now();
    pumpUntil ([] { return false; }, std::chrono::milliseconds (1200));
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - begun).count();
    for (auto& timer : timers)
        timer->stopTimer();

    std::sort (stamps.begin(), stamps.end());
    int passes = 0;
    for (std::size_t i = 0; i < stamps.size(); ++i)
        if (i == 0 || stamps[i] - stamps[i - 1] > std::chrono::milliseconds (2))
            ++passes;

    for (const auto& timer : timers)
        REQUIRE (timer->fired >= 20);
    // One pass per 30 Hz instant, give or take the shorter starter timer's own.
    REQUIRE (passes <= static_cast<int> (seconds * 30.0) + 4);
}
#endif
