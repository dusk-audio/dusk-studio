#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/TakeLaneLayout.h"

using namespace duskstudio::imgui::takelanes;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Take lanes leave the band to the region view when there are no takes", "[takes][editor]")
{
    const auto none = split (400.0f, 0, kDefaultLaneShare);
    REQUIRE_THAT (none.region, WithinAbs (400.0f, 1e-6));
    REQUIRE_THAT (none.caption, WithinAbs (0.0f, 1e-6));
    REQUIRE_THAT (none.lanes, WithinAbs (0.0f, 1e-6));

    const auto many = split (400.0f, 20, kDefaultLaneShare);
    REQUIRE_THAT (many.lanes, WithinAbs ((400.0f - kCaptionHeight) * kDefaultLaneShare, 1e-4));
    REQUIRE_THAT (many.laneHeight, WithinAbs (kMinLaneHeight, 1e-6));
    REQUIRE (many.region > 0.0f);
}

TEST_CASE ("Take lanes grow into their share of the band, up to their tallest", "[takes][editor]")
{
    // Four lanes in 780 of room at the default share: 429 between them.
    const auto four = split (800.0f, 4, kDefaultLaneShare);
    REQUIRE_THAT (four.lanes, WithinAbs (780.0f * kDefaultLaneShare, 1e-3));
    REQUIRE_THAT (four.laneHeight, WithinAbs ((four.lanes + kLaneGap) / 4.0f - kLaneGap, 1e-3));
    REQUIRE_THAT (contentHeight (4, four.laneHeight), WithinAbs (four.lanes, 1e-3));
    REQUIRE_THAT (four.region + four.caption + four.lanes, WithinAbs (800.0f, 1e-3));

    // One lane stops at its tallest and hands the rest back to the region view.
    const auto one = split (800.0f, 1, kDefaultLaneShare);
    REQUIRE_THAT (one.laneHeight, WithinAbs (kMaxLaneHeight, 1e-6));
    REQUIRE_THAT (one.lanes, WithinAbs (kMaxLaneHeight, 1e-6));
    REQUIRE_THAT (one.region, WithinAbs (800.0f - kCaptionHeight - kMaxLaneHeight, 1e-3));

    // A share past either end of the divider's travel is held there.
    REQUIRE_THAT (split (800.0f, 20, 5.0f).lanes, WithinAbs (780.0f * kMaxLaneShare, 1e-3));
    REQUIRE_THAT (split (800.0f, 20, 0.0f).lanes, WithinAbs (780.0f * kMinLaneShare, 1e-3));
}

TEST_CASE ("Dragging the lane caption sets the lanes' share of the band", "[takes][editor]")
{
    const float band = 520.0f;
    const float room = band - kCaptionHeight;
    REQUIRE_THAT (shareForCaptionAt (room * 0.5f, band), WithinAbs (0.5f, 1e-5));
    REQUIRE_THAT (shareForCaptionAt (room * 0.25f, band), WithinAbs (0.75f, 1e-5));
    REQUIRE_THAT (shareForCaptionAt (0.0f, band), WithinAbs (kMaxLaneShare, 1e-6));
    REQUIRE_THAT (shareForCaptionAt (band, band), WithinAbs (kMinLaneShare, 1e-6));
    // The caption lands where it was dropped.
    const auto dropped = split (band, 30, shareForCaptionAt (room * 0.4f, band));
    REQUIRE_THAT (dropped.region, WithinAbs (room * 0.4f, 1e-3));
}

TEST_CASE ("Take lanes put the newest take at the top", "[takes][editor]")
{
    REQUIRE (takeIndexForLane (0, 3) == 2);
    REQUIRE (takeIndexForLane (2, 3) == 0);
    REQUIRE (takeIndexForLane (3, 3) == -1);
    REQUIRE (takeIndexForLane (-1, 3) == -1);
}

TEST_CASE ("A point in the take lane viewport names the lane under it", "[takes][editor]")
{
    for (const float height : { kMinLaneHeight, 100.0f })
    {
        INFO ("lanes " << height << " tall");
        const float pitch = height + kLaneGap;
        REQUIRE (laneAt (0.0f, 0.0f, 3, height) == 0);
        REQUIRE (laneAt (height - 0.5f, 0.0f, 3, height) == 0);
        REQUIRE (laneAt (height + kLaneGap * 0.5f, 0.0f, 3, height) == -1);
        REQUIRE (laneAt (pitch, 0.0f, 3, height) == 1);
        REQUIRE (laneAt (pitch * 3.0f, 0.0f, 3, height) == -1);
        REQUIRE (laneAt (-1.0f, 0.0f, 3, height) == -1);
        REQUIRE (laneAt (1.0f, pitch * 2.0f, 3, height) == 2);
        REQUIRE_THAT (laneTop (2, pitch, height), WithinAbs (pitch, 1e-6));
    }
}

TEST_CASE ("Revealing a take lane scrolls it wholly into view and no further", "[takes][editor]")
{
    const float height = kMinLaneHeight;
    const float pitch = height + kLaneGap;
    const float viewport = pitch * 2.0f;
    REQUIRE_THAT (maxScroll (viewport, 5, height), WithinAbs (contentHeight (5, height) - viewport, 1e-4));
    REQUIRE_THAT (maxScroll (viewport, 1, height), WithinAbs (0.0f, 1e-6));
    REQUIRE_THAT (clampScroll (-10.0f, viewport, 5, height), WithinAbs (0.0f, 1e-6));

    REQUIRE_THAT (revealScroll (0, 0.0f, viewport, 5, height), WithinAbs (0.0f, 1e-6));
    REQUIRE_THAT (revealScroll (4, 0.0f, viewport, 5, height), WithinAbs (maxScroll (viewport, 5, height), 1e-4));
    REQUIRE_THAT (revealScroll (2, 0.0f, viewport, 5, height), WithinAbs (2.0f * pitch + height - viewport, 1e-4));
    REQUIRE_THAT (revealScroll (1, 3.0f * pitch, viewport, 5, height), WithinAbs (pitch, 1e-4));
    REQUIRE_THAT (revealScroll (1, pitch, viewport, 5, height), WithinAbs (pitch, 1e-4));
}

TEST_CASE ("A drag across a take lane promotes the ordered span inside the take", "[takes][editor]")
{
    REQUIRE (dragSpan (300, 100, 0, 1000) == std::pair<std::int64_t, std::int64_t> { 100, 300 });
    REQUIRE (dragSpan (-50, 200, 0, 1000) == std::pair<std::int64_t, std::int64_t> { 0, 200 });
    REQUIRE (dragSpan (900, 1500, 0, 1000) == std::pair<std::int64_t, std::int64_t> { 900, 1000 });
    const auto missed = dragSpan (1200, 1500, 0, 1000);
    REQUIRE (missed.first == missed.second);
    const auto click = dragSpan (400, 400, 0, 1000);
    REQUIRE (click.first == click.second);
}
