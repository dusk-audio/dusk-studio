#pragma once

#include "../../foundation/MidiBuffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace duskstudio::midi
{
constexpr std::size_t kMidiOutMaxEvents =
    dusk::kMidiRoutingBlockBytes / dusk::MidiBuffer::minimumEventStorageBytes();

struct MidiSortScratch
{
    struct EventRef
    {
        const std::uint8_t* data;
        int numBytes;
        int samplePosition;
    };

    std::array<EventRef, kMidiOutMaxEvents> events;
    std::array<std::uint32_t, kMidiOutMaxEvents> order;
};

inline void copyMidiSorted (const dusk::MidiBuffer& source,
                            dusk::MidiBuffer& destination,
                            MidiSortScratch& scratch) noexcept
{
    destination.clear();

    std::size_t eventCount = 0;
    for (const auto meta : source)
    {
        if (eventCount == scratch.events.size())
            return;
        scratch.events[eventCount] = { meta.data, meta.numBytes, meta.samplePosition };
        scratch.order[eventCount] = (std::uint32_t) eventCount;
        ++eventCount;
    }

    std::sort (scratch.order.begin(), scratch.order.begin() + eventCount,
               [&scratch] (std::uint32_t lhs, std::uint32_t rhs)
               {
                   const int lhsPosition = scratch.events[lhs].samplePosition;
                   const int rhsPosition = scratch.events[rhs].samplePosition;
                   return lhsPosition < rhsPosition
                       || (lhsPosition == rhsPosition && lhs < rhs);
               });

    for (std::size_t i = 0; i < eventCount; ++i)
    {
        const auto& meta = scratch.events[scratch.order[i]];
        if (destination.addEvent (meta.data, meta.numBytes, meta.samplePosition))
            continue;
        if (destination.fitsWhenEmpty (meta.numBytes))
        {
            destination.clear();
            return;
        }
    }
}
} // namespace duskstudio::midi
