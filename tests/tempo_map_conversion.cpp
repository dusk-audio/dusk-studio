#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

#include <vector>

using duskstudio::TempoMap;
using duskstudio::TempoPoint;

namespace
{
constexpr double kSr = 48000.0;
}

TEST_CASE ("TempoMap: an empty map carries no tempo of its own", "[TempoMap]")
{
    TempoMap m;
    REQUIRE (m.empty());
    // Conversions return 0 when empty; constant tempo is handled by callers via
    // the free functions with Session::tempoBpm (single source of truth).
    REQUIRE (m.samplesToTicks (48000, kSr) == 0);
    REQUIRE (m.ticksToSamples (960,   kSr) == 0);
}

TEST_CASE ("TempoMap: a single point at 0 equals the constant tempo", "[TempoMap]")
{
    TempoMap m;
    m.setPoints ({ { 0, 90.0f } });
    REQUIRE_FALSE (m.empty());

    for (juce::int64 s : { (juce::int64) 1000, (juce::int64) 48000, (juce::int64) 200000 })
        REQUIRE (std::abs (m.samplesToTicks (s, kSr)
                            - duskstudio::samplesToTicks (s, kSr, 90.0f)) <= 1);
}

TEST_CASE ("TempoMap: piecewise integration across a tempo change", "[TempoMap]")
{
    // 120 bpm for the first second, then 60 bpm.
    TempoMap m;
    m.setPoints ({ { 0, 120.0f }, { 48000, 60.0f } });

    // 120 bpm => 2 beats/s => 960 ticks at the boundary.
    REQUIRE (std::abs (m.samplesToTicks (48000, kSr) - 960) <= 1);
    // + 1 s at 60 bpm => 1 beat => 480 more ticks.
    REQUIRE (std::abs (m.samplesToTicks (96000, kSr) - 1440) <= 1);

    REQUIRE (m.bpmAt (0)      == 120.0f);
    REQUIRE (m.bpmAt (47999)  == 120.0f);
    REQUIRE (m.bpmAt (48000)  == 60.0f);
    REQUIRE (m.bpmAt (1000000) == 60.0f);
}

TEST_CASE ("TempoMap: ticks<->samples round-trips through a tempo change", "[TempoMap]")
{
    TempoMap m;
    m.setPoints ({ { 0, 140.0f }, { 60000, 75.0f }, { 200000, 110.0f } });

    // Round-trip from ticks: ticks are the coarse quantum (~80 samples each at
    // 75 bpm), so ticks -> samples -> ticks recovers to within a single tick.
    // (The reverse, samples -> ticks -> samples, is lossy by up to one tick by
    // construction and isn't a well-posed equality.)
    for (juce::int64 ticks : { (juce::int64) 100, (juce::int64) 1500,
                               (juce::int64) 5000, (juce::int64) 13000 })
    {
        const auto s    = m.ticksToSamples (ticks, kSr);
        const auto back = m.samplesToTicks (s, kSr);
        REQUIRE (std::abs (back - ticks) <= 1);
    }
}

TEST_CASE ("TempoMap: setPoints sorts and clamps bpm, preserving positions", "[TempoMap]")
{
    TempoMap m;
    // Out of order, an out-of-range bpm, and an earliest point past 0.
    m.setPoints ({ { 96000, 1000.0f }, { 24000, 100.0f } });

    const auto& p = m.points();
    REQUIRE (p.size() == 2);
    REQUIRE (p[0].timelineSamples == 24000);          // position preserved, not moved
    REQUIRE (p[0].bpm == 100.0f);
    REQUIRE (p[1].bpm == TempoMap::kMaxBpm);           // 1000 clamped to 300

    // The span before the first point takes the first point's tempo, so the
    // first point's position doesn't change the integration here.
    REQUIRE (std::abs (m.samplesToTicks (24000, kSr)
                        - duskstudio::samplesToTicks (24000, kSr, 100.0f)) <= 1);
}
