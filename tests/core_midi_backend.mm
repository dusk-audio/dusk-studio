#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/midi/CoreMidiBackend.h"
#include "engine/midi/CoreMidiProtocol.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

using namespace duskstudio::midi;
using Catch::Matchers::WithinAbs;

namespace
{
MIDIClientRef testClient()
{
    static const auto client = []
    {
        MIDIClientRef value = 0;
        MIDIClientCreate (CFSTR ("Dusk CoreMIDI Tests"), nullptr, nullptr, &value);
        return value;
    }();
    return client;
}

struct Fixture
{
    MIDIEndpointRef source = 0;
    MIDIEndpointRef destination = 0;
    std::string sourceName;
    std::string destinationName;

    Fixture()
    {
        static int sequence = 0;
        const auto suffix = std::to_string (getpid()) + " " + std::to_string (++sequence);
        sourceName = "Dusk CoreMIDI Test Source " + suffix;
        destinationName = "Dusk CoreMIDI Test Destination " + suffix;
        const auto sourceString = CFStringCreateWithCString (nullptr, sourceName.c_str(), kCFStringEncodingUTF8);
        const auto sourceStatus = MIDISourceCreate (testClient(), sourceString, &source);
        CFRelease (sourceString);
        REQUIRE (sourceStatus == noErr);
        const auto destinationString = CFStringCreateWithCString (nullptr, destinationName.c_str(), kCFStringEncodingUTF8);
        const auto target = source;
        const auto destinationStatus = MIDIDestinationCreateWithBlock (testClient(), destinationString, &destination,
            ^(const MIDIPacketList* packets, void*) { MIDIReceived (target, packets); });
        CFRelease (destinationString);
        REQUIRE (destinationStatus == noErr);
    }

    ~Fixture()
    {
        if (destination != 0) MIDIEndpointDispose (destination);
        if (source != 0) MIDIEndpointDispose (source);
    }

    void emit (const std::vector<std::uint8_t>& bytes, MIDITimeStamp ticks = 0)
    {
        alignas(MIDIPacketList) std::array<std::uint8_t, 2048> storage {};
        auto* list = reinterpret_cast<MIDIPacketList*> (storage.data());
        REQUIRE (MIDIPacketListAdd (list, storage.size(), MIDIPacketListInit (list), ticks, bytes.size(), bytes.data()) != nullptr);
        REQUIRE (MIDIReceived (source, list) == noErr);
    }
};

std::string findId (const std::vector<BackendDeviceInfo>& devices, const std::string& name)
{
    for (const auto& device : devices)
        if (device.name == name) return device.identifier;
    return {};
}

struct Sink
{
    struct Event { std::vector<std::uint8_t> bytes; double timeMs; std::string id; };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;

    IMidiInputBackend::Receiver receiver()
    {
        return [this] (const std::string& id, const std::uint8_t* bytes, int count, double timeMs)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            events.push_back ({ { bytes, bytes + count }, timeMs, id });
            changed.notify_all();
        };
    }

    bool wait (std::size_t count)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (lock, std::chrono::seconds (3), [&] { return events.size() >= count; });
    }
};

bool pumpUntil (const std::function<bool()>& ready)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds (3);
    while (! ready() && std::chrono::steady_clock::now() < end)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false);
    return ready();
}

SInt32 negativeId (MIDIEndpointRef endpoint)
{
    static SInt32 next = -1000000000 - getpid();
    MIDIObjectRef existing = 0;
    MIDIObjectType type = kMIDIObjectType_Other;
    while (MIDIObjectFindByUniqueID (--next, &existing, &type) == noErr) {}
    REQUIRE (MIDIObjectSetIntegerProperty (endpoint, kMIDIPropertyUniqueID, next) == noErr);
    return next;
}

void setConnections (MIDIEndpointRef endpoint, const std::vector<SInt32>& ids)
{
    std::vector<std::uint8_t> bytes;
    for (const auto id : ids)
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back (static_cast<std::uint8_t> (static_cast<std::uint32_t> (id) >> shift));
    const auto data = CFDataCreate (nullptr, bytes.data(), static_cast<CFIndex> (bytes.size()));
    const auto status = MIDIObjectSetDataProperty (endpoint, kMIDIPropertyConnectionUniqueID, data);
    CFRelease (data);
    REQUIRE (status == noErr);
}
}

TEST_CASE ("CoreMIDI virtual endpoints carry channel system and SysEx messages", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    auto output = makeCoreMidiOutputBackend();
    const auto inputId = findId (input->enumerate(), fixture.sourceName);
    const auto outputId = findId (output->enumerate(), fixture.destinationName);
    REQUIRE_FALSE (inputId.empty());
    REQUIRE_FALSE (outputId.empty());
    REQUIRE (input->enable (inputId));
    input->setReceiver (sink.receiver());
    REQUIRE (output->open (outputId));
    input->start();
    const std::vector<std::vector<std::uint8_t>> expected {
        { 0x90, 60, 100 }, { 0x80, 60, 0 }, { 0xb0, 1, 64 }, { 0xc0, 2 }, { 0xd0, 33 }, { 0xe0, 0, 64 },
        { 0xf1, 0x17 }, { 0xf2, 1, 2 }, { 0xf3, 3 }, { 0xf6 }, { 0xf8 }, { 0xfa }, { 0xfb }, { 0xfc }, { 0xfe }, { 0xff },
        { 0xf0, 0x7f, 1, 1, 1, 0x60, 0, 0, 0, 0xf7 }
    };
    dusk::MidiBuffer events;
    for (const auto& bytes : expected) REQUIRE (events.addEvent (bytes.data(), static_cast<int> (bytes.size()), 0));
    REQUIRE (output->send (outputId, events, backendClockMs(), 48000.0));
    REQUIRE (sink.wait (expected.size()));
    input->stop();
    REQUIRE (sink.events.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        REQUIRE (sink.events[i].bytes == expected[i]);
        REQUIRE (sink.events[i].id == inputId);
    }
    output->closeAll();
    REQUIRE_FALSE (output->isOpen (outputId));
    REQUIRE_FALSE (output->send (outputId, events, backendClockMs(), 48000.0));
}

TEST_CASE ("CoreMIDI preserves packet time and reassembles SysEx across packets", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    input->setReceiver (sink.receiver());
    REQUIRE (input->enable (findId (input->enumerate(), fixture.sourceName)));
    input->start();
    mach_timebase_info_data_t timebase {};
    REQUIRE (mach_timebase_info (&timebase) == KERN_SUCCESS);
    const auto tenMilliseconds = static_cast<std::uint64_t> (10000000.0 * timebase.denom / timebase.numer);
    const auto timestamp = mach_absolute_time() - tenMilliseconds;
    const double expectedMs = backendClockMs() - 10.0;
    fixture.emit ({ 0x90, 60, 100 }, timestamp);
    fixture.emit ({ 0xf0, 0x7f, 1, 1 }, timestamp);
    fixture.emit ({ 1, 0x60, 0, 0, 0, 0xf7 });
    REQUIRE (sink.wait (2));
    input->stop();
    REQUIRE_THAT (sink.events[0].timeMs, WithinAbs (expectedMs, 2.0));
    REQUIRE_THAT (sink.events[1].timeMs, WithinAbs (expectedMs, 2.0));
    REQUIRE (sink.events[1].bytes == std::vector<std::uint8_t> { 0xf0, 0x7f, 1, 1, 1, 0x60, 0, 0, 0, 0xf7 });
}

TEST_CASE ("CoreMIDI schedules queued sample offsets in time order", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    auto output = makeCoreMidiOutputBackend();
    input->setReceiver (sink.receiver());
    REQUIRE (input->enable (findId (input->enumerate(), fixture.sourceName)));
    const auto outputId = findId (output->enumerate(), fixture.destinationName);
    REQUIRE (output->open (outputId));
    input->start();
    dusk::MidiBuffer events;
    const std::uint8_t note[] { 0x90, 60, 100 };
    REQUIRE (events.addEvent (note, 3, 960));
    const std::uint8_t off[] { 0x80, 60, 0 };
    REQUIRE (events.addEvent (off, 3, 0));
    const double baseMs = backendClockMs() - 10.0;
    REQUIRE (output->send (outputId, events, baseMs, 48000.0));
    REQUIRE (sink.wait (2));
    input->stop();
    REQUIRE (sink.events[0].bytes == std::vector<std::uint8_t> { 0x80, 60, 0 });
    REQUIRE (sink.events[1].bytes == std::vector<std::uint8_t> { 0x90, 60, 100 });
    REQUIRE (sink.events[1].timeMs >= baseMs + 19.0);
    REQUIRE (sink.events[1].timeMs < baseMs + 100.0);
}

TEST_CASE ("CoreMIDI stop fences an admitted callback and restart clears partial messages", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto input = makeCoreMidiInputBackend();
    input->setReceiver ([&] (const std::string&, const std::uint8_t*, int, double)
    {
        entered.set_value();
        released.wait();
    });
    REQUIRE (input->enable (findId (input->enumerate(), fixture.sourceName)));
    input->start();
    fixture.emit ({ 0x90, 60, 100 });
    REQUIRE (entered.get_future().wait_for (std::chrono::seconds (3)) == std::future_status::ready);
    auto stopped = std::async (std::launch::async, [&] { input->stop(); });
    const auto status = stopped.wait_for (std::chrono::milliseconds (20));
    release.set_value();
    REQUIRE (status == std::future_status::timeout);
    REQUIRE (stopped.wait_for (std::chrono::seconds (3)) == std::future_status::ready);

    Sink sink;
    input->setReceiver (sink.receiver());
    input->start();
    fixture.emit ({ 0xf0, 1, 2 });
    input->stop();
    fixture.emit ({ 3, 0xf7 });
    input->start();
    fixture.emit ({ 0x80, 60, 0 });
    REQUIRE (sink.wait (1));
    input->stop();
    REQUIRE (sink.events.size() == 1);
    REQUIRE (sink.events[0].bytes == std::vector<std::uint8_t> { 0x80, 60, 0 });
}

TEST_CASE ("CoreMIDI migrates signed and connected IDs and rejects ambiguous aliases", "[coremidi-native][issue-298]")
{
    Fixture first, second, connected, duplicate;
    const auto firstId = negativeId (first.source);
    const auto secondId = negativeId (second.source);
    auto input = makeCoreMidiInputBackend();
    REQUIRE (input->migrateIdentifier (std::to_string (firstId)) == coremidi::identifier (firstId));
    const std::vector<BackendDeviceInfo> inputs { { first.sourceName, std::to_string (firstId) } };
    REQUIRE (coreMidiLegacyInputIdentifier (coremidi::identifier (firstId), inputs) == std::to_string (firstId));
    REQUIRE (coreMidiLegacyInputIdentifier (coremidi::identifier (firstId), {}).empty());
    const auto outputId = negativeId (first.destination);
    auto output = makeCoreMidiOutputBackend();
    REQUIRE (output->migrateIdentifier (std::to_string (outputId)) == coremidi::identifier (outputId));
    const std::vector<BackendDeviceInfo> outputs { { first.destinationName, std::to_string (outputId) } };
    REQUIRE (coreMidiLegacyOutputIdentifier (coremidi::identifier (outputId), outputs) == std::to_string (outputId));
    REQUIRE (input->migrateIdentifier ("missing").empty());
    setConnections (connected.source, { firstId, secondId });
    SInt32 connectedId = 0;
    REQUIRE (MIDIObjectGetIntegerProperty (connected.source, kMIDIPropertyUniqueID, &connectedId) == noErr);
    const auto legacy = std::to_string (firstId) + ", " + std::to_string (secondId);
    REQUIRE (input->migrateIdentifier (legacy) == coremidi::identifier (connectedId));
    const std::vector<BackendDeviceInfo> connectedInputs { { connected.sourceName, legacy } };
    REQUIRE (coreMidiLegacyInputIdentifier (coremidi::identifier (connectedId), connectedInputs) == legacy);
    setConnections (duplicate.source, { firstId, secondId });
    REQUIRE (input->migrateIdentifier (legacy).empty());
    REQUIRE (coreMidiLegacyInputIdentifier (coremidi::identifier (connectedId), connectedInputs).empty());
}

TEST_CASE ("CoreMIDI reports endpoint hotplug without reporting its own input ports", "[coremidi-native][issue-298]")
{
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    std::atomic<int> changes { 0 };
    input->setReceiver (sink.receiver());
    input->setDeviceChangeHandler ([&] { changes.fetch_add (1); });
    input->start();
    auto fixture = std::make_unique<Fixture>();
    REQUIRE (pumpUntil ([&] { return changes.load() > 0; }));
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    const auto beforePort = changes.load();
    const auto identifier = findId (input->enumerate(), fixture->sourceName);
    REQUIRE (input->enable (identifier));
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    REQUIRE (changes.load() == beforePort);
    fixture->emit ({ 0x90, 60, 100 });
    REQUIRE (sink.wait (1));
    input->disableAll();
    REQUIRE (input->enable (identifier));
    fixture->emit ({ 0x80, 60, 0 });
    REQUIRE (sink.wait (2));
    input->disableAll();
    const auto beforeRemoval = changes.load();
    fixture.reset();
    REQUIRE (pumpUntil ([&] { return changes.load() > beforeRemoval; }));
    input->stop();
    const auto afterStop = changes.load();
    fixture = std::make_unique<Fixture>();
    REQUIRE (input->enable (findId (input->enumerate(), fixture->sourceName)));
    fixture->emit ({ 0x90, 61, 100 });
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    REQUIRE (changes.load() == afterStop);
    {
        const std::lock_guard<std::mutex> lock (sink.mutex);
        REQUIRE (sink.events.size() == 2);
    }
    input->start();
    fixture->emit ({ 0x80, 61, 0 });
    REQUIRE (sink.wait (3));
    input->stop();
    REQUIRE (sink.events.size() == 3);
}

TEST_CASE ("CoreMIDI future output does not delay a different destination", "[coremidi-native][issue-298]")
{
    Fixture first, second;
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    auto output = makeCoreMidiOutputBackend();
    input->setReceiver (sink.receiver());
    const auto firstInput = findId (input->enumerate(), first.sourceName);
    const auto secondInput = findId (input->enumerate(), second.sourceName);
    REQUIRE (input->enable (firstInput));
    REQUIRE (input->enable (secondInput));
    const auto firstOutput = findId (output->enumerate(), first.destinationName);
    const auto secondOutput = findId (output->enumerate(), second.destinationName);
    REQUIRE (output->open (firstOutput));
    REQUIRE (output->open (secondOutput));
    input->start();
    dusk::MidiBuffer events;
    const std::uint8_t note[] { 0x90, 60, 100 };
    REQUIRE (events.addEvent (note, 3, 0));
    REQUIRE (output->send (firstOutput, events, backendClockMs() + 500.0, 48000.0));
    REQUIRE (output->send (secondOutput, events, backendClockMs(), 48000.0));
    REQUIRE (sink.wait (2));
    input->stop();
    REQUIRE (sink.events[0].id == secondInput);
    REQUIRE (sink.events[1].id == firstInput);
}

TEST_CASE ("CoreMIDI close cancels owned pending output and preserves another sender", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    auto first = makeCoreMidiOutputBackend();
    auto second = makeCoreMidiOutputBackend();
    input->setReceiver (sink.receiver());
    REQUIRE (input->enable (findId (input->enumerate(), fixture.sourceName)));
    const auto outputId = findId (first->enumerate(), fixture.destinationName);
    REQUIRE (first->open (outputId));
    REQUIRE (second->open (outputId));
    input->start();
    dusk::MidiBuffer cancelled, retained;
    const std::uint8_t note[] { 0x90, 60, 100 }, off[] { 0x80, 60, 0 };
    REQUIRE (cancelled.addEvent (note, 3, 0));
    REQUIRE (retained.addEvent (off, 3, 0));
    const auto now = backendClockMs();
    REQUIRE (first->send (outputId, cancelled, now + 200.0, 48000.0));
    REQUIRE (second->send (outputId, retained, now + 250.0, 48000.0));
    first->closeAll();
    REQUIRE (first->open (outputId));
    REQUIRE (sink.wait (1));
    input->stop();
    REQUIRE (sink.events.size() == 1);
    REQUIRE (sink.events[0].bytes == std::vector<std::uint8_t> { 0x80, 60, 0 });
}

TEST_CASE ("CoreMIDI direct output survives immediate close or destruction", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    Sink sink;
    auto input = makeCoreMidiInputBackend();
    auto output = makeCoreMidiOutputBackend();
    input->setReceiver (sink.receiver());
    REQUIRE (input->enable (findId (input->enumerate(), fixture.sourceName)));
    const auto outputId = findId (output->enumerate(), fixture.destinationName);
    REQUIRE (output->open (outputId));
    input->start();
    dusk::MidiBuffer events;
    const std::uint8_t off[] { 0x80, 60, 0 }, note[] { 0x90, 60, 100 };
    REQUIRE (events.addEvent (off, 3, 0));
    REQUIRE (events.addEvent (note, 3, 48000));
    REQUIRE (output->send (outputId, events, backendClockMs(), 48000.0));
    SECTION ("closeAll") { output->closeAll(); }
    SECTION ("destruction") { output.reset(); }
    REQUIRE (sink.wait (1));
    input->stop();
    REQUIRE (sink.events.size() == 1);
    REQUIRE (sink.events[0].bytes == std::vector<std::uint8_t> { 0x80, 60, 0 });
}

TEST_CASE ("CoreMIDI refresh cannot reopen a removed destination", "[coremidi-native][issue-298]")
{
    Fixture fixture;
    auto output = makeCoreMidiOutputBackend();
    const auto outputId = findId (output->enumerate(), fixture.destinationName);
    REQUIRE (output->open (outputId));
    REQUIRE (MIDIEndpointDispose (fixture.destination) == noErr);
    fixture.destination = 0;
    output->closeAll();
    REQUIRE (findId (output->enumerate(), fixture.destinationName).empty());
    REQUIRE_FALSE (output->open (outputId));
}

#pragma clang diagnostic pop
