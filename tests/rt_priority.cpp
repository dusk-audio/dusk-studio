#include <catch2/catch_test_macros.hpp>

#include "engine/RtPriority.h"

// jucePriorityForSchedCeiling inverts JUCE's integer jmap of realtime priority
// [0,10] onto SCHED_RR [rrMin,rrMax]. The contract: the returned priority's
// forward-mapped sched value must never exceed the rlimit ceiling (EPERM →
// silent SCHED_OTHER fallback), and it must be the largest such priority.

using duskstudio::rt::jucePriorityForSchedCeiling;

namespace
{
// JUCE's forward map (jmap with integer truncation).
int forwardMap (int p, int rrMin, int rrMax)
{
    return rrMin + (p * (rrMax - rrMin)) / 10;
}
} // namespace

TEST_CASE ("rt priority: known ceilings on the Linux 1..99 range", "[rt-priority]")
{
    REQUIRE (jucePriorityForSchedCeiling (95, 1, 99) == 9);   // the bug that bit: 10→99>95
    REQUIRE (jucePriorityForSchedCeiling (99, 1, 99) == 10);
    REQUIRE (jucePriorityForSchedCeiling (89, 1, 99) == 9);   // forward(9) = 89 exactly
    REQUIRE (jucePriorityForSchedCeiling (50, 1, 99) == 5);
    REQUIRE (jucePriorityForSchedCeiling (10, 1, 99) == 1);   // p=1 maps to exactly 10
}

TEST_CASE ("rt priority: result always fits under the ceiling", "[rt-priority]")
{
    for (int ceiling = 0; ceiling <= 120; ++ceiling)
    {
        const int p = jucePriorityForSchedCeiling (ceiling, 1, 99);
        if (p > 0)
        {
            REQUIRE (forwardMap (p, 1, 99) <= ceiling);
            // Largest fitting: the next priority up must overshoot (or be > 10).
            if (p < 10)
                REQUIRE (forwardMap (p + 1, 1, 99) > ceiling);
        }
    }
}

TEST_CASE ("rt priority: degenerate inputs return no-RT", "[rt-priority]")
{
    REQUIRE (jucePriorityForSchedCeiling (0, 1, 99) == 0);    // ceiling below rrMin
    REQUIRE (jucePriorityForSchedCeiling (5, 1, 99) == 0);    // p=1 needs sched 10
    REQUIRE (jucePriorityForSchedCeiling (95, 99, 1) == 0);   // rrMax <= rrMin
    REQUIRE (jucePriorityForSchedCeiling (95, 50, 50) == 0);
}
