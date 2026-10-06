#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/RulerDensity.h"

#include <cstdint>
#include <limits>

using namespace duskstudio::imgui::ruler;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Bar numbers thin by powers of two as bars fall under 64 px", "[editor][ruler]")
{
    CHECK (barStep (200.0, 8) == 1);
    CHECK (barStep (64.0, 8) == 1);
    CHECK (barStep (63.9, 8) == 2);
    CHECK (barStep (32.0, 8) == 2);
    CHECK (barStep (31.9, 8) == 4);
    CHECK (barStep (16.0, 8) == 4);
    CHECK (barStep (15.9, 8) == 8);
    CHECK (barStep (8.0, 8) == 8);
    CHECK (barStep (7.9, 8) == 16);
    // Past the timeline ruler's last step of 16.
    CHECK (barStep (3.9, 8) == 32);
    CHECK (barStep (0.001, 8) == 65536);

    for (double pixelsPerBar = 0.01; pixelsPerBar < 500.0; pixelsPerBar *= 1.37)
    {
        const auto step = barStep (pixelsPerBar, 8);
        CHECK (static_cast<double> (step) * pixelsPerBar >= kMinMarkPixels);
        CHECK ((step == 1 || static_cast<double> (step / 2) * pixelsPerBar < kMinMarkPixels));
    }
}

TEST_CASE ("Bar numbers stay under the ceiling whatever the bar width says", "[editor][ruler]")
{
    // A tempo map can put far more bars in view than the session tempo allows for.
    const std::int64_t bars = 14'400'000;
    const auto step = barStep (100.0, bars);
    CHECK (bars / step <= kMaxMarks);
    CHECK (bars / (step / 2) > kMaxMarks);

    // No width to go by: the count alone decides, and the loop still ends.
    CHECK (barStep (0.0, 10) == 1);
    CHECK (barStep (-4.0, 10) == 1);
    CHECK (barStep (std::numeric_limits<double>::quiet_NaN(), 5000) == 8);
    CHECK (barStep (std::numeric_limits<double>::infinity(), 10) == 1);

    const auto most = std::numeric_limits<std::int64_t>::max();
    CHECK (most / barStep (1.0e-300, most) <= kMaxMarks);
}

TEST_CASE ("Time stamps step as the timeline's, then by minutes and hours", "[editor][ruler]")
{
    const auto step = [] (double pixelsPerSecond) { return stampSeconds (pixelsPerSecond, 60.0); };

    CHECK_THAT (step (120.0), WithinAbs (1.0, 1e-9));
    CHECK_THAT (step (40.0), WithinAbs (1.0, 1e-9));
    CHECK_THAT (step (39.9), WithinAbs (5.0, 1e-9));
    CHECK_THAT (step (16.0), WithinAbs (5.0, 1e-9));
    CHECK_THAT (step (15.9), WithinAbs (10.0, 1e-9));
    CHECK_THAT (step (6.0), WithinAbs (10.0, 1e-9));
    CHECK_THAT (step (5.9), WithinAbs (30.0, 1e-9));
    CHECK_THAT (step (2.2), WithinAbs (30.0, 1e-9));
    // 30 s stamps would sit closer than a stamp's width from here on.
    CHECK_THAT (step (2.1), WithinAbs (60.0, 1e-9));
    CHECK_THAT (step (1.0), WithinAbs (120.0, 1e-9));
    CHECK_THAT (step (0.5), WithinAbs (300.0, 1e-9));
    CHECK_THAT (step (0.2), WithinAbs (600.0, 1e-9));
    CHECK_THAT (step (0.1), WithinAbs (1800.0, 1e-9));
    CHECK_THAT (step (0.03), WithinAbs (3600.0, 1e-9));
    CHECK_THAT (step (0.01), WithinAbs (7200.0, 1e-9));
    CHECK_THAT (step (0.005), WithinAbs (14400.0, 1e-9));

    for (double pixelsPerSecond = 1.0e-4; pixelsPerSecond < 2.0; pixelsPerSecond *= 1.37)
        CHECK (step (pixelsPerSecond) * pixelsPerSecond >= kMinMarkPixels);
    // The timeline ruler's own steps come as close as 40 px, at one stamp a second.
    for (double pixelsPerSecond = 1.0e-4; pixelsPerSecond < 500.0; pixelsPerSecond *= 1.07)
        CHECK (step (pixelsPerSecond) * pixelsPerSecond >= 40.0);
}

TEST_CASE ("Time stamps stay under the ceiling whatever the zoom says", "[editor][ruler]")
{
    const auto ceiling = static_cast<double> (kMaxMarks);

    const double longView = 1.0e7;
    const double step = stampSeconds (100.0, longView);
    CHECK (longView / step <= ceiling);
    CHECK (longView / (step / 2.0) > ceiling);

    // Three minutes at 48 kHz read at 1 Hz, fitted to a 1000 px view.
    const double misread = 8'640'000.0;
    CHECK (misread / stampSeconds (1000.0 / misread, misread) < 20.0);

    // No zoom to go by: 30 s, then the span decides, and the loop still ends.
    CHECK_THAT (stampSeconds (0.0, 600.0), WithinAbs (30.0, 1e-9));
    CHECK_THAT (stampSeconds (std::numeric_limits<double>::quiet_NaN(), 600.0), WithinAbs (30.0, 1e-9));
    CHECK (1.0e9 / stampSeconds (0.0, 1.0e9) <= ceiling);
    CHECK (stampSeconds (1.0, std::numeric_limits<double>::infinity()) > 0.0);
    CHECK (stampSeconds (1.0e-300, 1.0e300) > 0.0);
}
