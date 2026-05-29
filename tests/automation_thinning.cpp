#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

#include <cmath>

// Catch2's headers transitively include <cmath> BEFORE this file's
// code runs, which on MSVC means M_PI conditional resolution has
// already happened without _USE_MATH_DEFINES set. Defining it here
// is a no-op — too late. Use a portable constant instead so neither
// toolchain has anything to disagree about.
#ifndef M_PI
constexpr double M_PI = 3.14159265358979323846;
#endif

using Catch::Matchers::WithinAbs;

// Ramer-Douglas-Peucker is supposed to drop interior points whose
// perpendicular distance to the connecting chord is below epsilon, and
// keep the rest. The tests below pin the algorithm's invariants so a
// future tweak (e.g. switching to a different polyline simplifier) is
// caught by ctest rather than by a user's bloated session.json.

namespace
{
duskstudio::AutomationPoint pt (juce::int64 t, float v)
{
    duskstudio::AutomationPoint p;
    p.timeSamples   = t;
    p.value         = v;
    p.recordedAtBPM = 120.0f;
    return p;
}
} // namespace

TEST_CASE ("thinAutomationLane: empty + tiny lanes are no-ops",
            "[automation][rdp]")
{
    duskstudio::AutomationLane empty;
    duskstudio::thinAutomationLane (empty, duskstudio::AutomationParam::FaderDb, 0.002);
    REQUIRE (empty.points.empty());

    duskstudio::AutomationLane two;
    two.points.push_back (pt (0,      0.0f));
    two.points.push_back (pt (44100,  1.0f));
    duskstudio::thinAutomationLane (two, duskstudio::AutomationParam::FaderDb, 0.002);
    REQUIRE (two.points.size() == 2);
}

TEST_CASE ("thinAutomationLane: collinear interior points are dropped",
            "[automation][rdp]")
{
    // Straight ramp from 0 → 1 over 10 samples-per-step; every interior
    // point lies exactly on the chord, so RDP should keep only the
    // endpoints.
    duskstudio::AutomationLane lane;
    for (int i = 0; i <= 10; ++i)
        lane.points.push_back (pt ((juce::int64) (i * 100),
                                     (float) i / 10.0f));

    duskstudio::thinAutomationLane (lane, duskstudio::AutomationParam::FaderDb, 0.002);
    REQUIRE (lane.points.size() == 2);
    REQUIRE (lane.points.front().timeSamples == 0);
    REQUIRE (lane.points.back ().timeSamples == 1000);
    REQUIRE_THAT (lane.points.front().value, WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (lane.points.back ().value, WithinAbs (1.0f, 1e-6f));
}

TEST_CASE ("thinAutomationLane: sine-shaped lane keeps the peak + endpoints",
            "[automation][rdp]")
{
    // 0 → 1 → 0 hump over 21 points (a Hann window — a full cycle of
    // cos run through 0.5*(1-cos(2π*frac))). The peak at index 10 must
    // survive any epsilon below 1.0 (the peak amplitude); both endpoints
    // must survive unconditionally.
    duskstudio::AutomationLane lane;
    for (int i = 0; i <= 20; ++i)
    {
        const float frac = (float) i / 20.0f;
        const float v    = 0.5f * (1.0f - std::cos ((float) M_PI * frac * 2.0f));
        lane.points.push_back (pt ((juce::int64) (i * 100), v));
    }

    duskstudio::thinAutomationLane (lane, duskstudio::AutomationParam::FaderDb, 0.02);
    // 21 points in, RDP at epsilon=0.02 keeps ~13 (matches the curvature
    // analysis: spacing needed for 0.02 error on a unit-amplitude sine
    // is roughly 0.06 of the span). The exact count is less important
    // than 'meaningfully fewer than 21 + the structural points survive'.
    REQUIRE (lane.points.size() >= 3);
    REQUIRE (lane.points.size() <  21);
    REQUIRE (lane.points.front().timeSamples == 0);
    REQUIRE (lane.points.back ().timeSamples == 2000);

    // The peak should still be present (somewhere near sample-index 1000).
    bool hasPeak = false;
    for (const auto& p : lane.points)
        if (std::abs (p.value - 1.0f) < 0.05f) hasPeak = true;
    REQUIRE (hasPeak);
}

TEST_CASE ("thinAutomationLane: discrete lanes are NOT thinned",
            "[automation][rdp]")
{
    // Mute is discrete; RDP would average two 0/1 transitions into a
    // 0.5 nonsense value and silently lose the toggle. thinAutomationLane
    // must early-return on non-continuous params.
    duskstudio::AutomationLane lane;
    lane.points.push_back (pt (0,      0.0f));
    lane.points.push_back (pt (10000,  0.0f));
    lane.points.push_back (pt (20000,  1.0f));
    lane.points.push_back (pt (30000,  1.0f));
    lane.points.push_back (pt (40000,  0.0f));

    const auto before = lane.points.size();
    duskstudio::thinAutomationLane (lane, duskstudio::AutomationParam::Mute, 0.002);
    REQUIRE (lane.points.size() == before);
}

TEST_CASE ("thinAutomationLane: large epsilon over-thins to 2 points",
            "[automation][rdp]")
{
    // Same 0 → 1 → 0 hump as the previous test, but with epsilon ≥ peak
    // amplitude the central peak gets thinned out — only the two anchored
    // endpoints remain.
    duskstudio::AutomationLane lane;
    for (int i = 0; i <= 20; ++i)
    {
        const float frac = (float) i / 20.0f;
        const float v    = 0.5f * (1.0f - std::cos ((float) M_PI * frac * 2.0f));
        lane.points.push_back (pt ((juce::int64) (i * 100), v));
    }

    duskstudio::thinAutomationLane (lane, duskstudio::AutomationParam::FaderDb, 5.0);
    REQUIRE (lane.points.size() == 2);
}
