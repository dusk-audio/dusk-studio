#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/PumpPacing.h"

#include <chrono>

using duskstudio::imgui::nextPumpAfter;
using std::chrono::milliseconds;

TEST_CASE ("an embedded view's pump waits out only the part of a frame past its interval", "[ui][pacing]")
{
    const auto started = std::chrono::steady_clock::time_point {} + milliseconds (1000);
    const auto pumpAfter = [started] (int frameMs, int intervalMs)
    {
        const auto finished = started + milliseconds (frameMs);
        return nextPumpAfter (started, finished, intervalMs) - finished;
    };

    SECTION ("a frame within the interval waits for nothing")
    {
        CHECK (pumpAfter (0, 16) == milliseconds (0));
        CHECK (pumpAfter (16, 16) == milliseconds (0));
    }

    SECTION ("a slower frame leaves the thread its overrun")
    {
        CHECK (pumpAfter (17, 16) == milliseconds (1));
        CHECK (pumpAfter (40, 16) == milliseconds (24));
    }

    SECTION ("a stalled frame counts for no more than 100 ms")
    {
        CHECK (pumpAfter (100, 16) == milliseconds (84));
        CHECK (pumpAfter (5000, 16) == milliseconds (84));
    }
}
