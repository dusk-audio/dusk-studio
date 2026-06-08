#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

#include <vector>

using duskstudio::AutomationLane;
using duskstudio::AutomationPoint;
using duskstudio::AutomationParam;
using duskstudio::evaluateLane;
using Catch::Matchers::WithinAbs;

namespace
{
AutomationPoint pt (juce::int64 t, float v)
{
    AutomationPoint p;
    p.timeSamples   = t;
    p.value         = v;
    p.recordedAtBPM = 120.0f;
    return p;
}
} // namespace

// The lane is now an AtomicSnapshot; these pin the accessor contract the
// audio thread (pointsForRead) and the message thread (pointsConst /
// mutableForWritePass / publishPoints / mutatePoints) rely on.
TEST_CASE ("AutomationLane: default lane reads a non-null empty vector", "[automation][snapshot]")
{
    AutomationLane lane;
    REQUIRE (lane.snapshot.read() != nullptr);          // audio thread never sees null
    REQUIRE (lane.pointsForRead().empty());
    REQUIRE (lane.pointsConst().empty());
}

TEST_CASE ("AutomationLane: publishPoints swaps the audio-visible vector", "[automation][snapshot]")
{
    AutomationLane lane;
    const auto* before = lane.snapshot.read();

    lane.publishPoints ({ pt (0, 0.0f), pt (48000, 1.0f) });

    // read() now points at the freshly published vector with the new contents.
    REQUIRE (lane.pointsForRead().size() == 2);
    REQUIRE (lane.pointsConst().size() == 2);
    REQUIRE (lane.snapshot.read() != before);           // pointer actually swapped

    // A second publish keeps reads coherent (one-publish-behind retire keeps
    // the prior alive; the latest is always what read() returns).
    lane.publishPoints ({ pt (0, 0.5f) });
    REQUIRE (lane.pointsForRead().size() == 1);
    REQUIRE_THAT (lane.pointsForRead().front().value, WithinAbs (0.5f, 1e-6f));
}

TEST_CASE ("AutomationLane: mutableForWritePass appends in place, no swap", "[automation][snapshot]")
{
    AutomationLane lane;
    const auto* owned = lane.snapshot.read();

    // The Write-mode capture path mutates the owned vector in place — the
    // audio thread observes it through its existing acquire-loaded pointer,
    // so the pointer must NOT change.
    lane.mutableForWritePass().push_back (pt (1000, 0.25f));

    REQUIRE (lane.snapshot.read() == owned);            // same buffer, mutated in place
    REQUIRE (lane.pointsForRead().size() == 1);
    REQUIRE (lane.pointsConst().size() == 1);
}

TEST_CASE ("AutomationLane: mutatePoints copy-applies-publishes", "[automation][snapshot]")
{
    AutomationLane lane;
    lane.publishPoints ({ pt (0, 0.0f), pt (10, 0.1f), pt (20, 0.2f) });

    lane.mutatePoints ([] (std::vector<AutomationPoint>& v)
                        { v.erase (v.begin() + 1); });   // drop the middle point

    REQUIRE (lane.pointsForRead().size() == 2);
    REQUIRE (lane.pointsForRead()[0].timeSamples == 0);
    REQUIRE (lane.pointsForRead()[1].timeSamples == 20);
}

// evaluateLane is the audio-thread reader's hot path; pin its boundaries.
TEST_CASE ("evaluateLane: continuous interpolation + hold-first/last", "[automation][eval]")
{
    // FaderDb lane: 0.0 at t=0 → 1.0 at t=48000. evaluateLane returns the
    // DENORMALISED value (dB), so compare against the normalised→dB mapping at
    // the endpoints and the midpoint fraction in normalised space.
    const std::vector<AutomationPoint> pts { pt (0, 0.25f), pt (48000, 0.75f) };

    const float atStart = evaluateLane (pts, -100, AutomationParam::FaderDb);   // before first → hold-first
    const float atFirst = evaluateLane (pts, 0,    AutomationParam::FaderDb);
    REQUIRE_THAT (atStart, WithinAbs (atFirst, 1e-4f));

    const float atEnd  = evaluateLane (pts, 999999, AutomationParam::FaderDb);  // after last → hold-last
    const float atLast = evaluateLane (pts, 48000,  AutomationParam::FaderDb);
    REQUIRE_THAT (atEnd, WithinAbs (atLast, 1e-4f));

    // Midpoint in time → midpoint in normalised value (0.5) → its dB. Monotone
    // mapping, so the midpoint dB sits strictly between the two endpoints.
    const float mid = evaluateLane (pts, 24000, AutomationParam::FaderDb);
    REQUIRE (((atFirst < mid && mid < atLast) || (atLast < mid && mid < atFirst)));
}

TEST_CASE ("evaluateLane: discrete holds previous (step), thresholds at 0.5", "[automation][eval]")
{
    // Mute lane: 0 until t=1000, then 1. Discrete → step-hold, never interpolates.
    const std::vector<AutomationPoint> pts { pt (0, 0.0f), pt (1000, 1.0f) };

    REQUIRE (evaluateLane (pts, 500,  AutomationParam::Mute) < 0.5f);   // still previous (0)
    REQUIRE (evaluateLane (pts, 999,  AutomationParam::Mute) < 0.5f);   // hold-previous up to the next point
    REQUIRE (evaluateLane (pts, 1000, AutomationParam::Mute) >= 0.5f);  // at/after the toggle
    REQUIRE (evaluateLane (pts, 5000, AutomationParam::Mute) >= 0.5f);  // hold-last
}

TEST_CASE ("evaluateLane: empty lane returns 0", "[automation][eval]")
{
    const std::vector<AutomationPoint> empty;
    REQUIRE_THAT (evaluateLane (empty, 0, AutomationParam::FaderDb), WithinAbs (0.0f, 1e-9f));
}
