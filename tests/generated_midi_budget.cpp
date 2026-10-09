#include <catch2/catch_test_macros.hpp>

#include "engine/GeneratedMidiBudget.h"
#include "foundation/MidiBuffer.h"

#include <cstddef>
#include <cstdint>

using duskstudio::GeneratedMidiBudget;

TEST_CASE ("Generated MIDI structural reset releases its reserved capacity",
           "[audio-engine][midi][budget]")
{
    constexpr int eventBytes = 9;
    constexpr int resetBytes = 48 * eventBytes;
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
    constexpr int resetBytes = 48 * eventBytes;
    GeneratedMidiBudget budget (resetBytes - eventBytes);

    CHECK_FALSE (budget.reserveStructural (resetBytes));
    CHECK_FALSE (budget.consumeStructural (resetBytes));
    CHECK (budget.remainingBytes() == resetBytes - eventBytes);
    CHECK (budget.reservedStructuralBytes() == 0);
}

TEST_CASE ("Generated MIDI is charged what it takes in the routing buffer",
           "[audio-engine][midi][budget]")
{
    // A buffer that holds exactly the budget must take every event the budget
    // lets the scheduler generate.
    dusk::MidiBuffer generated;
    generated.reserveBytes ((std::size_t) duskstudio::kGeneratedMidiBudgetBytes);
    GeneratedMidiBudget budget (duskstudio::kGeneratedMidiBudgetBytes);
    const std::uint8_t controller[3] { 0xB0, 1, 0 };
    int charged = 0, carried = 0;
    while (budget.spendDiscretionary (duskstudio::kGeneratedEventBytes))
    {
        ++charged;
        if (generated.addEvent (controller, 3, 0)) ++carried;
    }
    CHECK (charged > 0);
    REQUIRE (carried == charged);
}

TEST_CASE ("A track's routing buffer holds the generated budget and both live inputs",
           "[audio-engine][midi][budget]")
{
    // The timeline's events, then a full block from the track's MIDI input and
    // one from the on-screen keyboard an armed track also hears.
    dusk::MidiBuffer routing;
    routing.reserveBytes (dusk::kMidiRoutingBlockBytes);
    GeneratedMidiBudget budget (duskstudio::kGeneratedMidiBudgetBytes);
    const std::uint8_t controller[3] { 0xB0, 1, 0 };
    int refused = 0;
    while (budget.spendDiscretionary (duskstudio::kGeneratedEventBytes))
        if (! routing.addEvent (controller, 3, 0)) ++refused;

    for (int source = 0; source < 2; ++source)
    {
        dusk::MidiBuffer input;
        input.reserveBytes (dusk::kMidiBlockBytes);
        while (input.addEvent (controller, 3, 0)) {}
        for (const auto meta : input)
            if (! routing.addEvent (meta.data, meta.numBytes, meta.samplePosition)) ++refused;
    }
    REQUIRE (refused == 0);
}
