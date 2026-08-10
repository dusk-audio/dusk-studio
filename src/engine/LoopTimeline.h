#pragma once

#include <cstdint>
#include <limits>
#include <utility>

namespace duskstudio
{
struct LoopTimelineSpan
{
    std::int64_t timelineStart = 0;
    int          bufferOffset  = 0;
    int          length        = 0;
    bool         wrappedBefore = false;
};

namespace detail
{
// Returns start + distance without overflowing a signed intermediate. Callers
// guarantee that the mathematical result is representable as int64_t.
constexpr std::int64_t addTimelineDistance (std::int64_t start,
                                             std::uint64_t distance) noexcept
{
    if (start >= 0)
        return start + static_cast<std::int64_t> (distance);

    const auto magnitude = static_cast<std::uint64_t> (-(start + 1)) + 1;
    if (distance >= magnitude)
        return static_cast<std::int64_t> (distance - magnitude);

    const auto remaining = magnitude - distance;
    if (remaining == static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max()) + 1)
        return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t> (remaining);
}
} // namespace detail

// Enumerates the timeline portions represented by one host buffer. A valid loop
// wraps only at loopEnd: a block beginning before loopStart (for example during
// count-in) therefore continues linearly through loopStart before its first
// wrap. The callback is invoked directly, so this helper owns no storage and
// performs no allocation or synchronization.
template <typename Callback>
void forEachLoopTimelineSpan (std::int64_t blockStart,
                              int numSamples,
                              bool loopEnabled,
                              std::int64_t loopStart,
                              std::int64_t loopEnd,
                              Callback&& callback)
    noexcept (noexcept (std::declval<Callback&>() (std::declval<const LoopTimelineSpan&>())))
{
    if (numSamples <= 0)
        return;

    if (! loopEnabled || loopEnd <= loopStart)
    {
        const LoopTimelineSpan span { blockStart, 0, numSamples, false };
        callback (span);
        return;
    }

    const auto loopLength = static_cast<std::uint64_t> (loopEnd)
                          - static_cast<std::uint64_t> (loopStart);
    std::int64_t position = blockStart;
    bool wrappedBefore = false;

    if (position >= loopEnd)
    {
        const auto distancePastEnd = static_cast<std::uint64_t> (position)
                                   - static_cast<std::uint64_t> (loopEnd);
        position = detail::addTimelineDistance (loopStart, distancePastEnd % loopLength);
        wrappedBefore = true;
    }

    int bufferOffset = 0;
    while (bufferOffset < numSamples)
    {
        const auto distanceToEnd = static_cast<std::uint64_t> (loopEnd)
                                 - static_cast<std::uint64_t> (position);
        const int remaining = numSamples - bufferOffset;
        const int length = distanceToEnd < static_cast<std::uint64_t> (remaining)
                         ? static_cast<int> (distanceToEnd)
                         : remaining;

        const LoopTimelineSpan span { position, bufferOffset, length, wrappedBefore };
        callback (span);
        bufferOffset += length;
        position = detail::addTimelineDistance (position, static_cast<std::uint64_t> (length));

        if (bufferOffset < numSamples && position == loopEnd)
        {
            position = loopStart;
            wrappedBefore = true;
        }
        else
        {
            wrappedBefore = false;
        }
    }
}
} // namespace duskstudio
