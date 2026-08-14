#include <catch2/catch_test_macros.hpp>

#include "engine/DeviceFallbackMessage.h"
#include "foundation/Text.h"

#include <string>

using duskstudio::startupDeviceMessage;
using duskstudio::device::DeviceIdentity;
using dusk::text::contains;
using dusk::text::containsIgnoreCase;

// The startup busy-device fallback resolves the saved and live endpoint/backend
// identities into alert copy. Switching itself needs real hardware, so the pure
// decision seam carries the platform-independent regression coverage.

namespace
{
DeviceIdentity identity (const std::string& outputName, const std::string& backendName)
{
    DeviceIdentity result;
    result.outputName = outputName;
    result.backendName = backendName;
    return result;
}
} // namespace

TEST_CASE ("startupDeviceMessage: saved device opened -> no alert", "[audio][device]")
{
    // Same device that was saved opened fine.
    REQUIRE (startupDeviceMessage (true,
                                   identity ("UMC1820", "ALSA"),
                                   identity ("UMC1820", "ALSA")).empty());
    // No saved device to compare (fresh machine) and something opened.
    REQUIRE (startupDeviceMessage (true,
                                   identity ("", ""),
                                   identity ("Built-in Audio", "ALSA")).empty());
    // An unavailable live backend name cannot prove that the saved backend changed.
    REQUIRE (startupDeviceMessage (true,
                                   identity ("UMC1820", "ALSA"),
                                   identity ("UMC1820", "")).empty());
}

TEST_CASE ("startupDeviceMessage: fell back to a different device -> names both", "[audio][device]")
{
    const auto m = startupDeviceMessage (true,
                                         identity ("UMC1820", "ALSA"),
                                         identity ("Built-in Audio", "ALSA"));
    REQUIRE_FALSE (m.empty());
    REQUIRE (contains (m, "UMC1820"));          // the saved device that was busy
    REQUIRE (contains (m, "Built-in Audio"));   // the substitute now in use
    REQUIRE (containsIgnoreCase (m, "in use"));
    // We did NOT change the saved device — it returns next launch.
    REQUIRE (containsIgnoreCase (m, "next launch"));
}

TEST_CASE ("startupDeviceMessage: same endpoint on a different backend alerts",
           "[audio][device]")
{
    const auto m = startupDeviceMessage (
        true,
        identity ("Speakers", "Windows Audio (Exclusive Mode)"),
        identity ("Speakers", "Windows Audio"));
    REQUIRE_FALSE (m.empty());
    REQUIRE (contains (m, "Speakers"));
    REQUIRE (contains (m, "Windows Audio (Exclusive Mode)"));
    REQUIRE (contains (m, "Windows Audio backend"));
    REQUIRE (containsIgnoreCase (m, "switched"));
}

TEST_CASE ("startupDeviceMessage: backend-only saved identity detects fallback",
           "[audio][device]")
{
    const auto m = startupDeviceMessage (true,
                                         identity ("", "ALSA"),
                                         identity ("Built-in Audio", "PipeWire"));
    REQUIRE_FALSE (m.empty());
    REQUIRE (contains (m, "the default device on the ALSA backend"));
    REQUIRE (contains (m, "Built-in Audio"));
    REQUIRE (contains (m, "PipeWire backend"));
}

TEST_CASE ("startupDeviceMessage: nothing opened -> silent-session warning", "[audio][device]")
{
    SECTION ("names the saved device")
    {
        const auto m = startupDeviceMessage (false,
                                             identity ("UMC1820", "ALSA"),
                                             identity ("", ""));
        REQUIRE (contains (m, "UMC1820"));
        REQUIRE (containsIgnoreCase (m, "no audio device"));
        REQUIRE (containsIgnoreCase (m, "recording is disabled"));
        // The strips are prepared from the device callback, so an unopened
        // device is also why a plugin or soundfont load is refused.
        REQUIRE (containsIgnoreCase (m, "soundfonts cannot be loaded"));
    }
    SECTION ("no saved device still warns, no dangling name")
    {
        const auto m = startupDeviceMessage (false,
                                             identity ("", ""),
                                             identity ("", ""));
        REQUIRE (containsIgnoreCase (m, "no audio device"));
        REQUIRE (containsIgnoreCase (m, "recording is disabled"));
        REQUIRE (containsIgnoreCase (m, "soundfonts cannot be loaded"));
        REQUIRE_FALSE (contains (m, "\"\""));   // no empty-quoted device name
    }
}
