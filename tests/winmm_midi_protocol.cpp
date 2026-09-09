#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_core/juce_core.h>

#include "engine/midi/WinMmProtocol.h"

#if defined(_WIN32)
 #include "engine/midi/WinMmDeviceQuery.h"
 #include <mmddk.h>
#endif

#include <algorithm>
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

#if defined(_WIN32)
namespace
{
namespace winmm = duskstudio::midi::winmm;

struct QueryDevice
{
    std::wstring name, identifier;
    MMRESULT capsResult = MMSYSERR_NOERROR, sizeResult = MMSYSERR_NOERROR, idResult = MMSYSERR_NOERROR;
    std::optional<ULONG> reportedBytes;
    bool unterminatedName = false, unterminatedId = false;
};

struct QueryFixture
{
    static inline thread_local QueryFixture* current = nullptr;
    QueryFixture* previous = current;
    std::vector<QueryDevice> inputs, outputs;
    bool invalidCall = false;

    QueryFixture() { current = this; }
    ~QueryFixture() { current = previous; }

    static UINT WINAPI inputCount() { return static_cast<UINT> (current->inputs.size()); }
    static UINT WINAPI outputCount() { return static_cast<UINT> (current->outputs.size()); }

    template <class Caps>
    static MMRESULT caps (const std::vector<QueryDevice>& devices, UINT_PTR index, Caps* result, UINT bytes)
    {
        if (index >= devices.size() || result == nullptr || bytes != sizeof (Caps))
        {
            current->invalidCall = true;
            return MMSYSERR_INVALPARAM;
        }
        const auto& device = devices[index];
        *result = {};
        if (device.unterminatedName)
            std::fill_n (result->szPname, MAXPNAMELEN, L'X');
        else
            std::copy_n (device.name.data(), std::min<std::size_t> (device.name.size(), MAXPNAMELEN - 1),
                         result->szPname);
        return device.capsResult;
    }

    static MMRESULT WINAPI inputCaps (UINT_PTR index, LPMIDIINCAPSW result, UINT bytes)
    { return caps (current->inputs, index, result, bytes); }
    static MMRESULT WINAPI outputCaps (UINT_PTR index, LPMIDIOUTCAPSW result, UINT bytes)
    { return caps (current->outputs, index, result, bytes); }

    static MMRESULT message (const std::vector<QueryDevice>& devices, UINT_PTR index,
                             UINT command, DWORD_PTR first, DWORD_PTR second)
    {
        if (index >= devices.size() || first == 0)
        {
            current->invalidCall = true;
            return MMSYSERR_INVALPARAM;
        }
        const auto& device = devices[index];
        if (command == DRV_QUERYDEVICEINTERFACESIZE && second == 0)
        {
            *reinterpret_cast<ULONG*> (first) = device.reportedBytes.value_or (
                static_cast<ULONG> ((device.identifier.size() + 1) * sizeof (wchar_t)));
            return device.sizeResult;
        }
        if (command == DRV_QUERYDEVICEINTERFACE && second == 512 * sizeof (wchar_t))
        {
            auto* destination = reinterpret_cast<wchar_t*> (first);
            if (device.unterminatedId)
                std::fill_n (destination, 512, L'X');
            else
            {
                std::fill_n (destination, 512, L'\0');
                std::copy_n (device.identifier.data(), std::min<std::size_t> (device.identifier.size(), 511),
                             destination);
            }
            return device.idResult;
        }
        current->invalidCall = true;
        return MMSYSERR_INVALPARAM;
    }

    static MMRESULT WINAPI inputMessage (HMIDIIN index, UINT command, DWORD_PTR first, DWORD_PTR second)
    { return message (current->inputs, reinterpret_cast<UINT_PTR> (index), command, first, second); }
    static MMRESULT WINAPI outputMessage (HMIDIOUT index, UINT command, DWORD_PTR first, DWORD_PTR second)
    { return message (current->outputs, reinterpret_cast<UINT_PTR> (index), command, first, second); }

    winmm::DeviceQueryApi api() const
    { return { inputCount, outputCount, inputCaps, outputCaps, inputMessage, outputMessage }; }
};
}

TEST_CASE ("WinMM native enumeration preserves Windows indices across capability failures", "[midi][winmm][issue-299]")
{
    QueryFixture fixture;
    fixture.inputs = { { L"In 0", L"input-0" }, { L"missing", L"input-1", MMSYSERR_NODRIVER },
                       { L"In 2", L"input-2" } };
    fixture.outputs = { { L"Out 0", L"output-0" }, { L"missing", L"output-1", MMSYSERR_BADDEVICEID },
                        { L"Out 2", L"output-2" } };
    for (auto direction : { winmm::Direction::Input, winmm::Direction::Output })
    {
        const auto devices = winmm::enumerateDevices (direction, fixture.api());
        REQUIRE (devices.size() == 2);
        REQUIRE (devices[0].deviceIndex == 0);
        REQUIRE (devices[1].deviceIndex == 2);
        REQUIRE (devices[1].info.identifier == (direction == winmm::Direction::Input ? "input-2" : "output-2"));
        REQUIRE (winmm::deviceIndexForIdentifier (devices, devices[1].info.identifier) == 2);
    }
    REQUIRE_FALSE (fixture.invalidCall);
}

TEST_CASE ("WinMM native enumeration applies fallback identifiers before duplicate decoration", "[midi][winmm][issue-299]")
{
    QueryFixture fixture;
    fixture.inputs = { { L"Keys", L"" }, { L"Keys", L"Keys" }, { L"Keys", L"Keys-2" } };
    const auto devices = winmm::enumerateDevices (winmm::Direction::Input, fixture.api());
    REQUIRE (devices.size() == 3);
    REQUIRE (devices[0].info.identifier == "Keys");
    REQUIRE (devices[1].info.identifier == "Keys-2");
    REQUIRE (devices[2].info.identifier == "Keys-2-2");
    REQUIRE (devices[0].info.name == "Keys");
    REQUIRE (devices[1].info.name == "Keys-2");
    REQUIRE (devices[2].info.name == "Keys-3");
    REQUIRE_FALSE (fixture.invalidCall);
}

TEST_CASE ("WinMM native queries convert bounded Unicode metadata", "[midi][winmm][issue-299]")
{
    QueryFixture fixture;
    fixture.outputs = { { L"\u00c9cho \U0001f3b9", L"\\\\?\\midi#\u00e9#\U0001f3b9" }, { L"", L"nameless" },
                        { L"bad", L"bad" } };
    fixture.outputs[2].unterminatedName = true;
    const auto devices = winmm::enumerateDevices (winmm::Direction::Output, fixture.api());
    REQUIRE (devices.size() == 2);
    REQUIRE (devices[0].info.name == u8"\u00c9cho \U0001f3b9");
    REQUIRE (devices[0].info.identifier == u8"\\\\?\\midi#\u00e9#\U0001f3b9");
    REQUIRE (devices[1].info.name.empty());
    REQUIRE (devices[1].info.identifier == "nameless");
    REQUIRE_FALSE (fixture.invalidCall);
}

TEST_CASE ("WinMM native queries bound unavailable or malformed interface metadata", "[midi][winmm][issue-299]")
{
    QueryFixture fixture;
    fixture.inputs = { { L"Keys", L"interface" } };
    auto& device = fixture.inputs.front();
    SECTION ("size query fails") { device.sizeResult = MMSYSERR_NOTSUPPORTED; }
    SECTION ("identifier query fails") { device.idResult = MMSYSERR_NOTSUPPORTED; }
    SECTION ("no interface") { device.reportedBytes = 0; }
    SECTION ("incomplete wide character") { device.reportedBytes = 1; }
    SECTION ("odd byte size") { device.reportedBytes = 3; }
    SECTION ("legacy size ceiling") { device.reportedBytes = 1024; }
    SECTION ("oversize metadata") { device.reportedBytes = 0xffffffffu; }
    SECTION ("missing terminator") { device.unterminatedId = true; }
    SECTION ("invalid UTF16") { device.identifier.assign (1, static_cast<wchar_t> (0xd800)); }
    SECTION ("largest accepted interface")
    {
        device.identifier.assign (510, L'x');
        const auto devices = winmm::enumerateDevices (winmm::Direction::Input, fixture.api());
        REQUIRE (devices.size() == 1);
        REQUIRE (devices[0].info.identifier == std::string (510, 'x'));
        REQUIRE_FALSE (fixture.invalidCall);
        return;
    }
    const auto devices = winmm::enumerateDevices (winmm::Direction::Input, fixture.api());
    REQUIRE (devices.size() == 1);
    REQUIRE (devices[0].info.identifier == "Keys");
    REQUIRE_FALSE (fixture.invalidCall);
}

TEST_CASE ("WinMM device lookup refuses missing or ambiguous identifiers", "[midi][winmm][issue-299]")
{
    QueryFixture fixture;
    REQUIRE (winmm::enumerateDevices (winmm::Direction::Input, fixture.api()).empty());
    fixture.inputs = { { L"Keys-2", L"" }, { L"Keys", L"" }, { L"Keys", L"" } };
    const auto devices = winmm::enumerateDevices (winmm::Direction::Input, fixture.api());
    REQUIRE (devices.size() == 3);
    REQUIRE (winmm::deviceIndexForIdentifier (devices, "Keys") == 1);
    REQUIRE_FALSE (winmm::deviceIndexForIdentifier (devices, "Keys-2").has_value());
    REQUIRE_FALSE (winmm::deviceIndexForIdentifier (devices, "keys").has_value());
    REQUIRE_FALSE (winmm::deviceIndexForIdentifier (devices, "absent").has_value());
    REQUIRE_FALSE (winmm::deviceIndexForIdentifier (devices, "").has_value());
    REQUIRE_FALSE (fixture.invalidCall);
}
#endif
