#pragma once

#include "MidiEvents.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace duskstudio
{
template <typename Regions>
int findMidiMergeTarget (const Regions& regions, std::int64_t start, std::int64_t end) noexcept
{
    if (end <= start) return -1;
    for (int i = (int) regions.size() - 1; i >= 0; --i)
    {
        const auto& region = regions[(std::size_t) i];
        if (! region.locked && region.lengthInSamples > 0 && end > region.timelineStart
            && (start < region.timelineStart
                || (std::uint64_t) start - (std::uint64_t) region.timelineStart
                     < (std::uint64_t) region.lengthInSamples))
            return i;
    }
    return -1;
}

struct MidiMergeResult
{
    bool changed = false;
    std::int64_t leadingTicks = 0;
};

template <typename Performance>
MidiMergeResult mergeMidiPass (Performance& target, const Performance& pass,
                               std::int64_t offsetTicks)
{
    if (&target == &pass || (pass.notes.empty() && pass.ccs.empty())) return {};
    if (offsetTicks == std::numeric_limits<std::int64_t>::min()) return {};
    const auto leading = std::max<std::int64_t> (0, -offsetTicks);
    const auto offset = std::max<std::int64_t> (0, offsetTicks);
    const auto fits = [] (std::int64_t position, std::int64_t amount)
    { return position >= 0 && amount >= 0
        && position <= std::numeric_limits<std::int64_t>::max() - amount; };
    if (! fits (target.lengthInTicks, leading) || ! fits (pass.lengthInTicks, offset)) return {};
    for (const auto& note : target.notes)
        if (! fits (note.startTick, leading)) return {};
    for (const auto& control : target.ccs)
        if (! fits (control.atTick, leading)) return {};
    for (const auto& take : target.previousTakes)
    {
        if (! fits (take.lengthInTicks, leading)) return {};
        for (const auto& note : take.notes)
            if (! fits (note.startTick, leading)) return {};
        for (const auto& control : take.ccs)
            if (! fits (control.atTick, leading)) return {};
    }
    for (const auto& note : pass.notes)
        if (! fits (note.startTick, offset)
            || ! fits (note.startTick + offset, std::max<std::int64_t> (0, note.lengthInTicks))) return {};
    for (const auto& control : pass.ccs)
        if (! fits (control.atTick, offset) || ! fits (control.atTick + offset, 1)) return {};

    target.notes.reserve (target.notes.size() + pass.notes.size());
    target.ccs.reserve (target.ccs.size() + pass.ccs.size());
    for (auto& note : target.notes) note.startTick += leading;
    for (auto& control : target.ccs) control.atTick += leading;
    for (auto& take : target.previousTakes)
    {
        take.lengthInTicks += leading;
        for (auto& note : take.notes) note.startTick += leading;
        for (auto& control : take.ccs) control.atTick += leading;
    }
    target.lengthInTicks = std::max (target.lengthInTicks + leading, pass.lengthInTicks + offset);
    for (auto note : pass.notes)
    {
        note.startTick += offset;
        target.lengthInTicks = std::max (target.lengthInTicks,
                                        note.startTick + std::max<std::int64_t> (0, note.lengthInTicks));
        target.notes.push_back (note);
    }
    for (auto control : pass.ccs)
    {
        control.atTick += offset;
        target.lengthInTicks = std::max (target.lengthInTicks, control.atTick + 1);
        target.ccs.push_back (control);
    }
    std::stable_sort (target.notes.begin(), target.notes.end(),
                      [] (const MidiNote& a, const MidiNote& b) { return a.startTick < b.startTick; });
    std::stable_sort (target.ccs.begin(), target.ccs.end(),
                      [] (const MidiCc& a, const MidiCc& b) { return a.atTick < b.atTick; });
    return { true, leading };
}
} // namespace duskstudio
