#include <catch2/catch_test_macros.hpp>

#include "engine/hosting/MidiFit.h"

#include <array>
#include <cstdint>
#include <vector>

// What a plug-in host keeps when a block's MIDI does not fit its room.

namespace
{
struct Sent
{
    std::uint8_t status, data1, data2;
    int numBytes, samplePosition;
};

void add (dusk::MidiBuffer& block, std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    const std::array<std::uint8_t, 3> bytes { status, data1, data2 };
    block.addEvent (bytes.data(), (status & 0xF0) == 0xD0 ? 2 : 3, 0);
}

std::vector<Sent> deliver (const dusk::MidiBuffer& block, std::size_t room)
{
    std::vector<Sent> sent;
    duskstudio::hosting::deliverWithinRoom (
        block, room, [] (const std::uint8_t*, int) { return std::size_t { 1 }; },
        [&sent] (const std::uint8_t* data, int numBytes, int samplePosition)
        { sent.push_back ({ data[0], data[1], numBytes >= 3 ? data[2] : std::uint8_t { 0 }, numBytes, samplePosition }); });
    return sent;
}

bool carries (const std::vector<Sent>& sent, std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    for (const auto& s : sent)
        if (s.status == status && s.data1 == data1 && s.data2 == data2)
            return true;
    return false;
}
} // namespace

TEST_CASE ("every pedal coming up counts as a release", "[midi][hosting]")
{
    using duskstudio::hosting::endsNotesOrExpression;
    const auto releases = [] (std::uint8_t controller, std::uint8_t value)
    {
        const std::array<std::uint8_t, 3> bytes { 0xB3, controller, value };
        return endsNotesOrExpression (bytes.data(), (int) bytes.size());
    };
    for (const std::uint8_t pedal : { 64, 66, 69 })
    {
        CAPTURE ((int) pedal);
        CHECK (releases (pedal, 0));
        CHECK (releases (pedal, 63));
        CHECK_FALSE (releases (pedal, 64));
        CHECK_FALSE (releases (pedal, 127));
    }
    CHECK_FALSE (releases (67, 0));   // soft pedal holds nothing
    CHECK (releases (123, 0));
}

TEST_CASE ("a block over its host's room keeps every pedal's release",
           "[midi][hosting][regression]")
{
    SECTION ("neutral expression survives an ordinary event flood")
    {
        dusk::MidiBuffer stop;
        REQUIRE (duskstudio::midi::emitTransportStopReset (stop, 17));
        const auto expected = deliver (stop, 96);
        REQUIRE (expected.size() == 96);
        dusk::MidiBuffer block;
        for (int i = 0; i < 100; ++i) add (block, 0x90, 60, 100);
        for (const auto event : stop) REQUIRE (block.addEvent (event.data, event.numBytes, event.samplePosition));
        const auto sent = deliver (block, 96);
        REQUIRE (sent.size() == expected.size());
        for (std::size_t i = 0; i < sent.size(); ++i)
        {
            CHECK (sent[i].status == expected[i].status);
            CHECK (sent[i].data1 == expected[i].data1);
            CHECK (sent[i].data2 == expected[i].data2);
            CHECK (sent[i].numBytes == expected[i].numBytes);
            CHECK (sent[i].samplePosition == 17);
        }
        return;
    }
    SECTION ("an overflowing release block retains the complete Stop packet")
    {
        dusk::MidiBuffer block;
        REQUIRE (duskstudio::midi::emitTransportStopReset (block, 0));
        for (int i = 0; i < 97; ++i) add (block, 0x80, 60, 0);
        const auto sent = deliver (block, 96);
        REQUIRE (sent.size() == 96);
        for (int channel = 0; channel < 16; ++channel)
        {
            CHECK (carries (sent, (std::uint8_t) (0xB0 | channel), 121, 0));
            CHECK (carries (sent, (std::uint8_t) (0xE0 | channel), 0, 64));
            CHECK (carries (sent, (std::uint8_t) (0xD0 | channel), 0, 0));
        }
        for (const auto& event : sent)
        {
            CHECK (event.samplePosition == 0);
            CHECK (event.numBytes == ((event.status & 0xF0) == 0xD0 ? 2 : 3));
        }
        return;
    }
    SECTION ("an overflowing release block without Stop keeps the narrow reset")
    {
        dusk::MidiBuffer block;
        for (int i = 0; i < 97; ++i) add (block, 0x80, 60, 0);
        const auto sent = deliver (block, 96);
        REQUIRE (sent.size() == (std::size_t) duskstudio::midi::kHangingResetMessageCount);
        for (const auto& event : sent)
        {
            CHECK ((event.status & 0xF0) == 0xB0);
            CHECK (event.data1 != 121);
        }
        return;
    }
    SECTION ("nonneutral expression remains expendable")
    {
        dusk::MidiBuffer block;
        add (block, 0xE5, 127, 127);
        add (block, 0xD5, 90, 0);
        add (block, 0x85, 60, 0);
        const auto sent = deliver (block, 1);
        REQUIRE (sent.size() == 1);
        CHECK (sent.front().status == 0x85);
        return;
    }
    SECTION ("pedal releases leave remaining room for ordinary events") {}
    // Sustain, sostenuto and hold 2 all come up, with more ordinary events
    // than the room leaves beside them.
    dusk::MidiBuffer block;
    add (block, 0x90, 60, 100);
    add (block, 0xB0, 66, 127);
    add (block, 0xB0, 1, 20);
    add (block, 0xB0, 1, 30);
    add (block, 0xB0, 66, 0);
    add (block, 0xB0, 69, 10);
    add (block, 0xB0, 64, 0);
    add (block, 0x80, 60, 0);

    const auto sent = deliver (block, 5);
    REQUIRE (sent.size() == 5);
    CHECK (carries (sent, 0xB0, 66, 0));
    CHECK (carries (sent, 0xB0, 69, 10));
    CHECK (carries (sent, 0xB0, 64, 0));
    CHECK (carries (sent, 0x80, 60, 0));
    CHECK (carries (sent, 0x90, 60, 100));   // the room left over goes in block order
}
