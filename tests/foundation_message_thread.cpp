#include <catch2/catch_test_macros.hpp>

#include "foundation/MessageThread.h"

#include <juce_events/juce_events.h>

#include <atomic>

// The event seam is a thin wrapper over the platform message loop. These tests
// drive a real JUCE MessageManager and pump it, verifying that dusk::Timer
// actually fires on the message thread and that dusk::callAsync dispatches - the
// behaviour the non-GUI engine code relies on after dropping its juce includes.

namespace
{
struct CountingTimer final : dusk::Timer
{
    std::atomic<int> ticks { 0 };
    void timerCallback() override { ticks.fetch_add (1, std::memory_order_relaxed); }
};

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
} // namespace

TEST_CASE ("dusk::Timer fires while running and stops on request", "[foundation][events]")
{
    juce::MessageManager::getInstance();   // this thread becomes the message thread

    CountingTimer timer;
    REQUIRE_FALSE (timer.isTimerRunning());

    timer.startTimerHz (60);
    REQUIRE (timer.isTimerRunning());

    pumpFor (150);
    REQUIRE (timer.ticks.load() > 0);

    timer.stopTimer();
    REQUIRE_FALSE (timer.isTimerRunning());

    // Nothing new should arrive once stopped.
    const int afterStop = timer.ticks.load();
    pumpFor (60);
    REQUIRE (timer.ticks.load() == afterStop);

    juce::MessageManager::deleteInstance();
}

TEST_CASE ("dusk::callAsync runs its callback on the next dispatch", "[foundation][events]")
{
    juce::MessageManager::getInstance();

    std::atomic<bool> ran { false };
    const bool queued = dusk::callAsync ([&ran] { ran.store (true, std::memory_order_relaxed); });
    REQUIRE (queued);
    REQUIRE_FALSE (ran.load());   // deferred, not run inline

    pumpFor (50);
    REQUIRE (ran.load());

    juce::MessageManager::deleteInstance();
}
