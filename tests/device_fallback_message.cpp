#include <catch2/catch_test_macros.hpp>

#include "engine/DeviceFallbackMessage.h"
#include "foundation/Text.h"

#include <string>

using duskstudio::startupDeviceMessage;
using dusk::text::contains;
using dusk::text::containsIgnoreCase;

// The startup busy-device fallback resolves (opened?, savedName, actualName)
// into the alert copy. The JUCE device switching itself needs real hardware, so
// only this decision is unit-tested — but it's the only branchy part.

TEST_CASE ("startupDeviceMessage: saved device opened -> no alert", "[audio][device]")
{
    // Same device that was saved opened fine.
    REQUIRE (startupDeviceMessage (true, "UMC1820", "UMC1820").empty());
    // No saved device to compare (fresh machine) and something opened.
    REQUIRE (startupDeviceMessage (true, "", "Built-in Audio").empty());
}

TEST_CASE ("startupDeviceMessage: fell back to a different device -> names both", "[audio][device]")
{
    const auto m = startupDeviceMessage (true, "UMC1820", "Built-in Audio");
    REQUIRE_FALSE (m.empty());
    REQUIRE (contains (m, "UMC1820"));          // the saved device that was busy
    REQUIRE (contains (m, "Built-in Audio"));   // the substitute now in use
    REQUIRE (containsIgnoreCase (m, "in use"));
    // We did NOT change the saved device — it returns next launch.
    REQUIRE (containsIgnoreCase (m, "next launch"));
}

TEST_CASE ("startupDeviceMessage: nothing opened -> silent-session warning", "[audio][device]")
{
    SECTION ("names the saved device")
    {
        const auto m = startupDeviceMessage (false, "UMC1820", "");
        REQUIRE (contains (m, "UMC1820"));
        REQUIRE (containsIgnoreCase (m, "no audio device"));
        REQUIRE (containsIgnoreCase (m, "recording is disabled"));
    }
    SECTION ("no saved device still warns, no dangling name")
    {
        const auto m = startupDeviceMessage (false, "", "");
        REQUIRE (containsIgnoreCase (m, "no audio device"));
        REQUIRE_FALSE (contains (m, "\"\""));   // no empty-quoted device name
    }
}
