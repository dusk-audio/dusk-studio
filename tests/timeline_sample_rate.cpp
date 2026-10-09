#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/TimelineSampleRate.h"

#include <limits>

using Catch::Matchers::WithinAbs;
using duskstudio::kFallbackTimelineSampleRate;
using duskstudio::timelineSampleRate;

TEST_CASE ("The timeline measures time at the first known rate", "[timeline][rate]")
{
    SECTION ("a running device wins over the rate it last ran at and the session's")
    {
        CHECK_THAT (timelineSampleRate (96000.0, 44100.0, 48000.0), WithinAbs (96000.0, 1e-9));
    }
    SECTION ("with no device running it is the rate the device last ran at")
    {
        CHECK_THAT (timelineSampleRate (0.0, 44100.0, 48000.0), WithinAbs (44100.0, 1e-9));
    }
    SECTION ("with no device since launch it is the session's saved rate")
    {
        CHECK_THAT (timelineSampleRate (0.0, 0.0, 88200.0), WithinAbs (88200.0, 1e-9));
    }
    SECTION ("with no rate anywhere it is 48 kHz, never zero")
    {
        CHECK_THAT (timelineSampleRate (0.0, 0.0, 0.0), WithinAbs (48000.0, 1e-9));
        CHECK_THAT (kFallbackTimelineSampleRate, WithinAbs (48000.0, 1e-9));
    }
    SECTION ("a negative or NaN rate counts as unknown")
    {
        CHECK_THAT (timelineSampleRate (-1.0, std::numeric_limits<double>::quiet_NaN(), 44100.0),
                    WithinAbs (44100.0, 1e-9));
    }
}
