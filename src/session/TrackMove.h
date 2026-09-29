#pragma once

#include "SessionLayout.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace duskstudio
{
// A reordering of the track slots. newToOld[n] is the slot the track that ends
// up at n came from, oldToNew its inverse. lo..hi spans every slot whose track
// changes; lo > hi when none does.
struct TrackMovePlan
{
    static constexpr int kNumTracks = SessionLayout::kNumTracks;

    std::array<int, kNumTracks> newToOld {};
    std::array<int, kNumTracks> oldToNew {};
    int lo = 0;
    int hi = -1;

    bool isIdentity() const noexcept { return lo > hi; }
};

inline TrackMovePlan identityTrackMove() noexcept
{
    TrackMovePlan plan;
    for (int i = 0; i < TrackMovePlan::kNumTracks; ++i)
    {
        plan.newToOld[(size_t) i] = i;
        plan.oldToNew[(size_t) i] = i;
    }
    return plan;
}

// nullopt unless newToOld names every slot exactly once.
inline std::optional<TrackMovePlan> trackMoveFromNewToOld (
    const std::array<int, TrackMovePlan::kNumTracks>& newToOld) noexcept
{
    constexpr int n = TrackMovePlan::kNumTracks;
    TrackMovePlan plan;
    plan.oldToNew.fill (-1);
    plan.lo = n;
    plan.hi = -1;
    for (int to = 0; to < n; ++to)
    {
        const int from = newToOld[(size_t) to];
        if (from < 0 || from >= n || plan.oldToNew[(size_t) from] >= 0) return std::nullopt;
        plan.newToOld[(size_t) to] = from;
        plan.oldToNew[(size_t) from] = to;
        if (from != to)
        {
            plan.lo = std::min (plan.lo, to);
            plan.hi = std::max (plan.hi, to);
        }
    }
    if (plan.hi < 0) plan.lo = 0;
    return plan;
}

inline TrackMovePlan invertTrackMove (const TrackMovePlan& plan) noexcept
{
    TrackMovePlan inverse = plan;
    std::swap (inverse.newToOld, inverse.oldToNew);
    return inverse;
}

// Moves the tracks in `selected` as one block, their order kept, so that they
// land just before the track now at slot insertBefore (kNumTracks = after the
// last). Tracks outside the block keep their order and close up around it.
// Out-of-range and repeated slots in `selected` are ignored.
inline TrackMovePlan planBlockMove (const std::vector<int>& selected, int insertBefore)
{
    constexpr int n = TrackMovePlan::kNumTracks;
    std::array<bool, n> moving {};
    for (const int s : selected)
        if (s >= 0 && s < n) moving[(size_t) s] = true;
    insertBefore = std::clamp (insertBefore, 0, n);

    std::array<int, n> order {};
    int next = 0;
    for (int s = 0; s < insertBefore; ++s)
        if (! moving[(size_t) s]) order[(size_t) next++] = s;
    for (int s = 0; s < n; ++s)
        if (moving[(size_t) s]) order[(size_t) next++] = s;
    for (int s = insertBefore; s < n; ++s)
        if (! moving[(size_t) s]) order[(size_t) next++] = s;

    return *trackMoveFromNewToOld (order);
}

// Where a drop lands, as a slot for planBlockMove. visibleOrder lists the slots
// of the rows on screen, top to bottom; gap counts the rows above the drop.
// Past the first row the drop goes right after the row above it, so a hidden
// track between two visible rows stays with the upper one.
inline int insertionSlotForGap (const std::vector<int>& visibleOrder, int gap) noexcept
{
    if (visibleOrder.empty()) return 0;
    gap = std::clamp (gap, 0, (int) visibleOrder.size());
    return gap == 0 ? visibleOrder.front() : visibleOrder[(size_t) gap - 1] + 1;
}

// A track whose input follows its own track number keeps the physical input it
// had, so a moved track records from the same channel as before.
constexpr int kInputFollowsTrack = -2;

constexpr int followInputAfterMove (int inputSource, int oldSlot) noexcept
{
    return inputSource == kInputFollowsTrack ? oldSlot : inputSource;
}
} // namespace duskstudio
