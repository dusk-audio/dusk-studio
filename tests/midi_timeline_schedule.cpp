#include <catch2/catch_test_macros.hpp>

#include "engine/MidiTimelineSchedule.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

// The timeline scheduler reads a region's time-sorted copy: a block costs what
// falls inside it, not what the region holds.

using namespace duskstudio;

namespace
{
constexpr double kRate = 48000.0;
constexpr float  kBpm = 120.0f;   // 50 samples a tick at 480 per quarter

struct Event
{
    std::uint8_t status, data1, data2;
    std::int64_t at;   // timeline sample
};

struct Timeline
{
    std::vector<MidiRegion> regions;
    std::vector<MidiPlaybackRegion> playback;
    midischedule::ControllerChase chase;

    void add (MidiRegion region)
    {
        playback.push_back (buildMidiPlayback (region.notes, region.ccs));
        regions.push_back (std::move (region));
    }

    midischedule::Outcome play (std::int64_t start, std::int64_t end, bool chased,
                                std::vector<Event>& out, int& scans)
    {
        return midischedule::scheduleSpan (
            regions, playback, nullptr, kRate, kBpm, { start, end, chased }, scans, chase,
            [&out, start] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t inSpan)
            {
                out.push_back ({ s, d1, d2, start + inSpan });
                return true;
            });
    }
};

MidiRegion regionOfTicks (std::int64_t ticks)
{
    MidiRegion region;
    region.lengthInTicks = ticks;
    region.lengthInSamples = ticksToSamples (ticks, kRate, kBpm);
    return region;
}
} // namespace

TEST_CASE ("a region dense with controllers still plays its notes, a block at a time",
           "[midi][schedule][regression]")
{
    // A long breath-controller take: one controller every tick for 100,000
    // ticks, more than a block may look at, with notes through it.
    Timeline timeline;
    auto region = regionOfTicks (100000);
    for (int tick = 0; tick < 100000; ++tick)
        region.ccs.push_back ({ 1, 2, tick & 0x7F, tick });
    for (int i = 0; i < 50; ++i)
        region.notes.push_back ({ 1, 60 + i % 12, 100, 1000 + i * 1000, 400 });
    timeline.add (std::move (region));

    constexpr int kBlock = 256;
    const auto end = ticksToSamples (60000, kRate, kBpm);
    int ons = 0, offs = 0, controllers = 0, mostScans = 0;
    for (std::int64_t at = 0; at < end; at += kBlock)
    {
        const auto blockEnd = std::min (at + kBlock, end);
        std::vector<Event> events;
        int scans = 32768;
        REQUIRE (timeline.play (at, blockEnd, false, events, scans) == midischedule::Outcome::complete);
        mostScans = std::max (mostScans, 32768 - scans);
        for (const auto& e : events)
        {
            REQUIRE (e.at >= at);
            REQUIRE (e.at < blockEnd);
            const int kind = e.status & 0xF0;
            ons += kind == 0x90;
            offs += kind == 0x80;
            controllers += kind == 0xB0;
        }
    }
    CHECK (ons == 50);
    CHECK (offs == 50);
    CHECK (controllers == 60000);
    // A block reads its few events and the searches to find them.
    CHECK (mostScans < 200);
}

TEST_CASE ("a chased span sends each controller's value and the notes held across its start",
           "[midi][schedule]")
{
    Timeline timeline;
    auto region = regionOfTicks (2000);
    region.ccs.push_back ({ 1, 7, 100, 0 });
    region.ccs.push_back ({ 1, 7, 50, 100 });
    region.ccs.push_back ({ 2, 1, 30, 150 });
    region.ccs.push_back ({ 1, 7, 90, 900 });           // after the span: not chased
    region.notes.push_back ({ 1, 60, 90, 50, 450 });    // held across tick 200
    region.notes.push_back ({ 1, 62, 90, 50, 100 });    // over before it
    timeline.add (std::move (region));

    const auto start = ticksToSamples (200, kRate, kBpm);
    std::vector<Event> events;
    int scans = 32768;
    REQUIRE (timeline.play (start, start + 256, true, events, scans) == midischedule::Outcome::complete);
    REQUIRE (events.size() == 3);
    CHECK ((events[0].status == 0xB0 && events[0].data1 == 7 && events[0].data2 == 50));
    CHECK ((events[1].status == 0xB1 && events[1].data1 == 1 && events[1].data2 == 30));
    CHECK ((events[2].status == 0x90 && events[2].data1 == 60 && events[2].data2 == 90));
    for (const auto& e : events)
        CHECK (e.at == start);

    // A controller exactly at the span's start is the value it starts with.
    Timeline exact;
    auto onStart = regionOfTicks (2000);
    onStart.ccs.push_back ({ 1, 7, 100, 0 });
    onStart.ccs.push_back ({ 1, 7, 20, 200 });
    exact.add (std::move (onStart));
    events.clear();
    REQUIRE (exact.play (start, start + 256, true, events, scans) == midischedule::Outcome::complete);
    REQUIRE (events.size() == 1);
    CHECK (events[0].data2 == 20);
}

TEST_CASE ("at one sample a controller leads, a note-off comes before the next note-on",
           "[midi][schedule]")
{
    // The note on key 60 that ends at tick 100 must not cut off the one that
    // starts there, whatever order the region keeps them in.
    Timeline timeline;
    auto region = regionOfTicks (1000);
    region.notes.push_back ({ 1, 60, 100, 100, 100 });
    region.notes.push_back ({ 1, 60, 100, 0, 100 });
    region.ccs.push_back ({ 1, 74, 64, 100 });
    region.notes.push_back ({ 1, 64, 100, 100, 0 });    // no length: off right behind its on
    timeline.add (std::move (region));

    const auto at = ticksToSamples (100, kRate, kBpm);
    std::vector<Event> events;
    int scans = 32768;
    REQUIRE (timeline.play (at, at + 1, false, events, scans) == midischedule::Outcome::complete);
    REQUIRE (events.size() == 5);
    CHECK ((events[0].status == 0xB0 && events[0].data1 == 74));
    CHECK ((events[1].status == 0x80 && events[1].data1 == 60));
    CHECK ((events[2].status == 0x90 && events[2].data1 == 60));
    CHECK ((events[3].status == 0x90 && events[3].data1 == 64));
    CHECK ((events[4].status == 0x80 && events[4].data1 == 64));
}

TEST_CASE ("a span the scan budget cannot finish says so", "[midi][schedule]")
{
    Timeline timeline;
    for (int r = 0; r < 100; ++r)
    {
        auto region = regionOfTicks (100);
        region.timelineStart = r * 10000;
        region.notes.push_back ({ 1, 60, 100, 0, 10 });
        timeline.add (std::move (region));
    }
    std::vector<Event> events;
    int scans = 50;
    CHECK (timeline.play (0, 256, false, events, scans) == midischedule::Outcome::scanBudgetSpent);
    CHECK (scans == 0);

    // Muted regions and events the wire cannot carry play nothing.
    Timeline quiet;
    auto muted = regionOfTicks (100);
    muted.muted = true;
    muted.notes.push_back ({ 1, 60, 100, 0, 10 });
    quiet.add (std::move (muted));
    auto invalid = regionOfTicks (100);
    invalid.notes.push_back ({ 17, 60, 100, 0, 10 });
    invalid.ccs.push_back ({ 1, 128, 1, 0 });
    quiet.add (std::move (invalid));
    events.clear();
    scans = 32768;
    CHECK (quiet.play (0, 256, true, events, scans) == midischedule::Outcome::complete);
    CHECK (events.empty());
}

TEST_CASE ("an event the block cannot take stops the span", "[midi][schedule]")
{
    Timeline timeline;
    auto region = regionOfTicks (100);
    region.notes.push_back ({ 1, 60, 100, 0, 1 });
    region.notes.push_back ({ 1, 62, 100, 0, 1 });
    timeline.add (std::move (region));

    int taken = 0, scans = 32768;
    const auto outcome = midischedule::scheduleSpan (
        timeline.regions, timeline.playback, nullptr, kRate, kBpm, { 0, 256, false }, scans,
        timeline.chase, [&taken] (std::uint8_t, std::uint8_t, std::uint8_t, std::int64_t)
        { return ++taken < 2; });
    CHECK (outcome == midischedule::Outcome::eventRefused);
    CHECK (taken == 2);
}
