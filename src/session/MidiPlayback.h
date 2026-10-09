#pragma once

#include "MidiEvents.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace duskstudio
{
// A region's notes and controllers the way the audio thread schedules them:
// copied out when the region is published and sorted by time, so a block finds
// what falls inside it by binary search instead of reading every event the
// region holds. Immutable once built. The region's own vectors keep the order
// the editors give them, and an editor changing them in place touches nothing
// the audio thread reads.
struct MidiPlaybackRegion
{
    struct Note
    {
        std::int64_t startTick = 0;
        std::int64_t endTick   = 0;   // at or before startTick: ends where it starts
        std::uint8_t channel   = 0;   // 0..15
        std::uint8_t key       = 0;
        std::uint8_t velocity  = 0;
    };

    struct Release
    {
        std::int64_t endTick = 0;
        std::uint8_t channel = 0;
        std::uint8_t key     = 0;
    };

    struct Controller
    {
        std::int64_t tick       = 0;
        std::uint8_t channel    = 0;
        std::uint8_t controller = 0;
        std::uint8_t value      = 0;

        int key() const noexcept { return (int) channel * 128 + (int) controller; }
    };

    // One channel and controller's events: [begin, end) of byController.
    struct Lane
    {
        int           key   = 0;
        std::uint32_t begin = 0;
        std::uint32_t end   = 0;
    };

    static constexpr std::size_t kNoteChunk = 64;

    std::vector<Note>         notes;           // by startTick
    std::vector<std::int64_t> chunkLatestEnd;  // the latest endTick of each kNoteChunk notes
    std::vector<Release>      releases;        // notes that end after they start, by endTick
    std::vector<Controller>   controllers;     // by tick
    std::vector<Controller>   byController;    // by channel and controller, then tick
    std::vector<Lane>         lanes;
};

// Equal times keep the region's order, which is what decides between two
// events on one tick. Events the MIDI wire cannot carry (a channel outside
// 1..16, a key or controller outside 0..127) are left out.
inline MidiPlaybackRegion buildMidiPlayback (const std::vector<MidiNote>& notes,
                                             const std::vector<MidiCc>& ccs)
{
    const auto dataByte = [] (int v) { return (std::uint8_t) std::clamp (v, 0, 127); };
    MidiPlaybackRegion playback;

    playback.notes.reserve (notes.size());
    for (const auto& n : notes)
        if (n.channel >= 1 && n.channel <= 16 && n.noteNumber >= 0 && n.noteNumber < 128)
            playback.notes.push_back ({ n.startTick, n.startTick + n.lengthInTicks,
                                        (std::uint8_t) (n.channel - 1),
                                        (std::uint8_t) n.noteNumber, dataByte (n.velocity) });
    std::stable_sort (playback.notes.begin(), playback.notes.end(),
                      [] (const auto& a, const auto& b) { return a.startTick < b.startTick; });

    const auto chunk = MidiPlaybackRegion::kNoteChunk;
    playback.chunkLatestEnd.reserve ((playback.notes.size() + chunk - 1) / chunk);
    for (std::size_t first = 0; first < playback.notes.size(); first += chunk)
    {
        const auto last = std::min (first + chunk, playback.notes.size());
        auto latest = playback.notes[first].endTick;
        for (auto i = first + 1; i < last; ++i)
            latest = std::max (latest, playback.notes[i].endTick);
        playback.chunkLatestEnd.push_back (latest);
    }

    for (const auto& n : playback.notes)
        if (n.endTick > n.startTick)
            playback.releases.push_back ({ n.endTick, n.channel, n.key });
    std::stable_sort (playback.releases.begin(), playback.releases.end(),
                      [] (const auto& a, const auto& b) { return a.endTick < b.endTick; });

    playback.controllers.reserve (ccs.size());
    for (const auto& c : ccs)
        if (c.channel >= 1 && c.channel <= 16 && c.controller >= 0 && c.controller < 128)
            playback.controllers.push_back ({ c.atTick, (std::uint8_t) (c.channel - 1),
                                              (std::uint8_t) c.controller, dataByte (c.value) });
    std::stable_sort (playback.controllers.begin(), playback.controllers.end(),
                      [] (const auto& a, const auto& b) { return a.tick < b.tick; });

    playback.byController = playback.controllers;
    std::stable_sort (playback.byController.begin(), playback.byController.end(),
                      [] (const auto& a, const auto& b) { return a.key() < b.key(); });
    for (std::uint32_t i = 0; i < (std::uint32_t) playback.byController.size(); ++i)
    {
        const int key = playback.byController[i].key();
        if (playback.lanes.empty() || playback.lanes.back().key != key)
            playback.lanes.push_back ({ key, i, i });
        playback.lanes.back().end = i + 1;
    }
    return playback;
}

// Where a region sits on the timeline and whether it plays.
struct MidiRegionPlacement
{
    std::int64_t timelineStart   = 0;
    std::int64_t lengthInSamples = 0;
    std::int64_t lengthInTicks   = 0;
    bool         muted           = false;
};

// A track's MIDI timeline the way the audio thread plays it, one entry per
// region: its events and its placement. Immutable once published; every
// change, a drag included, publishes a new one. A region's events are shared
// with the timelines before it for as long as they stay the same, so a change
// that only moves regions, or a tempo change, copies and sorts no events.
//
// The index puts the regions that play (unmuted) in order of their start, so a
// block finds the few it overlaps without reading the rest: by start, a binary
// search; by end, kRegionChunk at a time, from the latest start and the longest
// length each chunk holds, which bound how late any of its regions ends.
struct MidiTimeline
{
    static constexpr std::size_t kRegionChunk = 64;

    std::vector<std::shared_ptr<const MidiPlaybackRegion>> playback;
    std::vector<MidiRegionPlacement>                       placement;   // playback.size() of them

    std::vector<std::uint32_t> byStart;               // unmuted regions, by timelineStart
    std::vector<std::int64_t>  startOf;               // their starts, ascending
    // Per kRegionChunk of byStart: the longest region in it, in samples and in
    // ticks, and over it and every chunk before it.
    std::vector<std::int64_t>  chunkLongestSamples, chunkLongestTicks;
    std::vector<std::int64_t>  upToLongestSamples, upToLongestTicks;
    // Each controller (channel * 128 + controller) an unmuted region sets, and
    // for each, holders[holdersFrom[k], holdersFrom[k + 1]): the positions in
    // byStart of the regions that set it, ascending.
    std::vector<int>           controllerKeys;
    std::vector<std::uint32_t> holdersFrom;
    std::vector<std::uint32_t> holders;

    std::size_t size() const noexcept { return playback.size(); }
    const MidiPlaybackRegion& events (std::size_t region) const noexcept { return *playback[region]; }
};

// Builds a timeline's index from its playback and placements. Message thread.
inline void indexMidiTimeline (MidiTimeline& timeline)
{
    timeline.byStart.clear();
    for (std::uint32_t i = 0; i < (std::uint32_t) timeline.size(); ++i)
        if (! timeline.placement[i].muted)
            timeline.byStart.push_back (i);
    std::stable_sort (timeline.byStart.begin(), timeline.byStart.end(), [&timeline] (std::uint32_t a, std::uint32_t b)
                      { return timeline.placement[a].timelineStart < timeline.placement[b].timelineStart; });

    const auto chunk = MidiTimeline::kRegionChunk;
    const auto count = timeline.byStart.size();
    timeline.startOf.resize (count);
    timeline.chunkLongestSamples.assign ((count + chunk - 1) / chunk, 0);
    timeline.chunkLongestTicks.assign ((count + chunk - 1) / chunk, 0);
    constexpr int kKeys = 16 * 128;
    std::vector<std::uint32_t> perKey (kKeys + 1, 0);
    for (std::size_t p = 0; p < count; ++p)
    {
        const auto region = timeline.byStart[p];
        const auto& place = timeline.placement[region];
        timeline.startOf[p] = place.timelineStart;
        auto& samples = timeline.chunkLongestSamples[p / chunk];
        auto& ticks = timeline.chunkLongestTicks[p / chunk];
        samples = std::max (samples, place.lengthInSamples);
        ticks = std::max (ticks, place.lengthInTicks);
        for (const auto& lane : timeline.events (region).lanes)
            ++perKey[(std::size_t) lane.key + 1];
    }

    timeline.controllerKeys.clear();
    timeline.holdersFrom.assign (1, 0);
    std::vector<std::uint32_t> slotOf (kKeys, 0);
    for (int key = 0; key < kKeys; ++key)
        if (perKey[(std::size_t) key + 1] > 0)
        {
            slotOf[(std::size_t) key] = timeline.holdersFrom.back();
            timeline.controllerKeys.push_back (key);
            timeline.holdersFrom.push_back (timeline.holdersFrom.back() + perKey[(std::size_t) key + 1]);
        }
    timeline.holders.resize (timeline.holdersFrom.back());
    for (std::size_t p = 0; p < count; ++p)
        for (const auto& lane : timeline.events (timeline.byStart[p]).lanes)
            timeline.holders[slotOf[(std::size_t) lane.key]++] = (std::uint32_t) p;
    timeline.upToLongestSamples = timeline.chunkLongestSamples;
    timeline.upToLongestTicks = timeline.chunkLongestTicks;
    for (std::size_t c = 1; c < timeline.upToLongestSamples.size(); ++c)
    {
        timeline.upToLongestSamples[c] = std::max (timeline.upToLongestSamples[c], timeline.upToLongestSamples[c - 1]);
        timeline.upToLongestTicks[c] = std::max (timeline.upToLongestTicks[c], timeline.upToLongestTicks[c - 1]);
    }
}
} // namespace duskstudio
