#include <catch2/catch_test_macros.hpp>

#include "foundation/TimerSchedule.h"

#include <chrono>
#include <cstddef>
#include <set>
#include <vector>

using dusk::detail::TimerSchedule;
using namespace std::chrono_literals;

namespace
{
// Plays the owner: wakes at each deadline the schedule asks for, as a backend timer
// would, and records every instant it woke and how often each slot fired.
struct Driver
{
    TimerSchedule schedule;
    TimerSchedule::TimePoint now = TimerSchedule::TimePoint (1000s);
    std::set<TimerSchedule::TimePoint::rep> wakes;
    std::vector<TimerSchedule::Slot*> fired;

    void runUntil (TimerSchedule::TimePoint end)
    {
        while (true)
        {
            const auto next = schedule.nextDue();
            if (! next || *next > end)
                break;
            now = std::max (now, *next);
            fired.clear();
            schedule.takeDue (now, fired);
            if (! fired.empty())
                wakes.insert (now.time_since_epoch().count());
            for (auto* slot : fired)
                ++*static_cast<int*> (slot->owner);
        }
        now = end;
    }
};
} // namespace

TEST_CASE ("timers started at different moments share the same deadlines", "[foundation][events]")
{
    Driver driver;
    static constexpr std::size_t kTimers = 30;
    std::vector<TimerSchedule::Slot> slots (kTimers);
    std::vector<int> counts (kTimers, 0);
    for (std::size_t i = 0; i < kTimers; ++i)
    {
        slots[i].owner = &counts[i];
        driver.schedule.start (slots[i], TimerSchedule::Duration (1s) / 30, driver.now);
        driver.now += 7ms;
    }

    const auto from = driver.now;
    driver.wakes.clear();
    driver.runUntil (from + 10s);

    // One wake per 30 Hz instant, not one per timer per period.
    REQUIRE (driver.wakes.size() <= 301);
    for (const int count : counts)
        REQUIRE (count >= 299);
}

TEST_CASE ("a 60 Hz timer and slower ones wake together", "[foundation][events]")
{
    Driver driver;
    TimerSchedule::Slot fast, pump, poll;
    int fastCount = 0, pumpCount = 0, pollCount = 0;
    fast.owner = &fastCount;
    pump.owner = &pumpCount;
    poll.owner = &pollCount;
    driver.schedule.start (fast, TimerSchedule::Duration (1s) / 60, driver.now);
    driver.now += 5ms;
    driver.schedule.start (pump, 32ms, driver.now);
    driver.now += 11ms;
    driver.schedule.start (poll, 250ms, driver.now);

    driver.runUntil (driver.now + 10s);

    REQUIRE (driver.wakes.size() <= 601);
    REQUIRE (fastCount >= 599);
    REQUIRE (pumpCount >= 290);
    REQUIRE (pollCount >= 39);
    REQUIRE (pollCount <= 41);
}

TEST_CASE ("a timer keeps its average rate on the shared instants", "[foundation][events]")
{
    for (const auto period : { TimerSchedule::Duration (50ms), TimerSchedule::Duration (40ms),
                               TimerSchedule::Duration (20ms), TimerSchedule::Duration (100ms) })
    {
        Driver driver;
        TimerSchedule::Slot slot;
        int count = 0;
        slot.owner = &count;
        driver.schedule.start (slot, period, driver.now);
        driver.runUntil (driver.now + 10s);
        const auto expected = static_cast<int> (10s / period);
        REQUIRE (count >= expected - 1);
        REQUIRE (count <= expected + 1);
    }
}

TEST_CASE ("a timer never fires before its period has run", "[foundation][events]")
{
    for (const auto period : { TimerSchedule::Duration (5ms), TimerSchedule::Duration (16ms),
                               TimerSchedule::Duration (33ms), TimerSchedule::Duration (500ms) })
    {
        TimerSchedule schedule;
        TimerSchedule::Slot slot;
        const auto started = TimerSchedule::TimePoint (1000s) + 3ms;
        schedule.start (slot, period, started);
        const auto due = schedule.nextDue();
        REQUIRE (due.has_value());
        REQUIRE (*due + TimerSchedule::kTolerance >= started + period);

        std::vector<TimerSchedule::Slot*> fired;
        schedule.takeDue (started + period - 2 * TimerSchedule::kTolerance, fired);
        REQUIRE (fired.empty());
    }
}

TEST_CASE ("a short timer keeps its own deadlines", "[foundation][events]")
{
    Driver driver;
    TimerSchedule::Slot slot;
    int count = 0;
    slot.owner = &count;
    const auto started = driver.now + 3ms;
    driver.now = started;
    driver.schedule.start (slot, 10ms, started);
    REQUIRE (driver.schedule.nextDue() == started + 10ms);
    driver.runUntil (started + 1s);
    REQUIRE (count == 100);
}

TEST_CASE ("a stopped timer leaves the schedule and a late one skips what it missed", "[foundation][events]")
{
    TimerSchedule schedule;
    TimerSchedule::Slot a, b;
    const auto now = TimerSchedule::TimePoint (1000s);
    schedule.start (a, 33ms, now);
    schedule.start (b, 100ms, now);
    REQUIRE (schedule.size() == 2);
    schedule.stop (a);
    schedule.stop (a);
    REQUIRE_FALSE (TimerSchedule::isScheduled (a));
    REQUIRE (TimerSchedule::isScheduled (b));
    REQUIRE (schedule.size() == 1);

    std::vector<TimerSchedule::Slot*> fired;
    const auto late = now + 2s;
    schedule.takeDue (late, fired);
    REQUIRE (fired.size() == 1);
    REQUIRE (schedule.nextDue() >= late + 100ms);
    fired.clear();
    schedule.takeDue (late + 50ms, fired);
    REQUIRE (fired.empty());
}

TEST_CASE ("a timer that waited behind a slow callback counts its period from its own turn", "[foundation][events]")
{
    TimerSchedule schedule;
    TimerSchedule::Slot slow, behind;
    const auto started = TimerSchedule::TimePoint (1000s);
    schedule.start (slow, 10ms, started);
    schedule.start (behind, 10ms, started);

    // One pass, dispatched the way the shared tick does it: each slot is rebased
    // just before its callback, and the first callback takes 100 ms.
    auto now = started + 10ms;
    std::vector<TimerSchedule::Slot*> pass;
    schedule.takeDue (now, pass);
    REQUIRE (pass.size() == 2);
    TimerSchedule::Slot* const first = pass[0];
    TimerSchedule::Slot* const second = pass[1];
    schedule.rebaseIfOverdue (*first, now);
    now += 100ms;
    schedule.rebaseIfOverdue (*second, now);
    const auto secondRan = now;
    REQUIRE (second->due + TimerSchedule::kTolerance >= secondRan + 10ms);

    // The next pass, right after the slow one, fires only the slow timer, whose
    // period ran out while it worked, and not the one that just had its turn.
    now += 1ms;
    std::vector<TimerSchedule::Slot*> next;
    schedule.takeDue (now, next);
    REQUIRE (next.size() == 1);
    REQUIRE (next[0] == first);
}
