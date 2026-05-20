#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::FadeShape;
using duskstudio::applyFadeShape;

namespace
{
// Mirrors PlaybackEngine's per-sample fade math: leading region fades out
// over [end - overlap, end), trailing region fades in over [start, start
// + overlap). Sum across the overlap window simulates what readForTrack
// writes into the output buffer when both regions hit the same sample.
struct OverlapSum
{
    int   overlapSamples = 0;
    FadeShape shape       = FadeShape::EqualPower;

    float at (int i) const noexcept
    {
        const float t   = (float) i / (float) overlapSamples;
        const float gIn  = applyFadeShape (t,        shape);
        const float gOut = applyFadeShape (1.0f - t, shape);
        // Both regions carry a constant 1.0 sample so the sum IS the
        // gain-sum across the overlap window.
        return gIn + gOut;
    }
};
}

TEST_CASE ("Crossfade: equal-power overlap holds constant power across window",
          "[Crossfade]")
{
    OverlapSum xf { 128, FadeShape::EqualPower };
    // Power sum (gIn^2 + gOut^2) is the perceived-loudness measure for
    // uncorrelated signals; equal-power keeps it ~1 across the entire
    // overlap. Test the same property the audio thread relies on.
    for (int i = 0; i <= xf.overlapSamples; ++i)
    {
        const float t   = (float) i / (float) xf.overlapSamples;
        const float gIn  = applyFadeShape (t,        FadeShape::EqualPower);
        const float gOut = applyFadeShape (1.0f - t, FadeShape::EqualPower);
        const float power = gIn * gIn + gOut * gOut;
        REQUIRE_THAT (power, WithinAbs (1.0f, 1.0e-5f));
    }
}

TEST_CASE ("Crossfade: linear overlap holds constant amplitude across window",
          "[Crossfade]")
{
    // Two correlated copies of the same signal (e.g. a duplicated take)
    // crossfade cleanly with linear shapes because amplitude sums to 1.
    OverlapSum xf { 64, FadeShape::Linear };
    for (int i = 0; i <= xf.overlapSamples; ++i)
        REQUIRE_THAT (xf.at (i), WithinAbs (1.0f, 1.0e-6f));
}

TEST_CASE ("Crossfade: at-boundary samples produce unity for both shapes",
          "[Crossfade]")
{
    OverlapSum xfEqual  { 32, FadeShape::EqualPower };
    OverlapSum xfLinear { 32, FadeShape::Linear };
    // The trailing region's first sample (t=0): fade-in=0, fade-out=1
    // for whichever shape — the leading region carries the audio alone.
    REQUIRE_THAT (xfEqual.at (0),  WithinAbs (1.0f, 1.0e-6f));
    REQUIRE_THAT (xfLinear.at (0), WithinAbs (1.0f, 1.0e-6f));
    // Mirror at t=1.
    REQUIRE_THAT (xfEqual.at (32),  WithinAbs (1.0f, 1.0e-6f));
    REQUIRE_THAT (xfLinear.at (32), WithinAbs (1.0f, 1.0e-6f));
}
