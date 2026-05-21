#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

using Catch::Matchers::WithinAbs;
using duskstudio::FadeShape;
using duskstudio::applyFadeShape;

TEST_CASE ("FadeShape: every shape is 0 at t=0 and 1 at t=1", "[FadeShape]")
{
    for (auto s : { FadeShape::Linear, FadeShape::EqualPower, FadeShape::Sigmoid,
                     FadeShape::Exp, FadeShape::Log, FadeShape::RaisedCosine })
    {
        REQUIRE_THAT (applyFadeShape (0.0f, s), WithinAbs (0.0f, 1.0e-6f));
        REQUIRE_THAT (applyFadeShape (1.0f, s), WithinAbs (1.0f, 1.0e-6f));
    }
}

// RaisedCosine's defining property is zero slope at BOTH endpoints
// (unlike EqualPower which has non-zero slope at t=0). That zero
// derivative is what makes it the right curve for very-short
// click-mask crossfades like punch-in/punch-out. The check below
// uses a small numerical derivative; the threshold is generous
// enough to tolerate float-precision noise but tight enough to
// catch a regression that flips this curve to a non-zero-slope one.
TEST_CASE ("FadeShape::RaisedCosine has zero slope at both endpoints", "[FadeShape]")
{
    constexpr float h = 1.0e-4f;
    const float slopeAtZero = (applyFadeShape (h, FadeShape::RaisedCosine)
                                 - applyFadeShape (0.0f, FadeShape::RaisedCosine)) / h;
    const float slopeAtOne  = (applyFadeShape (1.0f, FadeShape::RaisedCosine)
                                 - applyFadeShape (1.0f - h, FadeShape::RaisedCosine)) / h;

    // Symbolic derivative at endpoints is 0; numerical second-order
    // error is O(h * π²/2) ~ 5e-4 here.
    REQUIRE_THAT (slopeAtZero, WithinAbs (0.0f, 1.0e-3f));
    REQUIRE_THAT (slopeAtOne,  WithinAbs (0.0f, 1.0e-3f));
}

// At t=0.5 the raised-cosine curve passes through 0.5 (the cosine
// crosses zero, so 0.5 * (1 - 0) = 0.5). Useful as a midpoint sanity
// check that pins the shape to the cosine formula rather than some
// other zero-slope curve (e.g. a quintic).
TEST_CASE ("FadeShape::RaisedCosine is 0.5 at t=0.5", "[FadeShape]")
{
    REQUIRE_THAT (applyFadeShape (0.5f, FadeShape::RaisedCosine),
                  WithinAbs (0.5f, 1.0e-6f));
}

TEST_CASE ("FadeShape: out-of-range t is clamped", "[FadeShape]")
{
    REQUIRE_THAT (applyFadeShape (-1.0f, FadeShape::Linear), WithinAbs (0.0f, 1.0e-6f));
    REQUIRE_THAT (applyFadeShape ( 2.0f, FadeShape::Linear), WithinAbs (1.0f, 1.0e-6f));
}

TEST_CASE ("FadeShape: equal-power crossfade sums to constant power", "[FadeShape]")
{
    // Fade-in's gain at t  vs fade-out's gain at t (i.e. 1-t in shape terms).
    // For equal-power: g_in(t)^2 + g_out(t)^2 ~= 1 across the overlap.
    for (int i = 0; i <= 20; ++i)
    {
        const float t   = (float) i / 20.0f;
        const float gIn  = applyFadeShape (t,       FadeShape::EqualPower);
        const float gOut = applyFadeShape (1.0f - t, FadeShape::EqualPower);
        const float power = gIn * gIn + gOut * gOut;
        REQUIRE_THAT (power, WithinAbs (1.0f, 1.0e-5f));
    }
}

TEST_CASE ("FadeShape: linear crossfade sums to constant amplitude", "[FadeShape]")
{
    // Linear in + linear out = 1.0 in amplitude at every t. Adjacent
    // regions with linear fades crossfade by amplitude (-6 dB at mid),
    // which is what callers get when they choose Linear.
    for (int i = 0; i <= 20; ++i)
    {
        const float t   = (float) i / 20.0f;
        const float gIn  = applyFadeShape (t,       FadeShape::Linear);
        const float gOut = applyFadeShape (1.0f - t, FadeShape::Linear);
        REQUIRE_THAT (gIn + gOut, WithinAbs (1.0f, 1.0e-6f));
    }
}

TEST_CASE ("FadeShape: sigmoid is symmetric around t=0.5", "[FadeShape]")
{
    for (int i = 0; i <= 10; ++i)
    {
        const float t = (float) i / 10.0f;
        const float a = applyFadeShape (t,         FadeShape::Sigmoid);
        const float b = applyFadeShape (1.0f - t,  FadeShape::Sigmoid);
        REQUIRE_THAT (a + b, WithinAbs (1.0f, 1.0e-6f));
    }
    REQUIRE_THAT (applyFadeShape (0.5f, FadeShape::Sigmoid), WithinAbs (0.5f, 1.0e-6f));
}

TEST_CASE ("FadeShape: exp/log are inverses around the diagonal", "[FadeShape]")
{
    // Exp(t) = t^2, Log(t) = 1 - (1 - t)^2. By construction Log(t) = 1 - Exp(1-t).
    for (int i = 0; i <= 10; ++i)
    {
        const float t = (float) i / 10.0f;
        const float ex = applyFadeShape (t,        FadeShape::Exp);
        const float lg = applyFadeShape (1.0f - t, FadeShape::Log);
        REQUIRE_THAT (ex + lg, WithinAbs (1.0f, 1.0e-6f));
    }
}

TEST_CASE ("FadeShape: linear matches plain t", "[FadeShape]")
{
    // Smoke - the linear branch is a fast path for the historical default.
    for (int i = 0; i <= 10; ++i)
    {
        const float t = (float) i / 10.0f;
        REQUIRE_THAT (applyFadeShape (t, FadeShape::Linear),
                       WithinAbs (t, 1.0e-6f));
    }
}
