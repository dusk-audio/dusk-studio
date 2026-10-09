#pragma once

#include "../session/MidiPlayback.h"
#include "../session/Session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// The timeline MIDI one block plays, read from the time-sorted copy each region
// carries (MidiPlaybackRegion). Audio thread: no allocation, no locks.
namespace duskstudio::midischedule
{
// The latest value each channel and controller had before a chased span. Held
// by the engine, so the audio thread does not carry it on its stack.
struct ControllerChase
{
    static constexpr int kKeys = 16 * 128;

    std::array<int, kKeys>          value {};
    std::array<std::int64_t, kKeys> at {};
    std::array<bool, kKeys>         seen {};
    std::array<bool, kKeys>         explicitAtStart {};
    std::array<int, kKeys>          keys {};
    int keyCount = 0;

    void clear() noexcept
    {
        seen.fill (false);
        explicitAtStart.fill (false);
        keyCount = 0;
    }
};

// Notes counted by channel and key. The engine keeps one per track of the notes
// its timeline started on the instrument and has not ended: what an edit that
// takes notes off the timeline would otherwise leave sounding. Audio thread.
struct NoteCount
{
    static constexpr int kKeys = 16 * 128;

    std::array<std::uint8_t, kKeys> count {};
    int total = 0;

    // A status byte's low nibble is its channel, so either goes in.
    static std::size_t keyOf (std::uint8_t channel, std::uint8_t key) noexcept
    {
        return (std::size_t) (channel & 0x0F) * 128 + (std::size_t) (key & 0x7F);
    }

    void add (std::size_t key) noexcept
    {
        if (count[key] == 255) return;
        ++count[key];
        ++total;
    }

    // A note-on adds its key, a note-off (or a note-on at velocity 0) takes
    // one away; anything else is not a note.
    void hear (std::uint8_t status, std::uint8_t key, std::uint8_t velocity) noexcept
    {
        const int kind = status & 0xF0;
        const auto at = keyOf (status, key);
        if (kind == 0x90 && velocity > 0)
            add (at);
        else if ((kind == 0x80 || kind == 0x90) && count[at] > 0)
        {
            --count[at];
            --total;
        }
    }

    void clear() noexcept
    {
        if (total == 0) return;
        count.fill (0);
        total = 0;
    }
};

enum class Outcome
{
    complete,
    eventRefused,      // emit declined an event: the block's room for MIDI ran out
    scanBudgetSpent    // there was more to read than the scan budget allowed
};

// One stretch of the timeline the block plays, [start, end) in timeline
// samples. A chased span follows a reset: it sends every controller's value
// and every note held across its start again.
struct Span
{
    std::int64_t start = 0;
    std::int64_t end   = 0;
    bool         chase = false;
};

// Tick to timeline sample for one region: through the tempo map when there is
// one, else at the session's constant tempo.
struct RegionClock
{
    RegionClock (const TempoMap* tempoMap, double sr, float tempo, std::int64_t start) noexcept
        : map (tempoMap), sampleRate (sr), bpm (tempo), regionStart (start),
          regionStartTick (tempoMap != nullptr ? tempoMap->samplesToTicks (start, sr) : 0)
    {}

    std::int64_t at (std::int64_t tick) const noexcept
    {
        return map != nullptr ? map->ticksToSamples (regionStartTick + tick, sampleRate)
                              : regionStart + ticksToSamples (tick, sampleRate, bpm);
    }

    const TempoMap* map;
    double          sampleRate;
    float           bpm;
    std::int64_t    regionStart;
    std::int64_t    regionStartTick;
};

// Probes a binary search over n events makes.
inline int searchCost (std::size_t n) noexcept
{
    int probes = 1;
    for (; n > 0; n >>= 1)
        ++probes;
    return probes;
}

inline std::int64_t regionEnd (const MidiRegion& region, const RegionClock& clock) noexcept
{
    return clock.map != nullptr ? clock.at (region.lengthInTicks)
                                : region.timelineStart + region.lengthInSamples;
}

inline bool takeScans (int& scans, int n) noexcept
{
    if (scans < n)
    {
        scans = 0;
        return false;
    }
    scans -= n;
    return true;
}

// Calls held (note) for every note of one region that sounds at `at` and ends at
// or after it, its end taken no later than the region's: a note the region
// itself still has to end. Reads only the notes started before `at`, skipping
// any run of kNoteChunk of them that all ended earlier.
template <typename Held>
bool forEachNoteSoundingAt (const MidiPlaybackRegion& events, const RegionClock& clock,
                            std::int64_t end, std::int64_t at, int& scans, Held&& held) noexcept
{
    if (! takeScans (scans, searchCost (events.notes.size()))) return false;
    const auto started = (std::size_t) (std::partition_point (events.notes.begin(), events.notes.end(),
                                                              [&] (const auto& n) { return clock.at (n.startTick) < at; })
                                        - events.notes.begin());
    constexpr auto chunk = MidiPlaybackRegion::kNoteChunk;
    for (std::size_t c = 0; c * chunk < started; ++c)
    {
        if (! takeScans (scans, 1)) return false;
        if (clock.at (events.chunkLatestEnd[c]) < at) continue;
        const auto last = std::min (started, (c + 1) * chunk);
        for (auto n = c * chunk; n < last; ++n)
        {
            if (! takeScans (scans, 1)) return false;
            const auto& note = events.notes[n];
            if (note.endTick <= note.startTick || std::min (clock.at (note.endTick), end) < at) continue;
            if (! held (note)) return false;
        }
    }
    return true;
}

// Sends one span's MIDI through emit (status, data1, data2, sampleInSpan), which
// returns false for an event it cannot take. A region plays nothing from its
// end on, and a note still sounding there ends there. At one sample,
// controllers go before notes, so a value is in place before the note it
// shapes, and note-offs before note-ons, so a note that ends where the next on
// its key begins does not cut that one off. Only what falls in the span is
// read, plus, for a chase, what decides each controller's value and which
// notes are held at its start; scans bounds that work, and running out of it
// is reported.
template <typename Emit>
Outcome scheduleSpan (const std::vector<MidiRegion>& regions,
                      const std::vector<MidiPlaybackRegion>& playback,
                      const TempoMap* map, double sampleRate, float bpm,
                      const Span& span, int& scans, ControllerChase& chase,
                      Emit&& emit) noexcept
{
    const auto take = [&scans] (int n) noexcept { return takeScans (scans, n); };
    const TempoMap* tempoMap = (map != nullptr && ! map->empty()) ? map : nullptr;
    const auto clockOf = [&] (const MidiRegion& region) noexcept
    {
        return RegionClock (tempoMap, sampleRate, bpm, region.timelineStart);
    };
    const auto overlapsSpan = [&] (const MidiRegion& region, std::int64_t end) noexcept
    {
        return end > span.start && region.timelineStart < span.end;
    };
    const auto firstFrom = [&] (const auto& events, const RegionClock& clock, auto tickOf) noexcept
    {
        return std::partition_point (events.begin(), events.end(),
                                     [&] (const auto& e) { return clock.at (tickOf (e)) < span.start; });
    };
    const auto status = [] (int kind, std::uint8_t channel) noexcept
    {
        return (std::uint8_t) (kind | channel);
    };
    const std::size_t count = std::min (regions.size(), playback.size());

    if (span.chase)
        chase.clear();
    for (std::size_t i = 0; i < count; ++i)
    {
        if (! take (1)) return Outcome::scanBudgetSpent;
        const auto& region = regions[i];
        const auto& events = playback[i];
        if (region.muted || events.controllers.empty()) continue;
        const auto clock = clockOf (region);
        const auto end = regionEnd (region, clock);
        if (span.chase)
        {
            const auto upTo = std::min (span.start, end);
            for (const auto& lane : events.lanes)
            {
                if (! take (searchCost (lane.end - lane.begin))) return Outcome::scanBudgetSpent;
                const auto first = events.byController.begin() + lane.begin;
                const auto last  = events.byController.begin() + lane.end;
                const auto next = std::partition_point (first, last, [&] (const auto& c)
                                                        { return clock.at (c.tick) < upTo; });
                const auto key = (std::size_t) lane.key;
                if (next != first)
                {
                    const auto& latest = *(next - 1);
                    const auto at = clock.at (latest.tick);
                    if (! chase.seen[key] || at >= chase.at[key])
                    {
                        if (! chase.seen[key])
                            chase.keys[(std::size_t) chase.keyCount++] = lane.key;
                        chase.seen[key] = true;
                        chase.at[key] = at;
                        chase.value[key] = latest.value;
                    }
                }
                if (end > span.start && next != last && clock.at (next->tick) == span.start)
                    chase.explicitAtStart[key] = true;
            }
        }
        if (! overlapsSpan (region, end)) continue;
        if (! take (searchCost (events.controllers.size()))) return Outcome::scanBudgetSpent;
        for (auto c = firstFrom (events.controllers, clock, [] (const auto& e) { return e.tick; });
             c != events.controllers.end(); ++c)
        {
            if (! take (1)) return Outcome::scanBudgetSpent;
            const auto at = clock.at (c->tick);
            if (at >= span.end || at >= end) break;
            if (! emit (status (0xB0, c->channel), c->controller, c->value, at - span.start))
                return Outcome::eventRefused;
        }
    }
    if (span.chase)
        for (int k = 0; k < chase.keyCount; ++k)
        {
            const auto key = (std::size_t) chase.keys[(std::size_t) k];
            if (chase.explicitAtStart[key]) continue;
            if (! emit (status (0xB0, (std::uint8_t) (key / 128)), (std::uint8_t) (key % 128),
                        (std::uint8_t) chase.value[key], 0))
                return Outcome::eventRefused;
        }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (! take (1)) return Outcome::scanBudgetSpent;
        const auto& region = regions[i];
        const auto& events = playback[i];
        if (region.muted || events.releases.empty()) continue;
        const auto clock = clockOf (region);
        const auto end = regionEnd (region, clock);
        if (overlapsSpan (region, end))
        {
            if (! take (searchCost (events.releases.size()))) return Outcome::scanBudgetSpent;
            for (auto r = firstFrom (events.releases, clock, [] (const auto& e) { return e.endTick; });
                 r != events.releases.end(); ++r)
            {
                if (! take (1)) return Outcome::scanBudgetSpent;
                const auto at = clock.at (r->endTick);
                if (at >= span.end || at >= end) break;
                if (! emit (status (0x80, r->channel), r->key, 0, at - span.start))
                    return Outcome::eventRefused;
            }
        }
        // The notes still sounding where the region ends end with it. Also
        // when the end is the span's first sample, which the span before
        // stopped short of.
        if (end >= span.start && end < span.end)
        {
            bool refused = false;
            if (! forEachNoteSoundingAt (events, clock, end, end, scans, [&] (const auto& note) noexcept
                {
                    refused = ! emit (status (0x80, note.channel), note.key, 0, end - span.start);
                    return ! refused;
                }))
                return refused ? Outcome::eventRefused : Outcome::scanBudgetSpent;
        }
    }

    if (span.chase)
        for (std::size_t i = 0; i < count; ++i)
        {
            if (! take (1)) return Outcome::scanBudgetSpent;
            const auto& region = regions[i];
            const auto& events = playback[i];
            if (region.muted || events.notes.empty()) continue;
            const auto clock = clockOf (region);
            if (! overlapsSpan (region, regionEnd (region, clock))) continue;
            if (! take (searchCost (events.notes.size()))) return Outcome::scanBudgetSpent;
            const auto started = (std::size_t) (firstFrom (events.notes, clock,
                                                           [] (const auto& e) { return e.startTick; })
                                                 - events.notes.begin());
            constexpr auto chunk = MidiPlaybackRegion::kNoteChunk;
            for (std::size_t c = 0; c * chunk < started; ++c)
            {
                if (! take (1)) return Outcome::scanBudgetSpent;
                if (clock.at (events.chunkLatestEnd[c]) <= span.start) continue;
                const auto last = std::min (started, (c + 1) * chunk);
                for (auto n = c * chunk; n < last; ++n)
                {
                    if (! take (1)) return Outcome::scanBudgetSpent;
                    const auto& note = events.notes[n];
                    if (clock.at (note.endTick) <= span.start) continue;
                    if (! emit (status (0x90, note.channel), note.key, note.velocity, 0))
                        return Outcome::eventRefused;
                }
            }
        }

    for (std::size_t i = 0; i < count; ++i)
    {
        if (! take (1)) return Outcome::scanBudgetSpent;
        const auto& region = regions[i];
        const auto& events = playback[i];
        if (region.muted || events.notes.empty()) continue;
        const auto clock = clockOf (region);
        const auto end = regionEnd (region, clock);
        if (! overlapsSpan (region, end)) continue;
        if (! take (searchCost (events.notes.size()))) return Outcome::scanBudgetSpent;
        for (auto n = firstFrom (events.notes, clock, [] (const auto& e) { return e.startTick; });
             n != events.notes.end(); ++n)
        {
            if (! take (1)) return Outcome::scanBudgetSpent;
            const auto at = clock.at (n->startTick);
            if (at >= span.end || at >= end) break;
            if (! emit (status (0x90, n->channel), n->key, n->velocity, at - span.start))
                return Outcome::eventRefused;
            // A note with no length ends where it starts, right behind its on.
            if (n->endTick <= n->startTick
                && ! emit (status (0x80, n->channel), n->key, 0, at - span.start))
                return Outcome::eventRefused;
        }
    }
    return Outcome::complete;
}

// A note-off through emit (status, data1, data2, 0) for each note `sounding`
// counts beyond those `held` counts on its key, each one leaving the count.
template <typename Emit>
Outcome releaseBeyond (NoteCount& sounding, const NoteCount& held, Emit&& emit) noexcept
{
    for (std::size_t key = 0; key < (std::size_t) NoteCount::kKeys && sounding.total > 0; ++key)
        while (sounding.count[key] > held.count[key])
        {
            if (! emit ((std::uint8_t) (0x80 | (key / 128)), (std::uint8_t) (key % 128), (std::uint8_t) 0, 0))
                return Outcome::eventRefused;
            --sounding.count[key];
            --sounding.total;
        }
    return Outcome::complete;
}

// Ends what the timeline left sounding when it changed under its notes: a
// region muted, moved, deleted or cut short, a take switched, a note edited,
// the tempo changed. For each key, the notes the timeline as it stands still
// sounds at `at` and will end itself are left alone; every other note
// `sounding` counts is released (releaseBeyond). `held` is scratch.
template <typename Emit>
Outcome releaseStranded (const std::vector<MidiRegion>& regions,
                         const std::vector<MidiPlaybackRegion>& playback,
                         const TempoMap* map, double sampleRate, float bpm,
                         std::int64_t at, int& scans, NoteCount& sounding, NoteCount& held,
                         Emit&& emit) noexcept
{
    if (sounding.total == 0) return Outcome::complete;
    const TempoMap* tempoMap = (map != nullptr && ! map->empty()) ? map : nullptr;
    held.clear();
    const std::size_t count = std::min (regions.size(), playback.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (! takeScans (scans, 1)) return Outcome::scanBudgetSpent;
        const auto& region = regions[i];
        const auto& events = playback[i];
        if (region.muted || events.releases.empty()) continue;
        const RegionClock clock (tempoMap, sampleRate, bpm, region.timelineStart);
        const auto end = regionEnd (region, clock);
        if (end < at) continue;
        if (! forEachNoteSoundingAt (events, clock, end, at, scans, [&held] (const auto& note) noexcept
            {
                held.add (NoteCount::keyOf (note.channel, note.key));
                return true;
            }))
            return Outcome::scanBudgetSpent;
    }
    return releaseBeyond (sounding, held, emit);
}
} // namespace duskstudio::midischedule
