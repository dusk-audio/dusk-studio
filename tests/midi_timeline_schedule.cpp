#include <catch2/catch_test_macros.hpp>

#include "engine/MidiTimelineSchedule.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>
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
    std::unique_ptr<MidiTimeline> published = buildMidiTimeline ({});
    midischedule::ControllerChase chase;

    void add (MidiRegion region)
    {
        regions.push_back (std::move (region));
        published = buildMidiTimeline (regions);
    }

    midischedule::Outcome play (std::int64_t start, std::int64_t end, bool chased,
                                std::vector<Event>& out, int& scans)
    {
        return midischedule::scheduleSpan (
            *published, nullptr, kRate, kBpm, { start, end, chased }, scans, chase,
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
    // A hundred regions all sounding in the first block.
    Timeline timeline;
    for (int r = 0; r < 100; ++r)
    {
        auto region = regionOfTicks (100);
        region.timelineStart = r;
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
        *timeline.published, nullptr, kRate, kBpm, { 0, 256, false }, scans,
        timeline.chase, [&taken] (std::uint8_t, std::uint8_t, std::uint8_t, std::int64_t)
        { return ++taken < 2; });
    CHECK (outcome == midischedule::Outcome::eventRefused);
    CHECK (taken == 2);
}

TEST_CASE ("a region plays nothing past its end, and the notes sounding there end there",
           "[midi][schedule][regression]")
{
    // 400 ticks long. One note runs past the end, one ends on it, and a note
    // and a controller come after it, in the block the end falls in.
    Timeline timeline;
    auto region = regionOfTicks (400);
    region.notes.push_back ({ 1, 60, 100, 0, 1000 });
    region.notes.push_back ({ 1, 62, 100, 100, 300 });
    region.notes.push_back ({ 1, 64, 100, 402, 100 });
    region.ccs.push_back ({ 1, 1, 64, 403 });
    timeline.add (std::move (region));
    const auto end = ticksToSamples (400, kRate, kBpm);

    const auto playThrough = [&timeline] (std::int64_t to, std::int64_t block)
    {
        std::vector<Event> all;
        for (std::int64_t at = 0; at < to; at += block)
        {
            int scans = 32768;
            REQUIRE (timeline.play (at, std::min (at + block, to), false, all, scans)
                     == midischedule::Outcome::complete);
        }
        return all;
    };
    const auto check = [end] (const std::vector<Event>& events)
    {
        int offs60 = 0, offs62 = 0;
        for (const auto& e : events)
        {
            CHECK (e.data1 != 64);
            CHECK (e.status != 0xB0);
            if (e.status == 0x80 && e.data1 == 60) { ++offs60; CHECK (e.at == end); }
            if (e.status == 0x80 && e.data1 == 62) { ++offs62; CHECK (e.at == end); }
        }
        CHECK (offs60 == 1);
        CHECK (offs62 == 1);
    };

    SECTION ("the end inside a block")  { check (playThrough (end + 5000, 256)); }
    SECTION ("the end on a block boundary") { check (playThrough (end + 5000, end / 8)); }

    SECTION ("a chase after the end sends nothing of the region")
    {
        std::vector<Event> events;
        int scans = 32768;
        REQUIRE (timeline.play (end + 1000, end + 1256, true, events, scans) == midischedule::Outcome::complete);
        CHECK (events.empty());
    }
}

TEST_CASE ("a changed timeline ends only the notes it no longer ends itself",
           "[midi][schedule]")
{
    using midischedule::NoteCount;
    Timeline timeline;
    auto region = regionOfTicks (2000);
    region.notes.push_back ({ 1, 60, 100, 0, 1500 });   // still sounds at tick 1000
    region.notes.push_back ({ 1, 62, 100, 0, 1000 });   // ends right at tick 1000
    timeline.add (std::move (region));
    auto muted = regionOfTicks (2000);
    muted.muted = true;
    muted.notes.push_back ({ 1, 65, 100, 0, 1500 });
    timeline.add (std::move (muted));
    auto over = regionOfTicks (500);                     // ended before tick 1000
    over.notes.push_back ({ 2, 67, 100, 0, 1500 });
    timeline.add (std::move (over));

    // What the instrument was sent: key 60 twice, 62 once, 64, 65 and 67 on
    // channel 2 once each.
    NoteCount sounding, held;
    for (const int key : { 60, 60, 62, 64, 65 })
        sounding.hear (0x90, (std::uint8_t) key, 100);
    sounding.hear (0x91, 67, 100);
    sounding.hear (0x90, 70, 0);   // a velocity-0 on is an off, of nothing held
    REQUIRE (sounding.total == 6);

    std::vector<Event> offs;
    int scans = 32768;
    const auto at = ticksToSamples (1000, kRate, kBpm);
    REQUIRE (midischedule::releaseStranded (*timeline.published, nullptr, kRate, kBpm, at,
                                            scans, sounding, held,
                                            [&offs] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t)
                                            {
                                                offs.push_back ({ s, d1, d2, 0 });
                                                return true;
                                            })
             == midischedule::Outcome::complete);

    std::vector<int> released;
    for (const auto& e : offs)
    {
        CHECK ((e.status & 0xF0) == 0x80);
        released.push_back ((e.status & 0x0F) * 128 + e.data1);
    }
    std::sort (released.begin(), released.end());
    CHECK (released == std::vector<int> { 60, 64, 65, 128 + 67 });
    CHECK (sounding.total == 2);
    CHECK (sounding.count[NoteCount::keyOf (0, 60)] == 1);
    CHECK (sounding.count[NoteCount::keyOf (0, 62)] == 1);

    SECTION ("nothing sounding reads nothing")
    {
        sounding.clear();
        int untouched = 7;
        CHECK (midischedule::releaseStranded (*timeline.published, nullptr, kRate, kBpm, at,
                                              untouched, sounding, held,
                                              [] (std::uint8_t, std::uint8_t, std::uint8_t, std::int64_t)
                                              { return true; })
               == midischedule::Outcome::complete);
        CHECK (untouched == 7);
    }
}

TEST_CASE ("a region moved in place plays from where it is, with the events it was published with",
           "[midi][schedule][regression]")
{
    // A drag moves the region in the message thread's own copy and hands the
    // audio thread the new position through editedInPlace(), which publishes
    // it with the events the region already had: neither copied nor re-sorted.
    MidiRegionSnapshot snapshot;
    auto region = regionOfTicks (400);
    region.notes.push_back ({ 1, 60, 100, 0, 100 });
    snapshot.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    // Holds what it reads the way the audio callback does.
    const SnapshotReadScope reading;
    const auto* published = snapshot.read();
    const auto edits = snapshot.edits();

    const auto movedTo = ticksToSamples (1000, kRate, kBpm);
    snapshot.currentMutable().front().timelineStart = movedTo;
    snapshot.currentMutable().front().notes.front().noteNumber = 72;

    SECTION ("the move is not heard until it is handed over")
    {
        CHECK (snapshot.read() == published);
        CHECK (published->placement[0].timelineStart == 0);
    }

    SECTION ("handed over, it plays from there")
    {
        snapshot.editedInPlace();
        const auto* timeline = snapshot.read();
        REQUIRE (timeline != published);
        CHECK (timeline->playback[0] == published->playback[0]);
        CHECK (snapshot.edits() == edits + 1);
        CHECK (snapshot.generation() == 2);

        midischedule::ControllerChase chase;
        std::vector<Event> events;
        int scans = 32768;
        REQUIRE (midischedule::scheduleSpan (*timeline, nullptr, kRate, kBpm, { 0, movedTo, false }, scans, chase,
                                             [&events] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t at)
                                             {
                                                 events.push_back ({ s, d1, d2, at });
                                                 return true;
                                             })
                 == midischedule::Outcome::complete);
        CHECK (events.empty());

        REQUIRE (midischedule::scheduleSpan (*timeline, nullptr, kRate, kBpm, { movedTo, movedTo + 256, false }, scans,
                                             chase,
                                             [&events, movedTo] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2,
                                                                 std::int64_t at)
                                             {
                                                 events.push_back ({ s, d1, d2, movedTo + at });
                                                 return true;
                                             })
                 == midischedule::Outcome::complete);
        REQUIRE (events.size() == 1);
        CHECK (events[0].status == 0x90);
        CHECK (events[0].data1 == 60);   // the published note, not the edit
        CHECK (events[0].at == movedTo);
    }
}

TEST_CASE ("a note a latency rise lands whole on the block's first sample is dropped, its note-off kept",
           "[midi][schedule][regression]")
{
    // The span starts 512 samples before the sample the block's first one
    // plays: all of [0, 512] lands there together.
    constexpr std::int64_t kCollapse = 512;
    Timeline timeline;
    auto region = regionOfTicks (100);
    region.notes.push_back ({ 1, 60, 100, 1, 4 });    // [50, 250): wholly inside
    region.notes.push_back ({ 1, 62, 100, 2, 20 });   // [100, 1100): ends after it
    region.notes.push_back ({ 1, 64, 100, 3, 0 });    // no length, inside
    timeline.add (std::move (region));
    auto cut = regionOfTicks (2);                      // ends at 100, inside
    cut.timelineStart = 1;
    cut.notes.push_back ({ 1, 66, 100, 0, 50 });
    timeline.add (std::move (cut));

    std::vector<Event> events;
    int scans = 32768;
    midischedule::Span span { 0, 1024, false, kCollapse };
    REQUIRE (midischedule::scheduleSpan (*timeline.published, nullptr, kRate, kBpm, span, scans, timeline.chase,
                                         [&events] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t at)
                                         {
                                             events.push_back ({ s, d1, d2, at });
                                             return true;
                                         })
             == midischedule::Outcome::complete);
    const auto count = [&events] (std::uint8_t status, std::uint8_t key)
    {
        return std::count_if (events.begin(), events.end(), [&] (const Event& e)
                              { return e.status == status && e.data1 == key; });
    };
    CHECK (count (0x90, 60) == 0);
    CHECK (count (0x80, 60) == 1);
    CHECK (count (0x90, 62) == 1);
    CHECK (count (0x90, 64) == 0);
    CHECK (count (0x80, 64) == 0);
    CHECK (count (0x90, 66) == 0);
    CHECK (count (0x80, 66) == 1);

    SECTION ("a chase does not hold a note that ends inside the stretch")
    {
        Timeline held;
        auto across = regionOfTicks (100);
        across.notes.push_back ({ 1, 67, 100, 0, 8 });    // [0, 400), held across 200
        across.notes.push_back ({ 1, 69, 100, 0, 30 });   // [0, 1500), past it
        held.add (std::move (across));
        events.clear();
        midischedule::Span chased { 200, 1224, true, 200 + kCollapse };
        REQUIRE (midischedule::scheduleSpan (*held.published, nullptr, kRate, kBpm, chased, scans, held.chase,
                                             [&events] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t at)
                                             {
                                                 events.push_back ({ s, d1, d2, at });
                                                 return true;
                                             })
                 == midischedule::Outcome::complete);
        CHECK (count (0x90, 67) == 0);
        CHECK (count (0x90, 69) == 1);
    }
}

namespace
{
using Played = std::vector<std::tuple<std::int64_t, std::uint8_t, std::uint8_t, std::uint8_t>>;

Played playThrough (const MidiTimeline& timeline, const TempoMap* map, std::int64_t to, std::int64_t block,
                    int& mostScans)
{
    Played played;
    midischedule::ControllerChase chase;
    for (std::int64_t at = 0; at < to; at += block)
    {
        int scans = 32768;
        const auto outcome = midischedule::scheduleSpan (
            timeline, map, kRate, kBpm, { at, std::min (at + block, to), false }, scans, chase,
            [&played, at] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t inSpan)
            {
                played.emplace_back (at + inSpan, s, d1, d2);
                return true;
            });
        REQUIRE (outcome == midischedule::Outcome::complete);
        mostScans = std::max (mostScans, 32768 - scans);
    }
    std::sort (played.begin(), played.end());
    return played;
}
} // namespace

TEST_CASE ("regions out of order, overlapping, muted or long play as each would alone",
           "[midi][schedule]")
{
    // The index finds a block's regions by start and by how late they can end;
    // whatever order the regions are kept in, each must play exactly what it
    // plays on a timeline of its own.
    std::minstd_rand random (7);
    const auto pick = [&random] (int below) { return (int) (random() % (unsigned) below); };
    std::vector<MidiRegion> regions;
    for (int r = 0; r < 300; ++r)
    {
        auto region = regionOfTicks (pick (10) == 0 ? 20000 : 40 + pick (400));
        region.timelineStart = (std::int64_t) pick (2000000);
        region.muted = pick (12) == 0;
        for (int n = 0; n < 1 + pick (5); ++n)
            region.notes.push_back ({ 1 + pick (2), 36 + pick (48), 90, (std::int64_t) pick (500), (std::int64_t) pick (300) });
        for (int c = 0; c < pick (3); ++c)
            region.ccs.push_back ({ 1, 1 + pick (3), pick (128), (std::int64_t) pick (500) });
        regions.push_back (std::move (region));
    }

    TempoMap map;
    map.setPoints ({ { 0, 100.0f }, { 700000, 160.0f }, { 1500000, 90.0f } });
    for (const TempoMap* tempo : { (const TempoMap*) nullptr, (const TempoMap*) &map })
    {
        INFO ((tempo != nullptr ? "with a tempo map" : "at constant tempo"));
        const auto whole = buildMidiTimeline (regions);
        const std::int64_t to = 2000000 + ticksToSamples (20000, kRate, 90.0f) * 2;
        int mostScans = 0;
        const auto together = playThrough (*whole, tempo, to, 4096, mostScans);

        Played alone;
        for (const auto& region : regions)
        {
            int ignored = 0;
            const auto one = buildMidiTimeline ({ region });
            const auto played = playThrough (*one, tempo, to, 4096, ignored);
            alone.insert (alone.end(), played.begin(), played.end());
        }
        std::sort (alone.begin(), alone.end());
        CHECK (together.size() == alone.size());
        CHECK (together == alone);
    }
}

TEST_CASE ("a track of 10,000 regions plays a block on a fraction of its scan budget, anywhere on it",
           "[midi][schedule][regression]")
{
    // An eighth-note region every eighth note for about 20 minutes at 120 bpm,
    // each with two notes and a controller. Every pass of a block once read
    // every region, which spent the budget and reset the instrument every block.
    std::vector<MidiRegion> regions;
    const auto spacing = ticksToSamples (240, kRate, kBpm);
    for (int r = 0; r < 10000; ++r)
    {
        auto region = regionOfTicks (240);
        region.timelineStart = (std::int64_t) r * spacing;
        region.notes.push_back ({ 1, 60, 90, 0, 100 });
        region.notes.push_back ({ 1, 64, 90, 120, 100 });
        region.ccs.push_back ({ 1, 1, r % 128, 0 });
        regions.push_back (std::move (region));
    }
    const auto timeline = buildMidiTimeline (regions);
    midischedule::ControllerChase chase;

    for (const int at : { 0, 5000, 9990 })
    {
        CAPTURE (at);
        const auto from = (std::int64_t) at * spacing;
        int ons = 0, mostScans = 0;
        for (std::int64_t start = from; start < from + 8 * spacing; start += 256)
        {
            int scans = 32768;
            REQUIRE (midischedule::scheduleSpan (*timeline, nullptr, kRate, kBpm, { start, start + 256, false },
                                                 scans, chase,
                                                 [&ons] (std::uint8_t s, std::uint8_t, std::uint8_t, std::int64_t)
                                                 {
                                                     ons += (s & 0xF0) == 0x90;
                                                     return true;
                                                 })
                     == midischedule::Outcome::complete);
            mostScans = std::max (mostScans, 32768 - scans);
        }
        CHECK (ons == 16);
        CHECK (mostScans < 1000);

        // After a locate the block chases: every region before it holds a
        // controller, and the newest of them decides its value.
        std::vector<Event> chased;
        int scans = 32768;
        REQUIRE (midischedule::scheduleSpan (*timeline, nullptr, kRate, kBpm, { from + 10, from + 266, true },
                                             scans, chase,
                                             [&chased] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t)
                                             {
                                                 chased.push_back ({ s, d1, d2, 0 });
                                                 return true;
                                             })
                 == midischedule::Outcome::complete);
        REQUIRE_FALSE (chased.empty());
        CHECK (chased.front().status == 0xB0);
        CHECK (chased.front().data2 == at % 128);
    }
}

TEST_CASE ("a chase sends each controller the latest value any region set before it",
           "[midi][schedule]")
{
    // Controllers set once long ago, regions that reach past later ones, and
    // regions published in no particular order: the chase must find, for each
    // controller, what a read of every region would.
    std::minstd_rand random (11);
    const auto pick = [&random] (int below) { return (int) (random() % (unsigned) below); };
    std::vector<MidiRegion> regions;
    for (int r = 0; r < 2000; ++r)
    {
        auto region = regionOfTicks (pick (40) == 0 ? 40000 : 50 + pick (300));
        region.timelineStart = (std::int64_t) pick (40000000);
        region.muted = pick (20) == 0;
        const int controllers = r < 5 ? 1 : pick (3);
        for (int c = 0; c < controllers; ++c)
            region.ccs.push_back ({ 1 + pick (2), r < 5 ? 7 : 1 + pick (4), pick (128), (std::int64_t) pick (60000) });
        regions.push_back (std::move (region));
    }
    const auto timeline = buildMidiTimeline (regions);
    midischedule::ControllerChase chase;

    for (int round = 0; round < 40; ++round)
    {
        const std::int64_t at = (std::int64_t) pick (42000000);
        CAPTURE (at);
        // What a read of every region finds. A controller two regions set on
        // one sample is left out: the order of the read decides that one.
        std::map<int, std::pair<std::int64_t, int>> latest;
        std::set<int> tied;
        for (const auto& region : regions)
        {
            if (region.muted) continue;
            const auto end = region.timelineStart + region.lengthInSamples;
            for (const auto& c : region.ccs)
            {
                const auto when = region.timelineStart + ticksToSamples (c.atTick, kRate, kBpm);
                if (when >= std::min (at, end)) continue;
                const int key = (c.channel - 1) * 128 + c.controller;
                const auto found = latest.find (key);
                if (found == latest.end() || when > found->second.first)
                {
                    latest[key] = { when, c.value };
                    tied.erase (key);
                }
                else if (when == found->second.first)
                    tied.insert (key);
            }
        }

        std::map<int, int> sent;
        int scans = 32768;
        REQUIRE (midischedule::scheduleSpan (*timeline, nullptr, kRate, kBpm, { at, at + 1, true }, scans, chase,
                                             [&sent] (std::uint8_t s, std::uint8_t d1, std::uint8_t d2, std::int64_t inSpan)
                                             {
                                                 if ((s & 0xF0) == 0xB0 && inSpan == 0)
                                                     sent[(s & 0x0F) * 128 + d1] = d2;
                                                 return true;
                                             })
                 == midischedule::Outcome::complete);
        for (const auto& [key, value] : latest)
        {
            if (tied.count (key) > 0) continue;
            CAPTURE (key);
            REQUIRE (sent.count (key) == 1);
            CHECK (sent[key] == value.second);
        }
    }
}
