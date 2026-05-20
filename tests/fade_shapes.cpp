#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"

using Catch::Matchers::WithinAbs;
using duskstudio::FadeShape;
using duskstudio::applyFadeShape;

TEST_CASE ("FadeShape: every shape is 0 at t=0 and 1 at t=1", "[FadeShape]")
{
    for (auto s : { FadeShape::Linear, FadeShape::EqualPower, FadeShape::Sigmoid,
                     FadeShape::Exp, FadeShape::Log })
    {
        REQUIRE_THAT (applyFadeShape (0.0f, s), WithinAbs (0.0f, 1.0e-6f));
        REQUIRE_THAT (applyFadeShape (1.0f, s), WithinAbs (1.0f, 1.0e-6f));
    }
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
