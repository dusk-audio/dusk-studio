#include <catch2/catch_test_macros.hpp>
#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/hosting/MidiFit.h"
#include "engine/hosting/StandardVst3Midi.h"
#include "support/AllocationObserver.h"

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

TEST_CASE ("standard VST3 panic expansion preserves bounded note releases", "[midi][hosting][panic][allocation]")
{
    duskstudio::hosting::StandardVst3Midi adapter;
    std::vector<Sent> sent;
    sent.reserve (duskstudio::hosting::StandardVst3Midi::kMaxMessages);
    const auto render = [&] (const dusk::MidiBuffer& block, bool refuse = false)
    {
        sent.clear();
        std::size_t allocations = 0;
        {
            duskstudio::test::AllocationObserver observer;
            adapter.deliver (block, [&] (const std::uint8_t* data, int bytes, int at)
            {
                if (refuse) return false;
                sent.push_back ({ data[0], data[1], bytes >= 3 ? data[2] : std::uint8_t { 0 }, bytes, at });
                return true;
            });
            allocations = observer.count();
        }
        REQUIRE (allocations == 0);
        REQUIRE (sent.size() <= duskstudio::hosting::StandardVst3Midi::kMaxMessages);
    };
    const auto offs = [&] (int channel, int key)
    {
        int count = 0;
        for (const auto& message : sent)
            if (message.status == (0x80 | channel) && message.data1 == key) ++count;
        return count;
    };

    SECTION ("a panic releases counted onsets only on its own channel and at its sample")
    {
        dusk::MidiBuffer block;
        add (block, 0x90, 60, 100);
        add (block, 0x90, 60, 100);
        add (block, 0x91, 64, 100);
        add (block, 0x80, 60, 0);
        render (block);
        block.clear();
        const std::array<std::uint8_t, 3> panic { 0xB0, 123, 0 }, next { 0x90, 65, 100 };
        REQUIRE (block.addEvent (panic.data(), 3, 17));
        REQUIRE (block.addEvent (next.data(), 3, 18));
        render (block);
        REQUIRE (sent.size() == 3);
        CHECK (offs (0, 60) == 1);
        CHECK (offs (1, 64) == 0);
        CHECK (sent[1].samplePosition == 17);
        CHECK (sent[2].samplePosition == 18);
        block.clear();
        REQUIRE (duskstudio::midi::emitTransportStopReset (block, 0));
        render (block);
        CHECK (offs (0, 65) == 1);
        CHECK (offs (1, 64) == 1);
        CHECK (offs (0, 60) == 0);
    }

    SECTION ("a panic backlog drains in empty blocks and excludes intervening onsets")
    {
        dusk::MidiBuffer block;
        for (int note = 0; note < 1024; ++note) add (block, 0x90, 60, 100);
        for (int pass = 0; pass < 4; ++pass) render (block);
        block.clear();
        REQUIRE (duskstudio::midi::emitTransportStopReset (block, 0));
        render (block);
        int released = offs (0, 60);
        CHECK (released < 4096);
        block.clear();
        add (block, 0x90, 61, 100);
        for (int pass = 0; pass < 4 && released < 4096; ++pass)
        {
            render (block);
            released += offs (0, 60);
            CHECK_FALSE (carries (sent, 0x90, 61, 100));
            CHECK (carries (sent, 0xB0, 64, 0));
        }
        REQUIRE (released == 4096);
        render (block);
        CHECK (carries (sent, 0x90, 61, 100));
        block.clear();
        REQUIRE (duskstudio::midi::emitTransportStopReset (block, 0));
        render (block);
        CHECK (offs (0, 61) == 1);
        CHECK (offs (0, 60) == 0);
    }

    SECTION ("a full destination cannot discard a release permanently")
    {
        dusk::MidiBuffer block;
        add (block, 0x92, 62, 100);
        render (block);
        block.clear();
        add (block, 0x82, 62, 0);
        render (block, true);
        REQUIRE (sent.empty());
        block.clear();
        render (block);
        CHECK (offs (2, 62) == 1);
        CHECK (carries (sent, 0xB2, 64, 0));
    }

    SECTION ("dense ordinary events preserve a trailing release")
    {
        dusk::MidiBuffer block;
        add (block, 0x90, 60, 100);
        render (block);
        block.clear();
        for (int point = 0; point < 4000; ++point) add (block, 0xB0, 1, 100);
        add (block, 0x80, 60, 0);
        render (block);
        CHECK (offs (0, 60) == 1);
        block.clear();
        add (block, 0xB0, 123, 0);
        render (block);
        CHECK (offs (0, 60) == 0);
    }

    SECTION ("in-place legacy rewriting preserves the allocation across empty blocks and reprepare")
    {
        juce::MidiBuffer block;
        block.ensureSize (dusk::kMidiRoutingBlockBytes);
        adapter.prepare();
        const auto rewrite = [&]
        {
            std::size_t allocations = 0;
            const auto* storage = block.data.begin();
            {
                duskstudio::test::AllocationObserver observer;
                adapter.rewrite (block);
                allocations = observer.count();
            }
            CHECK (allocations == 0);
            CHECK (block.data.begin() == storage);
            CHECK (block.getNumEvents() <= (int) adapter.kMaxMessages);
        };
        const std::array<std::uint8_t, 3> onset { 0x90, 60, 100 }, panic { 0xB0, 123, 0 };
        for (int pass = 0; pass < 4; ++pass)
        {
            block.clear();
            for (int note = 0; note < 1024; ++note) block.addEvent (onset.data(), 3, 0);
            rewrite();
        }
        adapter.prepare();
        block.clear();
        block.addEvent (panic.data(), 3, 0);
        int released = 0;
        for (int pass = 0; pass < 4; ++pass)
        {
            rewrite();
            for (const auto event : block)
                if (event.numBytes == 3 && event.data[0] == 0x80 && event.data[1] == 60) ++released;
            block.clear();
        }
        CHECK (released == 4096);
    }

    SECTION ("a new instance starts with no inherited note ownership")
    {
        dusk::MidiBuffer block;
        add (block, 0x90, 60, 100);
        render (block);
        adapter.reset();
        block.clear();
        add (block, 0xB0, 123, 0);
        render (block);
        REQUIRE (sent.size() == 1);
        CHECK (offs (0, 60) == 0);
    }
}
