#include <catch2/catch_test_macros.hpp>

#include "foundation/MessageThread.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>

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
// Breaks out of runDispatchLoop after a bounded window (this JUCE build has no
// runDispatchLoopUntil). Itself a dusk::Timer, so it also exercises the seam.
struct LoopStopper final : dusk::Timer
{
    void timerCallback() override
    {
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
};

void pumpFor (int ms)
{
    LoopStopper stopper;
    stopper.startTimer (ms);
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
    pumpFor (150);
    REQUIRE (timer.ticks.load() > 0);
#endif

    timer.stopTimer();
    REQUIRE_FALSE (timer.isTimerRunning());

#if ! defined (__APPLE__)
    // Nothing new should arrive once stopped.
    const int afterStop = timer.ticks.load();
    pumpFor (60);
    REQUIRE (timer.ticks.load() == afterStop);
#endif
}

TEST_CASE ("dusk::callAsync defers and dispatches on the message thread", "[foundation][events]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::atomic<bool> ran { false };
    const bool queued = dusk::callAsync ([&ran] { ran.store (true, std::memory_order_relaxed); });
    REQUIRE (queued);
    REQUIRE_FALSE (ran.load());   // deferred, not run inline

#if ! defined (__APPLE__)
    pumpFor (50);
    REQUIRE (ran.load());
#endif
}
