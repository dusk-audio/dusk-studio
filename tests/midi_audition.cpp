#include <catch2/catch_test_macros.hpp>

#include "engine/MidiAudition.h"
#include "foundation/MidiBuffer.h"
#include "support/AllocationObserver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using duskstudio::midi::AuditionEvent;
using duskstudio::midi::AuditionGate;
using duskstudio::midi::AuditionQueue;

namespace
{
std::vector<AuditionEvent> drainAll (AuditionQueue& queue)
{
    std::vector<AuditionEvent> out;
    queue.drain ([&out] (const AuditionEvent& event) { out.push_back (event); });
    return out;
}

bool isMessage (const AuditionEvent& event, int track, int status, int data1, int data2)
{
    return event.track == track && event.bytes[0] == status && event.bytes[1] == data1 && event.bytes[2] == data2;
}
} // namespace

TEST_CASE ("Audition queue delivers each note to its track in the order it was played",
           "[midi][audition]")
{
    AuditionQueue queue;
    REQUIRE (queue.noteOn (3, 1, 60, 100));
    REQUIRE (queue.noteOff (3, 1, 60));
    REQUIRE (queue.noteOn (3, 1, 61, 100));
    REQUIRE (queue.noteOn (7, 10, 36, 127));
    REQUIRE (queue.noteOff (3, 1, 61));

    const auto events = drainAll (queue);
    REQUIRE (events.size() == 5);
    CHECK (isMessage (events[0], 3, 0x90, 60, 100));
    CHECK (isMessage (events[1], 3, 0x80, 60, 0));
    CHECK (isMessage (events[2], 3, 0x90, 61, 100));
    CHECK (isMessage (events[3], 7, 0x99, 36, 127));
    CHECK (isMessage (events[4], 3, 0x80, 61, 0));
    CHECK (queue.heldCount() == 1);
    CHECK (queue.isHeld (7, 10, 36));
    CHECK (drainAll (queue).empty());

    SECTION ("velocity and channel stay in range")
    {
        REQUIRE (queue.noteOn (0, 0, 64, 0));
        REQUIRE (queue.noteOn (0, 17, 65, 300));
        const auto clamped = drainAll (queue);
        REQUIRE (clamped.size() == 2);
        CHECK (isMessage (clamped[0], 0, 0x90, 64, 1));
        CHECK (isMessage (clamped[1], 0, 0x9F, 65, 127));
        CHECK_FALSE (queue.noteOn (0, 1, 128, 100));
        CHECK_FALSE (queue.noteOn (-1, 1, 60, 100));
    }
}

TEST_CASE ("Audition queue always has room for a held note's note-off",
           "[midi][audition]")
{
    AuditionQueue queue;
    SECTION ("a glissando the audio thread never drains")
    {
        int taken = 0;
        int note = 0;
        bool holding = false;
        for (int step = 0; step < 4 * AuditionQueue::kCapacity; ++step)
        {
            if (holding) REQUIRE (queue.noteOff (0, 1, note));
            note = step % 128;
            holding = queue.noteOn (0, 1, note, 100);
            if (holding) ++taken;
        }
        CHECK (taken > 0);
        CHECK (taken < AuditionQueue::kCapacity);
        if (holding) REQUIRE (queue.noteOff (0, 1, note));
        CHECK (queue.heldCount() == 0);

        int ons = 0, offs = 0;
        for (const auto& event : drainAll (queue))
            ((event.bytes[0] & 0xF0) == 0x90 ? ons : offs)++;
        CHECK (ons == taken);
        CHECK (offs == taken);
    }
    SECTION ("a chord past what the queue can end")
    {
        int held = 0;
        for (int note = 0; note < 128 && queue.noteOn (1, 1, note, 100); ++note) ++held;
        CHECK (held == AuditionQueue::kMaxHeld);
        queue.releaseAll();
        int offs = 0;
        for (const auto& event : drainAll (queue))
            if ((event.bytes[0] & 0xF0) == 0x80) ++offs;
        CHECK (offs == held);
    }
    SECTION ("a note-on refused for want of room leaves nothing to end")
    {
        for (int i = 0; i < AuditionQueue::kCapacity; ++i)
            if (queue.noteOn (2, 1, 60, 100)) REQUIRE (queue.noteOff (2, 1, 60));
        CHECK_FALSE (queue.noteOn (2, 1, 60, 100));
        CHECK_FALSE (queue.isHeld (2, 1, 60));
        drainAll (queue);
        CHECK (queue.noteOn (2, 1, 60, 100));
    }
}

TEST_CASE ("Audition queue sends a note-off only for a note it holds", "[midi][audition]")
{
    AuditionQueue queue;
    CHECK_FALSE (queue.noteOff (0, 1, 60));
    REQUIRE (queue.noteOn (0, 1, 60, 100));
    CHECK_FALSE (queue.noteOn (0, 1, 60, 100));
    CHECK_FALSE (queue.noteOff (1, 1, 60));
    CHECK_FALSE (queue.noteOff (0, 2, 60));
    REQUIRE (queue.noteOn (0, 2, 60, 100));
    queue.releaseAll();
    CHECK_FALSE (queue.noteOff (0, 1, 60));
    CHECK (queue.heldCount() == 0);

    const auto events = drainAll (queue);
    REQUIRE (events.size() == 4);
    CHECK (isMessage (events[0], 0, 0x90, 60, 100));
    CHECK (isMessage (events[1], 0, 0x91, 60, 100));
    CHECK (isMessage (events[2], 0, 0x80, 60, 0));
    CHECK (isMessage (events[3], 0, 0x81, 60, 0));
}

TEST_CASE ("Audition queue renumbers queued and held notes when tracks move", "[midi][audition]")
{
    AuditionQueue queue;
    REQUIRE (queue.noteOn (2, 1, 60, 100));
    REQUIRE (queue.noteOn (5, 1, 62, 100));
    drainAll (queue);
    REQUIRE (queue.noteOff (2, 1, 60));
    REQUIRE (queue.noteOn (2, 1, 64, 100));

    const std::array<int, 8> oldToNew { 0, 1, 4, 3, 2, 5, 6, 7 };
    queue.renumberTracks ([&oldToNew] (int was) { return oldToNew[(std::size_t) was]; });

    CHECK (queue.isHeld (4, 1, 64));
    CHECK (queue.isHeld (5, 1, 62));
    CHECK_FALSE (queue.isHeld (2, 1, 64));
    REQUIRE (queue.noteOff (4, 1, 64));
    REQUIRE (queue.noteOff (5, 1, 62));

    const auto events = drainAll (queue);
    REQUIRE (events.size() == 4);
    CHECK (isMessage (events[0], 4, 0x80, 60, 0));
    CHECK (isMessage (events[1], 4, 0x90, 64, 100));
    CHECK (isMessage (events[2], 4, 0x80, 64, 0));
    CHECK (isMessage (events[3], 5, 0x80, 62, 0));
}

TEST_CASE ("Audition messages merge into their own track at the block's first sample",
           "[midi][audition]")
{
    const std::array<AuditionEvent, 4> block {
        AuditionEvent { 1, { 0x80, 60, 0 } }, AuditionEvent { 0, { 0x90, 48, 100 } },
        AuditionEvent { 1, { 0x90, 61, 100 } }, AuditionEvent { 2, { 0x90, 72, 100 } } };

    dusk::MidiBuffer track;
    track.reserveBytes (dusk::kMidiRoutingBlockBytes);
    const std::uint8_t scheduled[] { 0x80, 59, 0 };
    REQUIRE (track.addEvent (scheduled, 3, 0));
    REQUIRE (duskstudio::midi::appendAuditions (block.data(), (int) block.size(), 1, true, track));

    std::vector<std::array<int, 4>> seen;
    for (const auto message : track)
        seen.push_back ({ message.samplePosition, message.data[0], message.data[1], message.data[2] });
    REQUIRE (seen.size() == 3);
    CHECK (seen[0] == std::array<int, 4> { 0, 0x80, 59, 0 });
    CHECK (seen[1] == std::array<int, 4> { 0, 0x80, 60, 0 });
    CHECK (seen[2] == std::array<int, 4> { 0, 0x90, 61, 100 });

    SECTION ("a track that may not start notes still ends them")
    {
        dusk::MidiBuffer frozen;
        frozen.reserveBytes (dusk::kMidiRoutingBlockBytes);
        REQUIRE (duskstudio::midi::appendAuditions (block.data(), (int) block.size(), 1, false, frozen));
        std::vector<std::array<int, 3>> ended;
        for (const auto message : frozen)
            ended.push_back ({ message.data[0], message.data[1], message.data[2] });
        CHECK (ended == std::vector<std::array<int, 3>> { { 0x80, 60, 0 } });
    }

    dusk::MidiBuffer full;
    full.reserveBytes (dusk::MidiBuffer::recordBytes (3));
    CHECK_FALSE (duskstudio::midi::appendAuditions (block.data(), (int) block.size(), 1, true, full));
    CHECK (duskstudio::midi::appendAuditions (block.data(), (int) block.size(), 3, true, full));
}

TEST_CASE ("Audition handoff allocates nothing on either side", "[midi][audition][allocation]")
{
    AuditionQueue queue;
    std::array<AuditionEvent, AuditionQueue::kCapacity> block {};
    dusk::MidiBuffer track;
    track.reserveBytes (dusk::kMidiRoutingBlockBytes);
    AuditionGate gate;

    std::size_t producerAllocations = 0, consumerAllocations = 0;
    for (int pass = 0; pass < 3; ++pass)
    {
        {
            duskstudio::test::AllocationObserver observer;
            for (int note = 60; note < 72; ++note)
            {
                queue.noteOn (0, 1, note, 100);
                queue.noteOff (0, 1, note);
            }
            queue.noteOn (0, 1, 72, 100);
            queue.releaseAll();
            producerAllocations += observer.count();
        }
        int count = 0;
        bool merged = false;
        {
            duskstudio::test::AllocationObserver observer;
            track.clear();
            queue.drain ([&] (const AuditionEvent& event) noexcept
            {
                block[(std::size_t) count++] = event;
                gate.hear (event.bytes);
            });
            merged = duskstudio::midi::appendAuditions (block.data(), count, 0, true, track);
            gate.afterBlock (256, 0.0f, 48000.0);
            consumerAllocations += observer.count();
        }
        CHECK (count == 26);
        CHECK (merged);
    }
    CHECK (producerAllocations == 0);
    CHECK (consumerAllocations == 0);
}

TEST_CASE ("Audition gate keeps a stopped track open until the release dies away",
           "[midi][audition]")
{
    constexpr double rate = 48000.0;
    constexpr int block = 480;
    const std::array<std::uint8_t, 3> on60 { 0x90, 60, 100 }, off60 { 0x80, 60, 0 },
                                      on62 { 0x90, 62, 100 }, off62 { 0x80, 62, 0 };
    AuditionGate gate;
    CHECK_FALSE (gate.isOpen());

    gate.hear (on60);
    CHECK (gate.isOpen());
    for (int i = 0; i < 1000; ++i) gate.afterBlock (block, 0.0f, rate);
    CHECK (gate.isOpen());

    gate.hear (off60);
    gate.hear (on62);
    gate.afterBlock (block, 0.5f, rate);
    gate.hear (off62);
    CHECK (gate.heldNotes() == 0);

    SECTION ("a release that dies away closes the gate once it has been quiet")
    {
        for (int i = 0; i < 20; ++i) gate.afterBlock (block, 0.25f, rate);
        CHECK (gate.isOpen());
        for (int i = 0; i < 4; ++i) gate.afterBlock (block, 1.0e-6f, rate);
        CHECK (gate.isOpen());
        gate.afterBlock (block, 0.1f, rate);
        for (int i = 0; i < 4; ++i) gate.afterBlock (block, 1.0e-6f, rate);
        CHECK (gate.isOpen());
        gate.afterBlock (block, 1.0e-6f, rate);
        CHECK_FALSE (gate.isOpen());
    }
    SECTION ("an instrument that never goes quiet still closes it")
    {
        const int blocks = (int) (rate * AuditionGate::kMaxRingSeconds) / block;
        for (int i = 0; i < blocks - 1; ++i) gate.afterBlock (block, 0.5f, rate);
        CHECK (gate.isOpen());
        gate.afterBlock (block, 0.5f, rate);
        CHECK_FALSE (gate.isOpen());
    }
    SECTION ("a note-off for a note never heard is ignored")
    {
        gate.hear (off60);
        CHECK (gate.heldNotes() == 0);
        gate.hear (on60);
        CHECK (gate.heldNotes() == 1);
    }
}
