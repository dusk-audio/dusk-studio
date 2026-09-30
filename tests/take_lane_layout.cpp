#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/TakeLaneLayout.h"

using namespace duskstudio::imgui::takelanes;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Take lanes leave the band to the region view when there are no takes", "[takes][editor]")
{
    const auto none = split (400.0f, 0);
    REQUIRE_THAT (none.region, WithinAbs (400.0f, 1e-6));
    REQUIRE_THAT (none.caption, WithinAbs (0.0f, 1e-6));
    REQUIRE_THAT (none.lanes, WithinAbs (0.0f, 1e-6));

    const auto one = split (400.0f, 1);
    REQUIRE_THAT (one.caption, WithinAbs (kCaptionHeight, 1e-6));
    REQUIRE_THAT (one.lanes, WithinAbs (kLaneHeight, 1e-6));
    REQUIRE_THAT (one.region + one.caption + one.lanes, WithinAbs (400.0f, 1e-4));

    const auto many = split (400.0f, 20);
    REQUIRE_THAT (many.lanes, WithinAbs ((400.0f - kCaptionHeight) * kMaxLaneShare, 1e-4));
    REQUIRE (many.region > 0.0f);
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
    REQUIRE (laneAt (0.0f, 0.0f, 3) == 0);
    REQUIRE (laneAt (kLaneHeight - 0.5f, 0.0f, 3) == 0);
    REQUIRE (laneAt (kLaneHeight + kLaneGap * 0.5f, 0.0f, 3) == -1);
    REQUIRE (laneAt (kLanePitch, 0.0f, 3) == 1);
    REQUIRE (laneAt (kLanePitch * 3.0f, 0.0f, 3) == -1);
    REQUIRE (laneAt (-1.0f, 0.0f, 3) == -1);
    REQUIRE (laneAt (1.0f, kLanePitch * 2.0f, 3) == 2);
    REQUIRE_THAT (laneTop (2, kLanePitch), WithinAbs (kLanePitch, 1e-6));
}

TEST_CASE ("Revealing a take lane scrolls it wholly into view and no further", "[takes][editor]")
{
    const float viewport = kLanePitch * 2.0f;
    REQUIRE_THAT (maxScroll (viewport, 5), WithinAbs (contentHeight (5) - viewport, 1e-4));
    REQUIRE_THAT (maxScroll (viewport, 1), WithinAbs (0.0f, 1e-6));
    REQUIRE_THAT (clampScroll (-10.0f, viewport, 5), WithinAbs (0.0f, 1e-6));

    REQUIRE_THAT (revealScroll (0, 0.0f, viewport, 5), WithinAbs (0.0f, 1e-6));
    REQUIRE_THAT (revealScroll (4, 0.0f, viewport, 5), WithinAbs (maxScroll (viewport, 5), 1e-4));
    REQUIRE_THAT (revealScroll (2, 0.0f, viewport, 5),
                  WithinAbs (2.0f * kLanePitch + kLaneHeight - viewport, 1e-4));
    REQUIRE_THAT (revealScroll (1, 3.0f * kLanePitch, viewport, 5), WithinAbs (kLanePitch, 1e-4));
    REQUIRE_THAT (revealScroll (1, kLanePitch, viewport, 5), WithinAbs (kLanePitch, 1e-4));
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
