#include <catch2/catch_test_macros.hpp>

#include "engine/GeneratedMidiBudget.h"

using duskstudio::GeneratedMidiBudget;

TEST_CASE ("Generated MIDI structural reset releases its reserved capacity",
           "[audio-engine][midi][budget]")
{
    constexpr int eventBytes = 9;
    constexpr int resetBytes = 32 * eventBytes;
    GeneratedMidiBudget budget (resetBytes + 2 * eventBytes);

    REQUIRE (budget.reserveStructural (resetBytes));
    CHECK (budget.reservedStructuralBytes() == resetBytes);
    CHECK (budget.discretionaryBytesAvailable() == 2 * eventBytes);
    CHECK_FALSE (budget.spendDiscretionary (3 * eventBytes));

    REQUIRE (budget.consumeStructural (resetBytes));
    CHECK (budget.reservedStructuralBytes() == 0);
    CHECK (budget.remainingBytes() == 2 * eventBytes);
    CHECK (budget.spendDiscretionary (eventBytes));
    CHECK (budget.spendDiscretionary (eventBytes));
    CHECK_FALSE (budget.spendDiscretionary (eventBytes));
}

TEST_CASE ("Generated MIDI structural reset consumption is all or nothing",
           "[audio-engine][midi][budget]")
{
    constexpr int eventBytes = 9;
    constexpr int resetBytes = 32 * eventBytes;
    GeneratedMidiBudget budget (resetBytes - eventBytes);

    CHECK_FALSE (budget.reserveStructural (resetBytes));
    CHECK_FALSE (budget.consumeStructural (resetBytes));
    CHECK (budget.remainingBytes() == resetBytes - eventBytes);
    CHECK (budget.reservedStructuralBytes() == 0);
}
