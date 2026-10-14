#pragma once

#include "MidiEvents.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace duskstudio
{
struct MidiMergeResult
{
    bool changed = false;
    std::int64_t leadingTicks = 0;
};

// The same event payload is held by regions and recorded passes. This transform
// leaves their placement, identity, display metadata and take history to the owner.
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
    for (const auto& note : pass.notes)
        if (! fits (note.startTick, offset)
            || ! fits (note.startTick + offset, std::max<std::int64_t> (0, note.lengthInTicks))) return {};
    for (const auto& control : pass.ccs)
        if (! fits (control.atTick, offset) || ! fits (control.atTick + offset, 1)) return {};

    target.notes.reserve (target.notes.size() + pass.notes.size());
    target.ccs.reserve (target.ccs.size() + pass.ccs.size());
    for (auto& note : target.notes) note.startTick += leading;
    for (auto& control : target.ccs) control.atTick += leading;
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
