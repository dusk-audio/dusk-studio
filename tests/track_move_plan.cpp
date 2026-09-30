#include <catch2/catch_test_macros.hpp>

#include "session/TrackMove.h"

#include <array>
#include <utility>
#include <vector>

using namespace duskstudio;

namespace
{
constexpr int kN = TrackMovePlan::kNumTracks;

// newToOld as a vector over the first `count` slots, for short expectations.
std::vector<int> head (const TrackMovePlan& plan, int count)
{
    return { plan.newToOld.begin(), plan.newToOld.begin() + count };
}

void requireConsistent (const TrackMovePlan& plan)
{
    for (int n = 0; n < kN; ++n)
    {
        const int old = plan.newToOld[(size_t) n];
        REQUIRE (old >= 0);
        REQUIRE (old < kN);
        REQUIRE (plan.oldToNew[(size_t) old] == n);
        if (n < plan.lo || n > plan.hi)
            REQUIRE (old == n);
    }
    if (! plan.isIdentity())
    {
        REQUIRE (plan.newToOld[(size_t) plan.lo] != plan.lo);
        REQUIRE (plan.newToOld[(size_t) plan.hi] != plan.hi);
    }
}
} // namespace

TEST_CASE ("Track move: one track dragged to the top shifts the ones above it down", "[track-move]")
{
    const auto plan = planBlockMove ({ 17 }, 0);
    requireConsistent (plan);

    REQUIRE (plan.newToOld[0] == 17);
    for (int n = 1; n <= 17; ++n)
        REQUIRE (plan.newToOld[(size_t) n] == n - 1);
    for (int n = 18; n < kN; ++n)
        REQUIRE (plan.newToOld[(size_t) n] == n);
    REQUIRE (plan.oldToNew[17] == 0);
    REQUIRE (plan.oldToNew[0] == 1);
    REQUIRE (plan.lo == 0);
    REQUIRE (plan.hi == 17);
}

TEST_CASE ("Track move: one track dragged down lands before the slot it was dropped on", "[track-move]")
{
    // Track 2 (slot 1) dropped between tracks 5 and 6: before old slot 5.
    const auto plan = planBlockMove ({ 1 }, 5);
    requireConsistent (plan);

    REQUIRE (head (plan, 7) == std::vector<int> { 0, 2, 3, 4, 1, 5, 6 });
    REQUIRE (plan.lo == 1);
    REQUIRE (plan.hi == 4);
}

TEST_CASE ("Track move: dropping a track past the last slot sends it to the end", "[track-move]")
{
    const auto plan = planBlockMove ({ 0 }, kN);
    requireConsistent (plan);

    REQUIRE (plan.newToOld[(size_t) kN - 1] == 0);
    REQUIRE (plan.newToOld[0] == 1);
    REQUIRE (plan.lo == 0);
    REQUIRE (plan.hi == kN - 1);
}

TEST_CASE ("Track move: a scattered selection moves as one block in its own order", "[track-move]")
{
    const auto plan = planBlockMove ({ 7, 2, 5 }, 1);
    requireConsistent (plan);

    REQUIRE (head (plan, 9) == std::vector<int> { 0, 2, 5, 7, 1, 3, 4, 6, 8 });
    REQUIRE (plan.lo == 1);
    REQUIRE (plan.hi == 7);
}

TEST_CASE ("Track move: dropping a block where it already is changes nothing", "[track-move]")
{
    SECTION ("before its first track")
    {
        REQUIRE (planBlockMove ({ 3, 4 }, 3).isIdentity());
    }
    SECTION ("before its own second track")
    {
        REQUIRE (planBlockMove ({ 3, 4 }, 4).isIdentity());
    }
    SECTION ("right after its last track")
    {
        REQUIRE (planBlockMove ({ 3, 4 }, 5).isIdentity());
    }
    SECTION ("an empty selection")
    {
        REQUIRE (planBlockMove ({}, 9).isIdentity());
    }
    SECTION ("slots that do not exist are ignored")
    {
        const auto plan = planBlockMove ({ -1, kN, 6, 6 }, 2);
        requireConsistent (plan);
        REQUIRE (head (plan, 7) == std::vector<int> { 0, 1, 6, 2, 3, 4, 5 });
    }
}

TEST_CASE ("Track move: the inverse plan puts every track back", "[track-move]")
{
    const auto plan = planBlockMove ({ 4, 9, 10 }, 20);
    const auto inverse = invertTrackMove (plan);
    requireConsistent (inverse);
    REQUIRE (inverse.lo == plan.lo);
    REQUIRE (inverse.hi == plan.hi);

    // Apply the move, then its inverse, to a row of labels.
    std::array<int, kN> labels {};
    for (int i = 0; i < kN; ++i) labels[(size_t) i] = 100 + i;
    std::array<int, kN> moved {}, back {};
    for (int n = 0; n < kN; ++n) moved[(size_t) n] = labels[(size_t) plan.newToOld[(size_t) n]];
    for (int n = 0; n < kN; ++n) back[(size_t) n] = moved[(size_t) inverse.newToOld[(size_t) n]];
    REQUIRE (back == labels);
    REQUIRE (moved != labels);
}

TEST_CASE ("Track move: a mapping that is not a permutation is refused", "[track-move]")
{
    std::array<int, kN> order {};
    for (int i = 0; i < kN; ++i) order[(size_t) i] = i;

    REQUIRE (trackMoveFromNewToOld (order).has_value());
    REQUIRE (trackMoveFromNewToOld (order)->isIdentity());

    auto repeated = order;
    repeated[3] = 4;
    REQUIRE_FALSE (trackMoveFromNewToOld (repeated).has_value());

    auto outOfRange = order;
    outOfRange[0] = kN;
    REQUIRE_FALSE (trackMoveFromNewToOld (outOfRange).has_value());

    auto swapped = order;
    std::swap (swapped[2], swapped[11]);
    const auto plan = trackMoveFromNewToOld (swapped);
    REQUIRE (plan.has_value());
    requireConsistent (*plan);
    REQUIRE (plan->lo == 2);
    REQUIRE (plan->hi == 11);
}

TEST_CASE ("Track move: a drop gap maps to the slot after the row above it", "[track-move]")
{
    SECTION ("every row visible")
    {
        std::vector<int> all;
        for (int i = 0; i < kN; ++i) all.push_back (i);
        REQUIRE (insertionSlotForGap (all, 0) == 0);
        REQUIRE (insertionSlotForGap (all, 5) == 5);
        REQUIRE (insertionSlotForGap (all, kN) == kN);
    }
    SECTION ("hidden tracks stay with the visible row above them")
    {
        // Tracks 3-5 and 9 are hidden.
        const std::vector<int> visible { 0, 1, 2, 6, 7, 8, 10 };
        REQUIRE (insertionSlotForGap (visible, 0) == 0);
        REQUIRE (insertionSlotForGap (visible, 3) == 3);    // after track 3 (slot 2)
        REQUIRE (insertionSlotForGap (visible, 4) == 7);    // after slot 6
        REQUIRE (insertionSlotForGap (visible, 7) == 11);   // after the last row
    }
    SECTION ("the top gap lands on the first visible row, not slot 0")
    {
        const std::vector<int> visible { 4, 5, 6 };
        REQUIRE (insertionSlotForGap (visible, 0) == 4);
        REQUIRE (planBlockMove ({ 6 }, insertionSlotForGap (visible, 0)).newToOld[4] == 6);
    }
    SECTION ("a gap past either end is clamped")
    {
        const std::vector<int> visible { 1, 2 };
        REQUIRE (insertionSlotForGap (visible, -3) == 1);
        REQUIRE (insertionSlotForGap (visible, 9) == 3);
        REQUIRE (insertionSlotForGap ({}, 2) == 0);
    }
}

TEST_CASE ("Track move: an input that follows its track number keeps the channel it had", "[track-move]")
{
    REQUIRE (followInputAfterMove (kInputFollowsTrack, 17) == 17);
    REQUIRE (followInputAfterMove (-1, 17) == -1);
    REQUIRE (followInputAfterMove (4, 17) == 4);
}
