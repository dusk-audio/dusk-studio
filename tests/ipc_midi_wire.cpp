#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/MidiWire.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace midiwire = duskstudio::ipc::midiwire;

namespace
{
struct Event
{
    std::vector<std::uint8_t> bytes;
    int sample = 0;

    bool operator== (const Event& other) const { return bytes == other.bytes && sample == other.sample; }
};

std::vector<Event> decode (const std::vector<std::uint8_t>& wire, std::uint32_t numBytes)
{
    std::vector<Event> events;
    midiwire::forEachEvent (wire.data(), numBytes, [&events] (const std::uint8_t* data, int length, int sample)
    {
        events.push_back ({ std::vector<std::uint8_t> (data, data + length), sample });
    });
    return events;
}

std::vector<std::uint8_t> sysex (std::size_t length)
{
    std::vector<std::uint8_t> bytes (length);
    for (std::size_t i = 0; i < length; ++i)
        bytes[i] = (std::uint8_t) (i & 0x7F);
    bytes.front() = 0xF0;
    bytes.back() = 0xF7;
    return bytes;
}
} // namespace

// Both ends of the plug-in host's shared-memory block read MIDI through this
// codec. The child once copied each event into a 256-byte buffer and stopped
// reading at the first longer one, so a long sysex took every note-off after it.
TEST_CASE ("MIDI wire: a long sysex in the middle of a block leaves every event around it whole",
           "[ipc][midi]")
{
    const std::vector<Event> block {
        { { 0x90, 60, 100 }, 0 },
        { { 0xB0, 64, 127 }, 3 },
        { sysex (300), 10 },
        { { 0x80, 60, 0 }, 20 },
        { { 0xE0, 0x00, 0x40 }, 200 },
        { { 0x80, 62, 0 }, 255 },
    };

    SECTION ("every event comes back intact and in order")
    {
        std::vector<std::uint8_t> wire (1024);
        std::uint32_t written = 0;
        for (const auto& e : block)
            REQUIRE (midiwire::append (wire.data(), written, wire.size(), e.bytes.data(), (int) e.bytes.size(), e.sample));
        REQUIRE (decode (wire, written) == block);
    }

    SECTION ("an event that does not fit is left out and the ones after it still go")
    {
        // One byte short of the sysex on top of the two events before it.
        std::size_t capacity = 0;
        for (std::size_t i = 0; i <= 2; ++i)
            capacity += midiwire::kHeaderBytes + block[i].bytes.size();
        capacity -= 1;
        std::vector<std::uint8_t> wire (capacity);
        std::uint32_t written = 0;
        std::vector<Event> sent;
        for (const auto& e : block)
            if (midiwire::append (wire.data(), written, wire.size(), e.bytes.data(), (int) e.bytes.size(), e.sample))
                sent.push_back (e);
        auto expected = block;
        expected.erase (expected.begin() + 2);
        REQUIRE (sent == expected);
        REQUIRE (decode (wire, written) == expected);
    }
}
