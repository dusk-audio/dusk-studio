#pragma once

#include "MidiEvents.h"

#include <algorithm>
#include <atomic>
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

// A placement the message thread changes while the audio thread plays it, so a
// region dragged along the timeline plays from where it is without a publish.
// Each field is atomic on its own: a block that reads across a change can pair
// one field's old value with another's new one, which the next block's edit
// count catches.
class LiveMidiPlacement
{
public:
    void store (const MidiRegionPlacement& p) noexcept
    {
        timelineStart.store (p.timelineStart, std::memory_order_relaxed);
        lengthInSamples.store (p.lengthInSamples, std::memory_order_relaxed);
        lengthInTicks.store (p.lengthInTicks, std::memory_order_relaxed);
        muted.store (p.muted, std::memory_order_relaxed);
    }

    MidiRegionPlacement load() const noexcept
    {
        return { timelineStart.load (std::memory_order_relaxed),
                 lengthInSamples.load (std::memory_order_relaxed),
                 lengthInTicks.load (std::memory_order_relaxed),
                 muted.load (std::memory_order_relaxed) };
    }

private:
    std::atomic<std::int64_t> timelineStart { 0 };
    std::atomic<std::int64_t> lengthInSamples { 0 };
    std::atomic<std::int64_t> lengthInTicks { 0 };
    std::atomic<bool>         muted { false };
};

// A track's MIDI timeline the way the audio thread plays it, one entry per
// region: its events, fixed when the regions are published, and its placement,
// which an edit in place changes. The audio thread reads nothing else of a
// region.
struct MidiTimeline
{
    std::vector<MidiPlaybackRegion>      playback;
    std::unique_ptr<LiveMidiPlacement[]> placement;   // playback.size() of them

    std::size_t size() const noexcept { return playback.size(); }
};
} // namespace duskstudio
