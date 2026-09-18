#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/TimelineFollow.h"

#include <cstdint>

using duskstudio::followPlayheadScroll;
using duskstudio::tapeStripFitZoom;

namespace
{
constexpr std::int64_t kVisible = 32000;   // a view one second wide at 32 kHz
} // namespace

TEST_CASE ("The view holds still while the playhead is inside it", "[timeline][follow]")
{
    CHECK (followPlayheadScroll (0,          0,     kVisible) == 0);
    CHECK (followPlayheadScroll (kVisible / 2, 0,   kVisible) == 0);
    CHECK (followPlayheadScroll (48000,      40000, kVisible) == 40000);
}

TEST_CASE ("The page turns as the playhead nears the right edge", "[timeline][follow]")
{
    const std::int64_t margin = kVisible / 32;

    SECTION ("just short of the margin stays put")
    {
        CHECK (followPlayheadScroll (kVisible - margin - 1, 0, kVisible) == 0);
    }

    SECTION ("inside the margin puts the playhead a quarter of the way in")
    {
        const std::int64_t playhead = kVisible - margin;
        const auto scroll = followPlayheadScroll (playhead, 0, kVisible);
        CHECK (scroll == playhead - kVisible / 4);
        CHECK (followPlayheadScroll (playhead, scroll, kVisible) == scroll);
    }

    SECTION ("a playhead far past the view lands a quarter of the way in")
    {
        CHECK (followPlayheadScroll (200000, 0, kVisible) == 200000 - kVisible / 4);
    }
}

TEST_CASE ("A playhead left of the view brings the view back to it", "[timeline][follow]")
{
    SECTION ("a quarter of the way in")
    {
        CHECK (followPlayheadScroll (100000, 150000, kVisible) == 100000 - kVisible / 4);
    }

    SECTION ("never before the timeline origin")
    {
        CHECK (followPlayheadScroll (1000, 150000, kVisible) == 0);
    }
}

TEST_CASE ("A view with no width leaves the scroll alone", "[timeline][follow]")
{
    CHECK (followPlayheadScroll (500000, 1234, 0) == 1234);
}

TEST_CASE ("Fit on an empty tape strip keeps the unzoomed minute", "[timeline][zoom]")
{
    using Catch::Matchers::WithinAbs;

    // Nothing to fit is the one-minute window, not the 32x clamp.
    CHECK_THAT (tapeStripFitZoom (0.0), WithinAbs (1.0, 1e-6));
    CHECK_THAT (tapeStripFitZoom (-1.0), WithinAbs (1.0, 1e-6));

    SECTION ("content fills the strip")
    {
        CHECK_THAT (tapeStripFitZoom (10.0), WithinAbs (6.0, 1e-5));
        CHECK_THAT (tapeStripFitZoom (100.0), WithinAbs (1.2, 1e-5));
    }

    SECTION ("very short content stops at the tightest zoom")
    {
        CHECK_THAT (tapeStripFitZoom (0.5), WithinAbs (32.0, 1e-5));
    }
}
