#pragma once

#include <algorithm>
#include <cstdint>

namespace duskstudio
{
// Left edge of a timeline view, in samples, that keeps the playhead in sight
// without touching the zoom. The view holds still while the playhead sits
// inside it short of a small right-hand margin; otherwise the page turns so
// the playhead lands a quarter of the way in.
inline std::int64_t followPlayheadScroll (std::int64_t playhead,
                                          std::int64_t scroll,
                                          std::int64_t visibleSamples) noexcept
{
    if (visibleSamples <= 0)
        return scroll;

    const std::int64_t rightMargin = visibleSamples / 32;
    if (playhead >= scroll && playhead < scroll + visibleSamples - rightMargin)
        return scroll;

    return std::max<std::int64_t> (0, playhead - visibleSamples / 4);
}
} // namespace duskstudio
