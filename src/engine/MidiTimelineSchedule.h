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

// Sends one span's MIDI through emit (status, data1, data2, sampleInSpan), which
// returns false for an event it cannot take. At one sample, controllers go
// before notes, so a value is in place before the note it shapes, and
// note-offs before note-ons, so a note that ends where the next on its key
// begins does not cut that one off. Only what falls in the span is read, plus,
// for a chase, what decides each controller's value and which notes are held
// at its start; scans bounds that work, and running out of it is reported.
template <typename Emit>
Outcome scheduleSpan (const std::vector<MidiRegion>& regions,
                      const std::vector<MidiPlaybackRegion>& playback,
                      const TempoMap* map, double sampleRate, float bpm,
                      const Span& span, int& scans, ControllerChase& chase,
                      Emit&& emit) noexcept
{
    const auto take = [&scans] (int n) noexcept
    {
        if (scans < n)
        {
            scans = 0;
            return false;
        }
        scans -= n;
        return true;
    };
    const TempoMap* tempoMap = (map != nullptr && ! map->empty()) ? map : nullptr;
    const auto clockOf = [&] (const MidiRegion& region) noexcept
    {
        return RegionClock (tempoMap, sampleRate, bpm, region.timelineStart);
    };
    const auto overlapsSpan = [&] (const MidiRegion& region, const RegionClock& clock) noexcept
    {
        const auto end = tempoMap != nullptr ? clock.at (region.lengthInTicks)
                                             : region.timelineStart + region.lengthInSamples;
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
        if (span.chase)
        {
            for (const auto& lane : events.lanes)
            {
                if (! take (searchCost (lane.end - lane.begin))) return Outcome::scanBudgetSpent;
                const auto first = events.byController.begin() + lane.begin;
                const auto last  = events.byController.begin() + lane.end;
                const auto next = std::partition_point (first, last, [&] (const auto& c)
                                                        { return clock.at (c.tick) < span.start; });
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
                if (next != last && clock.at (next->tick) == span.start)
                    chase.explicitAtStart[key] = true;
            }
        }
        if (! overlapsSpan (region, clock)) continue;
        if (! take (searchCost (events.controllers.size()))) return Outcome::scanBudgetSpent;
        for (auto c = firstFrom (events.controllers, clock, [] (const auto& e) { return e.tick; });
             c != events.controllers.end(); ++c)
        {
            if (! take (1)) return Outcome::scanBudgetSpent;
            const auto at = clock.at (c->tick);
            if (at >= span.end) break;
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
        if (! overlapsSpan (region, clock)) continue;
        if (! take (searchCost (events.releases.size()))) return Outcome::scanBudgetSpent;
        for (auto r = firstFrom (events.releases, clock, [] (const auto& e) { return e.endTick; });
             r != events.releases.end(); ++r)
        {
            if (! take (1)) return Outcome::scanBudgetSpent;
            const auto at = clock.at (r->endTick);
            if (at >= span.end) break;
            if (! emit (status (0x80, r->channel), r->key, 0, at - span.start))
                return Outcome::eventRefused;
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
            if (! overlapsSpan (region, clock)) continue;
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
        if (! overlapsSpan (region, clock)) continue;
        if (! take (searchCost (events.notes.size()))) return Outcome::scanBudgetSpent;
        for (auto n = firstFrom (events.notes, clock, [] (const auto& e) { return e.startTick; });
             n != events.notes.end(); ++n)
        {
            if (! take (1)) return Outcome::scanBudgetSpent;
            const auto at = clock.at (n->startTick);
            if (at >= span.end) break;
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
} // namespace duskstudio::midischedule
