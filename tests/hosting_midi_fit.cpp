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
};

void add (dusk::MidiBuffer& block, std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
{
    const std::array<std::uint8_t, 3> bytes { status, data1, data2 };
    block.addEvent (bytes.data(), (int) bytes.size(), 0);
}

std::vector<Sent> deliver (const dusk::MidiBuffer& block, std::size_t room)
{
    std::vector<Sent> sent;
    duskstudio::hosting::deliverWithinRoom (
        block, room, [] (const std::uint8_t*, int) { return std::size_t { 1 }; },
        [&sent] (const std::uint8_t* data, int, int) { sent.push_back ({ data[0], data[1], data[2] }); });
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
    using duskstudio::hosting::releasesNotes;
    const auto releases = [] (std::uint8_t controller, std::uint8_t value)
    {
        const std::array<std::uint8_t, 3> bytes { 0xB3, controller, value };
        return releasesNotes (bytes.data(), (int) bytes.size());
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
