#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_core/juce_core.h>

#include "engine/midi/WinMmProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

using duskstudio::midi::BackendDeviceInfo;
using duskstudio::midi::winmm::InputClock;
using duskstudio::midi::winmm::finishDeviceEnumeration;
using Catch::Matchers::WithinAbs;

TEST_CASE ("WinMM identifiers preserve interface paths and fallback names", "[midi][winmm][issue-299]")
{
    std::vector<BackendDeviceInfo> devices {
        { "Keys", R"(\\?\midi#vid_1234&pid_5678#{ab-cd})" },
        { u8"\u00c9cho \U0001f3b9", "" },
        { "", "port-2, endpoint -123" }
    };
    finishDeviceEnumeration (devices);
    REQUIRE (devices.size() == 3);
    REQUIRE (devices[0].name == "Keys");
    REQUIRE (devices[0].identifier == R"(\\?\midi#vid_1234&pid_5678#{ab-cd})");
    REQUIRE (devices[1].name == u8"\u00c9cho \U0001f3b9");
    REQUIRE (devices[1].identifier == u8"\u00c9cho \U0001f3b9");
    REQUIRE (devices[2].name.empty());
    REQUIRE (devices[2].identifier == "port-2, endpoint -123");
}

TEST_CASE ("WinMM duplicate names and IDs are decorated independently in enumeration order", "[midi][winmm][issue-299]")
{
    std::vector<BackendDeviceInfo> devices {
        { "Keys", "port" }, { "Keys", "other" }, { "KEYS", "port" },
        { "Keys-2", "other" }, { "Keys", "port-2" }
    };
    finishDeviceEnumeration (devices);
    const std::vector<std::string> names { "Keys", "Keys-2", "KEYS", "Keys-2-2", "Keys-3" };
    const std::vector<std::string> ids { "port", "other", "port-2", "other-2", "port-2-2" };
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        REQUIRE (devices[i].name == names[i]);
        REQUIRE (devices[i].identifier == ids[i]);
    }

    std::vector<BackendDeviceInfo> reordered { { "Keys-2", "port-2" }, { "Keys", "port" }, { "Keys", "port" } };
    finishDeviceEnumeration (reordered);
    // The existing fallback's suffixes can collide with an earlier literal name.
    REQUIRE (reordered[0].identifier == "port-2");
    REQUIRE (reordered[2].identifier == "port-2");
    REQUIRE (reordered[0].name == "Keys-2");
    REQUIRE (reordered[2].name == "Keys-2");
}

TEST_CASE ("WinMM enumeration agrees with JUCE duplicate decoration", "[midi][winmm][issue-299]")
{
    const std::vector<std::vector<BackendDeviceInfo>> snapshots {
        {}, { { "", "" } }, { { "", "" }, { "", "" }, { "-2", "" } },
        { { "A-2", "id-2" }, { "A", "id" }, { "A", "id" }, { "A-2", "id-2" } },
        { { "A", "" }, { "A", "A" }, { "A", "A-2" }, { "a", "a" }, { "A-2", "A" } },
        { { u8"\u00c9cho", "" }, { u8"\u00e9cho", "" }, { u8"\u00c9cho", "" }, { u8"\U0001f3b9", "" }, { u8"\U0001f3b9", "" } }
    };
    for (auto devices : snapshots)
    {
        juce::StringArray names, ids;
        for (const auto& device : devices)
        {
            names.add (juce::String::fromUTF8 (device.name.c_str()));
            ids.add (juce::String::fromUTF8 ((device.identifier.empty() ? device.name : device.identifier).c_str()));
        }
        names.appendNumbersToDuplicates (false, false, juce::CharPointer_UTF8 ("-"), juce::CharPointer_UTF8 (""));
        ids.appendNumbersToDuplicates (false, false, juce::CharPointer_UTF8 ("-"), juce::CharPointer_UTF8 (""));
        finishDeviceEnumeration (devices);
        REQUIRE (devices.size() == static_cast<std::size_t> (names.size()));
        for (std::size_t i = 0; i < devices.size(); ++i)
        {
            REQUIRE (devices[i].name == names[static_cast<int> (i)].toStdString());
            REQUIRE (devices[i].identifier == ids[static_cast<int> (i)].toStdString());
        }
    }
}

TEST_CASE ("WinMM input timestamps retain source time and reset on a new start", "[midi][winmm][issue-299]")
{
    InputClock clock (1000.25);
    REQUIRE_THAT (clock.toBackendTime (0, 1001.0), WithinAbs (1000.25, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (100, 1500.0), WithinAbs (1100.25, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (25, 1600.0), WithinAbs (1025.25, 1.0e-6));
    clock.reset (2000.5);
    REQUIRE_THAT (clock.toBackendTime (0, 2001.0), WithinAbs (2000.5, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (25, 2100.0), WithinAbs (2025.5, 1.0e-6));
}

TEST_CASE ("WinMM future timestamps retain the fallback clock correction", "[midi][winmm][issue-299]")
{
    InputClock clock (1000.0);
    REQUIRE_THAT (clock.toBackendTime (10, 1008.0), WithinAbs (1008.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (10, 1020.0), WithinAbs (1010.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (30, 1020.0), WithinAbs (1020.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (40, 1100.0), WithinAbs (1039.0, 1.0e-6));
    clock.reset (2000.0);
    REQUIRE_THAT (clock.toBackendTime (40, 2100.0), WithinAbs (2040.0, 1.0e-6));
}

TEST_CASE ("WinMM input timestamps unwrap across long runs and delayed completion", "[midi][winmm][issue-299]")
{
    constexpr double wrap = 4294967296.0;
    InputClock clock (1000.0);
    REQUIRE_THAT (clock.toBackendTime (0xfffffff0u, 1000.0 + wrap - 10.0), WithinAbs (1000.0 + wrap - 16.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (0, 1000.0 + wrap - 0.5), WithinAbs (1000.0 + wrap - 0.5, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (4, 1000.0 + wrap + 8.0), WithinAbs (1000.0 + wrap + 4.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (0xffffffffu, 1000.0 + wrap + 10.0), WithinAbs (1000.0 + wrap - 1.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (25, 1000.0 + 3.0 * wrap + 30.0), WithinAbs (1000.0 + 3.0 * wrap + 25.0, 1.0e-6));
    clock.reset (2000.0);
    REQUIRE_THAT (clock.toBackendTime (25, 2100.0), WithinAbs (2025.0, 1.0e-6));
    REQUIRE_THAT (clock.toBackendTime (0xffffffffu, 2100.0), WithinAbs (2000.0, 1.0e-6));
}
