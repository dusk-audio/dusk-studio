#include <catch2/catch_test_macros.hpp>
#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/JuceCompat.h"
#include "foundation/MidiBuffer.h"
#include "support/AllocationObserver.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

// Filling JUCE's MIDI buffer from a block that is already in time order.

namespace
{
struct Event
{
    std::vector<std::uint8_t> bytes;
    int samplePosition = 0;
};

// JUCE's buffer, counting the inserts that search it for their place.
struct CountingMidiBuffer
{
    juce::MidiBuffer buffer;
    juce::Array<juce::uint8>& data = buffer.data;
    int searchingInserts = 0;

    CountingMidiBuffer() = default;
    CountingMidiBuffer (const CountingMidiBuffer&) = delete;
    CountingMidiBuffer& operator= (const CountingMidiBuffer&) = delete;

    void clear() noexcept { buffer.clear(); }
    bool addEvent (const void* bytes, int numBytes, int samplePosition)
    {
        ++searchingInserts;
        return buffer.addEvent (bytes, numBytes, samplePosition);
    }
};

juce::MidiBuffer insertedOneByOne (const std::vector<Event>& events)
{
    juce::MidiBuffer buffer;
    for (const auto& event : events)
        buffer.addEvent (event.bytes.data(), (int) event.bytes.size(), event.samplePosition);
    return buffer;
}

void fillInOrder (CountingMidiBuffer& destination, const std::vector<Event>& events)
{
    duskstudio::juce_compat::OrderedMidiFill<CountingMidiBuffer> ordered (destination);
    for (const auto& event : events)
        ordered.add (event.bytes.data(), (int) event.bytes.size(), event.samplePosition);
}
} // namespace

TEST_CASE ("An ordered MIDI fill leaves the bytes JUCE's own inserts leave", "[hosting][midi][juce]")
{
    std::vector<Event> events;
    int position = 0;
    const auto add = [&] (std::vector<std::uint8_t> bytes)
    {
        events.push_back ({ std::move (bytes), position });
        if (events.size() % 3 == 0) ++position;
    };

    SECTION ("every status byte, cut short and run long")
    {
        for (int status = 0; status < 256; ++status)
            for (std::size_t length = 1; length <= 4; ++length)
            {
                std::vector<std::uint8_t> bytes { (std::uint8_t) status, 0x12, 0x34, 0x56 };
                bytes.resize (length);
                add (bytes);
            }
    }

    SECTION ("system exclusive, whole, open and with bytes past its end")
    {
        add ({ 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 });
        add ({ 0xF0, 0x43, 0x10, 0x4C });
        add ({ 0xF0, 0x41, 0xF7, 0x90, 0x3C, 0x64 });
        add ({ 0xF7, 0x01, 0x02, 0xF7, 0x03 });
        add ({ 0xF0 });
        add ({ 0xF7 });
        std::vector<std::uint8_t> large (3000, 0x55);
        large.front() = 0xF0;
        large.back() = 0xF7;
        add (large);
    }

    CountingMidiBuffer filled;
    fillInOrder (filled, events);
    const auto expected = insertedOneByOne (events);
    REQUIRE (filled.data == expected.data);
    REQUIRE (filled.buffer.getNumEvents() == expected.getNumEvents());
}

TEST_CASE ("An ordered MIDI fill searches JUCE's buffer only for an event out of order", "[hosting][midi][juce]")
{
    std::mt19937 random (1029);
    std::uniform_int_distribution<int> step (-6, 4);
    std::vector<Event> events;
    int position = 100;
    int latest = std::numeric_limits<int>::min();
    int outOfOrder = 0;
    for (int i = 0; i < 600; ++i)
    {
        position = std::max (0, position + step (random));
        events.push_back ({ { 0x90, (std::uint8_t) (i % 128), 100 }, position });
        if (position < latest) ++outOfOrder;
        latest = std::max (latest, position);
    }
    REQUIRE (outOfOrder > 0);

    CountingMidiBuffer filled;
    fillInOrder (filled, events);
    CHECK (filled.searchingInserts == outOfOrder);
    REQUIRE (filled.data == insertedOneByOne (events).data);
}

TEST_CASE ("A dense block at one sample fills JUCE's MIDI buffer in one pass", "[hosting][midi][juce][allocation]")
{
    // The standard VST3 cap. Inserted one at a time, each of these walks every
    // event before it: 2048 * 2047 / 2 records visited for the block.
    static constexpr int kMessages = 2048;
    dusk::MidiBuffer block;
    block.reserveBytes (dusk::kMidiRoutingBlockBytes);
    const std::array<std::uint8_t, 3> controller { 0xB0, 1, 100 };
    for (int i = 0; i < kMessages; ++i)
        REQUIRE (block.addEvent (controller.data(), 3, 0));

    CountingMidiBuffer filled;
    filled.buffer.ensureSize (dusk::kMidiRoutingBlockBytes);
    const auto* storage = filled.data.begin();
    std::size_t allocations = 0;
    {
        duskstudio::test::AllocationObserver observer;
        duskstudio::juce_compat::copyMidiInOrder (block, filled);
        allocations = observer.count();
    }
    CHECK (allocations == 0);
    CHECK (filled.data.begin() == storage);
    CHECK (filled.searchingInserts == 0);
    REQUIRE (filled.buffer.getNumEvents() == kMessages);

    juce::MidiBuffer expected;
    for (const auto meta : block)
        expected.addEvent (meta.data, meta.numBytes, meta.samplePosition);
    REQUIRE (filled.data == expected.data);
}
