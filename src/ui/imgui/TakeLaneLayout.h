#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

// The audio editor's take lanes, in design pixels: one lane per take under the region
// view, newest at the top, in a viewport that scrolls vertically.
namespace duskstudio::imgui::takelanes
{
// Lanes grow into the share of the band they are given, between these heights, and
// scroll once even the shortest no longer fit.
constexpr float kMinLaneHeight = 64.0f;
constexpr float kMaxLaneHeight = 160.0f;
constexpr float kHeaderHeight = 18.0f;
constexpr float kLaneGap = 2.0f;
constexpr float kCaptionHeight = 20.0f;
// The most of the band under the ruler the lanes take from the region view, by
// default and at either end of the divider's travel.
constexpr float kDefaultLaneShare = 0.55f;
constexpr float kMinLaneShare = 0.15f;
constexpr float kMaxLaneShare = 0.85f;

inline float contentHeight (int lanes, float laneHeight) noexcept
{
    return lanes <= 0 ? 0.0f : static_cast<float> (lanes) * (laneHeight + kLaneGap) - kLaneGap;
}

struct Split
{
    float region = 0.0f, caption = 0.0f, lanes = 0.0f;
    float laneHeight = kMinLaneHeight;
};

// Shares the band between the ruler and the scroll bar. The lanes get `share` of what
// the caption leaves, less whatever they cannot use at their tallest, and the region
// view the rest; each lane is as tall as that allows, within the lane heights. With no
// takes the region view keeps the whole band.
inline Split split (float band, int lanes, float share) noexcept
{
    band = std::max (0.0f, band);
    if (lanes <= 0)
        return { band, 0.0f, 0.0f, kMinLaneHeight };
    const float caption = std::min (kCaptionHeight, band);
    const float room = band - caption;
    const float laneArea = std::min (contentHeight (lanes, kMaxLaneHeight),
                                     room * std::clamp (share, kMinLaneShare, kMaxLaneShare));
    const float fitted = (laneArea + kLaneGap) / static_cast<float> (lanes) - kLaneGap;
    return { room - laneArea, caption, laneArea, std::clamp (fitted, kMinLaneHeight, kMaxLaneHeight) };
}

// The share that puts the top of the lane caption at `captionTop`, measured down from
// the top of the band.
inline float shareForCaptionAt (float captionTop, float band) noexcept
{
    const float room = std::max (1.0f, band - kCaptionHeight);
    return std::clamp ((room - captionTop) / room, kMinLaneShare, kMaxLaneShare);
}

inline float maxScroll (float viewport, int lanes, float laneHeight) noexcept
{
    return std::max (0.0f, contentHeight (lanes, laneHeight) - std::max (0.0f, viewport));
}

inline float clampScroll (float scroll, float viewport, int lanes, float laneHeight) noexcept
{
    return std::clamp (scroll, 0.0f, maxScroll (viewport, lanes, laneHeight));
}

// A lane's top edge below the viewport's.
inline float laneTop (int lane, float scroll, float laneHeight) noexcept
{
    return static_cast<float> (lane) * (laneHeight + kLaneGap) - scroll;
}

// The lane under a point `y` below the viewport's top edge; -1 in the gap between two
// lanes or past the last one.
inline int laneAt (float y, float scroll, int lanes, float laneHeight) noexcept
{
    const float content = y + scroll;
    const float pitch = laneHeight + kLaneGap;
    if (content < 0.0f || lanes <= 0)
        return -1;
    const int lane = static_cast<int> (content / pitch);
    if (lane >= lanes || content - static_cast<float> (lane) * pitch >= laneHeight)
        return -1;
    return lane;
}

// The scroll that brings a whole lane into view, moving as little as it can.
inline float revealScroll (int lane, float scroll, float viewport, int lanes, float laneHeight) noexcept
{
    const float top = static_cast<float> (lane) * (laneHeight + kLaneGap);
    const float bottom = top + laneHeight;
    if (top < scroll)
        scroll = top;
    else if (bottom > scroll + viewport)
        scroll = bottom - viewport;
    return clampScroll (scroll, viewport, lanes, laneHeight);
}

// Takes are stored in recording order and shown newest first, so the mapping is its
// own inverse. -1 for a lane or index out of range.
inline int takeIndexForLane (int lane, int lanes) noexcept
{
    return lane < 0 || lane >= lanes ? -1 : lanes - 1 - lane;
}

// The span a drag across a lane promotes: ordered and cut to the take. Empty (both
// ends equal) when it misses the take.
inline std::pair<std::int64_t, std::int64_t> dragSpan (std::int64_t from, std::int64_t to,
                                                       std::int64_t takeStart, std::int64_t takeEnd) noexcept
{
    const auto lo = std::max (std::min (from, to), takeStart);
    const auto hi = std::min (std::max (from, to), takeEnd);
    return hi > lo ? std::pair<std::int64_t, std::int64_t> { lo, hi }
                   : std::pair<std::int64_t, std::int64_t> { lo, lo };
}
} // namespace duskstudio::imgui::takelanes
