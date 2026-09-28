#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/WheelScroll.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace duskstudio::wheel;

namespace
{
constexpr float kNotchPx = 44.0f;

// What JUCE's macOS peer reports for a precise scroll of this many points.
float trackpadDelta (float points) { return points * kSmoothDeltaPerPoint; }
}

TEST_CASE ("wheel: one notch moves the same distance on every platform", "[wheel_scroll]")
{
    SECTION ("an X11 click")
    {
        Accumulator acc;
        CHECK (acc.pixels (50.0f / 256.0f, false, kNotchPx, kLinuxProfile) == 44);
    }
    SECTION ("a Windows click")
    {
        Accumulator acc;
        CHECK (acc.pixels (0.5f * 120.0f / 256.0f, false, kNotchPx, kWindowsProfile) == 44);
    }
    SECTION ("a macOS click of one line")
    {
        Accumulator acc;
        CHECK (acc.pixels (10.0f / 256.0f, false, kNotchPx, kMacProfile) == 44);
    }
}

TEST_CASE ("wheel: a macOS click is worth at least a notch and keeps its acceleration", "[wheel_scroll]")
{
    Accumulator acc;
    CHECK (acc.pixels (0.1f * 10.0f / 256.0f, false, kNotchPx, kMacProfile) == 44);
    CHECK (acc.pixels (5.0f * 10.0f / 256.0f, false, kNotchPx, kMacProfile) == 220);
    CHECK (acc.pixels (-0.1f * 10.0f / 256.0f, false, kNotchPx, kMacProfile) == -44);
}

TEST_CASE ("wheel: a macOS trackpad stream follows the fingers a pixel per point", "[wheel_scroll]")
{
    SECTION ("single points never truncate to nothing")
    {
        Accumulator acc;
        int total = 0;
        for (int i = 0; i < 30; ++i)
        {
            const int step = acc.pixels (trackpadDelta (1.0f), true, kNotchPx, kMacProfile);
            CHECK (step == 1);
            total += step;
        }
        CHECK (total == 30);
    }
    SECTION ("fractions of a point add up without drift")
    {
        Accumulator acc;
        int total = 0;
        for (int i = 0; i < 1000; ++i)
            total += acc.pixels (trackpadDelta (0.3f), true, kNotchPx, kMacProfile);
        CHECK (total >= 299);
        CHECK (total <= 300);
    }
    SECTION ("the smooth scale ignores the notch size")
    {
        Accumulator acc;
        CHECK (acc.pixels (trackpadDelta (12.0f), true, 3.0f, kMacProfile) == 12);
    }
}

TEST_CASE ("wheel: small line-based events add up to their notches", "[wheel_scroll]")
{
    SECTION ("a Windows touchpad's twelfths of a notch")
    {
        Accumulator acc;
        int total = 0;
        for (int i = 0; i < 12; ++i)
        {
            const int step = acc.pixels (0.5f * 10.0f / 256.0f, false, kNotchPx, kWindowsProfile);
            CHECK (step >= 3);
            CHECK (step <= 4);
            total += step;
        }
        CHECK (total == 44);
    }
    SECTION ("an event under a pixel still moves one, as the Viewport does")
    {
        Accumulator acc;
        CHECK (acc.pixels (0.001f, false, kNotchPx, kLinuxProfile) == 1);
        CHECK (acc.pixels (-0.001f, false, kNotchPx, kLinuxProfile) == -1);
    }
    SECTION ("a fractional notch size does not lose a pixel to rounding")
    {
        Accumulator acc;
        CHECK (acc.pixels (2.56f * 50.0f / 256.0f, false, 6.25f, kLinuxProfile) == 16);
    }
}

TEST_CASE ("wheel: direction follows the delta and a reversal answers at once", "[wheel_scroll]")
{
    Accumulator acc;
    CHECK (acc.pixels (trackpadDelta (0.9f), true, kNotchPx, kMacProfile) == 0);
    CHECK (acc.pixels (trackpadDelta (-1.0f), true, kNotchPx, kMacProfile) == -1);
    CHECK (acc.pixels (trackpadDelta (0.5f), true, kNotchPx, kMacProfile) == 0);
    CHECK (acc.pixels (trackpadDelta (0.5f), true, kNotchPx, kMacProfile) == 1);

    Accumulator line;
    CHECK (line.pixels (-50.0f / 256.0f, false, kNotchPx, kLinuxProfile) == -44);
    CHECK (line.pixels (50.0f / 256.0f, false, kNotchPx, kLinuxProfile) == 44);
}

TEST_CASE ("wheel: reset and nonsense deltas leave nothing owed", "[wheel_scroll]")
{
    Accumulator acc;
    CHECK (acc.pixels (trackpadDelta (0.9f), true, kNotchPx, kMacProfile) == 0);
    acc.reset();
    CHECK (acc.pixels (trackpadDelta (0.5f), true, kNotchPx, kMacProfile) == 0);
    CHECK (acc.pixels (0.0f, false, kNotchPx, kLinuxProfile) == 0);
    CHECK (acc.pixels (std::nanf (""), true, kNotchPx, kMacProfile) == 0);
    CHECK (acc.pixels (INFINITY, false, kNotchPx, kLinuxProfile) == 0);
    CHECK (acc.pixels (trackpadDelta (0.5f), true, kNotchPx, kMacProfile) == 1);
}

TEST_CASE ("wheel: a trackpad zooms by the notches it travels, not a step per event", "[wheel_scroll]")
{
    CHECK_THAT (zoomFactor (50.0f / 256.0f, false, 1.15f, kLinuxProfile), WithinRel (1.15f, 1.0e-6f));
    CHECK_THAT (zoomFactor (-50.0f / 256.0f, false, 1.15f, kLinuxProfile), WithinRel (1.0f / 1.15f, 1.0e-6f));
    CHECK_THAT (zoomFactor (0.2f * 10.0f / 256.0f, false, 1.15f, kMacProfile), WithinRel (1.15f, 1.0e-6f));

    double zoom = 1.0;
    for (int i = 0; i < static_cast<int> (kPointsPerNotch); ++i)
        zoom *= zoomFactor (trackpadDelta (1.0f), true, 1.15f, kMacProfile);
    CHECK_THAT (zoom, WithinRel (1.15, 1.0e-4));

    CHECK_THAT (zoomFactor (0.0f, true, 1.15f, kMacProfile), WithinAbs (1.0f, 1.0e-9f));
}
