#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/AutomationLaneEdit.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::AutomationMode;
using duskstudio::AutomationParam;
using duskstudio::AutomationPoint;

namespace
{
AutomationPoint point (std::int64_t t, float v)
{
    AutomationPoint p;
    p.timeSamples = t;
    p.value = v;
    return p;
}

std::vector<std::int64_t> times (const std::vector<AutomationPoint>& points)
{
    std::vector<std::int64_t> out;
    for (const auto& p : points)
        out.push_back (p.timeSamples);
    return out;
}
} // namespace

TEST_CASE ("quantizeAutomationValue: on/off lanes store 0 or 1, the rest clamp", "[automation][editor]")
{
    REQUIRE_THAT (duskstudio::quantizeAutomationValue (AutomationParam::Mute, 0.49f), WithinAbs (0.0, 1e-6));
    REQUIRE_THAT (duskstudio::quantizeAutomationValue (AutomationParam::Solo, 0.5f), WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (duskstudio::quantizeAutomationValue (AutomationParam::FaderDb, 0.3f), WithinAbs (0.3, 1e-6));
    REQUIRE_THAT (duskstudio::quantizeAutomationValue (AutomationParam::Pan, 1.4f), WithinAbs (1.0, 1e-6));
    REQUIRE_THAT (duskstudio::quantizeAutomationValue (AutomationParam::AuxSend2, -0.2f), WithinAbs (0.0, 1e-6));
}

TEST_CASE ("insertAutomationPoint and moveAutomationPoint keep the lane sorted", "[automation][editor]")
{
    std::vector<AutomationPoint> lane { point (100, 0.1f), point (300, 0.3f) };

    REQUIRE (duskstudio::insertAutomationPoint (lane, point (200, 0.2f)) == 1);
    REQUIRE (duskstudio::insertAutomationPoint (lane, point (200, 0.9f)) == 2);
    REQUIRE (times (lane) == std::vector<std::int64_t> { 100, 200, 200, 300 });

    const int moved = duskstudio::moveAutomationPoint (lane, 0, 400, 0.7f);
    REQUIRE (moved == 3);
    REQUIRE (times (lane) == std::vector<std::int64_t> { 200, 200, 300, 400 });
    REQUIRE_THAT (lane[3].value, WithinAbs (0.7, 1e-6));

    REQUIRE (duskstudio::moveAutomationPoint (lane, moved, 50, 0.5f) == 0);
    REQUIRE (times (lane) == std::vector<std::int64_t> { 50, 200, 200, 300 });

    const auto before = lane;
    REQUIRE (duskstudio::moveAutomationPoint (lane, 4, 10, 0.0f) == -1);
    REQUIRE (duskstudio::moveAutomationPoint (lane, -1, 10, 0.0f) == -1);
    REQUIRE (lane == before);
}

TEST_CASE ("paintAutomationStep: a stroke replaces the automation it sweeps", "[automation][editor]")
{
    std::vector<AutomationPoint> lane { point (0, 0.5f), point (1500, 0.9f), point (2500, 0.9f), point (5000, 0.5f) };
    duskstudio::AutomationStroke stroke;

    duskstudio::paintAutomationStep (lane, stroke, 1000, 0.2f, 120.0f, 10.0f, 4.0f);
    duskstudio::paintAutomationStep (lane, stroke, 2000, 0.4f, 120.0f, 20.0f, 4.0f);
    duskstudio::paintAutomationStep (lane, stroke, 3000, 0.6f, 120.0f, 30.0f, 4.0f);

    REQUIRE (times (lane) == std::vector<std::int64_t> { 0, 1000, 2000, 3000, 5000 });
    REQUIRE_THAT (lane[2].value, WithinAbs (0.4, 1e-6));
    REQUIRE (std::is_sorted (lane.begin(), lane.end(), [] (const auto& a, const auto& b)
                             { return a.timeSamples < b.timeSamples; }));
}

TEST_CASE ("paintAutomationStep: a move under the step retunes the last point", "[automation][editor]")
{
    std::vector<AutomationPoint> lane;
    duskstudio::AutomationStroke stroke;

    duskstudio::paintAutomationStep (lane, stroke, 1000, 0.2f, 120.0f, 10.0f, 4.0f);
    duskstudio::paintAutomationStep (lane, stroke, 1100, 0.8f, 96.0f, 12.0f, 4.0f);

    REQUIRE (lane.size() == 1);
    REQUIRE (lane[0].timeSamples == 1000);
    REQUIRE_THAT (lane[0].value, WithinAbs (0.8, 1e-6));
    REQUIRE_THAT (lane[0].recordedAtBPM, WithinAbs (96.0, 1e-6));

    duskstudio::paintAutomationStep (lane, stroke, 1400, 0.3f, 120.0f, 14.0f, 4.0f);
    REQUIRE (times (lane) == std::vector<std::int64_t> { 1000, 1400 });
}

TEST_CASE ("automationModeAfterEdit: Off and Write become Read", "[automation][editor]")
{
    REQUIRE (duskstudio::automationModeAfterEdit (AutomationMode::Off) == AutomationMode::Read);
    REQUIRE (duskstudio::automationModeAfterEdit (AutomationMode::Write) == AutomationMode::Read);
    REQUIRE (duskstudio::automationModeAfterEdit (AutomationMode::Read) == AutomationMode::Read);
    REQUIRE (duskstudio::automationModeAfterEdit (AutomationMode::Touch) == AutomationMode::Touch);
}
