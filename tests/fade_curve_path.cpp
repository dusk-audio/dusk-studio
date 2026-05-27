#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/FadeCurve.h"

using Catch::Matchers::WithinAbs;
using duskstudio::FadeShape;
using duskstudio::fadeviz::buildFadeCurve;

TEST_CASE ("buildFadeCurve: rising starts bottom-left, ends top-right", "[fade_curve]")
{
    juce::Rectangle<float> zone (10.0f, 20.0f, 100.0f, 50.0f);  // x,y,w,h
    auto p = buildFadeCurve (zone, FadeShape::Linear, /*rising*/ true);

    // First vertex = gain 0 → bottom; last = gain 1 → top.
    const auto first = p.getPointAlongPath (0.0f);
    const auto last  = p.getPointAlongPath (p.getLength());
    REQUIRE_THAT (first.x, WithinAbs (zone.getX(), 0.5f));
    REQUIRE_THAT (first.y, WithinAbs (zone.getBottom(), 0.5f));
    REQUIRE_THAT (last.x,  WithinAbs (zone.getRight(), 0.5f));
    REQUIRE_THAT (last.y,  WithinAbs (zone.getY(), 0.5f));
}

TEST_CASE ("buildFadeCurve: falling mirrors rising", "[fade_curve]")
{
    juce::Rectangle<float> zone (0.0f, 0.0f, 80.0f, 40.0f);
    auto p = buildFadeCurve (zone, FadeShape::Linear, /*rising*/ false);

    const auto first = p.getPointAlongPath (0.0f);
    const auto last  = p.getPointAlongPath (p.getLength());
    // Fade-out: starts top-left (gain 1), ends bottom-right (gain 0).
    REQUIRE_THAT (first.y, WithinAbs (zone.getY(), 0.5f));
    REQUIRE_THAT (last.y,  WithinAbs (zone.getBottom(), 0.5f));
}

TEST_CASE ("buildFadeCurve: shape is honoured (equal-power bows above linear)",
           "[fade_curve]")
{
    // A rising equal-power fade reaches a higher gain at the midpoint
    // than linear (sin(pi/4)=0.707 > 0.5), so its curve sits HIGHER =
    // smaller y. Comparing the two curves' bounds is independent of
    // JUCE's arc-length path parameterisation, so it's a robust check
    // that the FadeShape actually drives the geometry.
    juce::Rectangle<float> zone (0.0f, 0.0f, 100.0f, 100.0f);
    auto linear = buildFadeCurve (zone, FadeShape::Linear,     /*rising*/ true);
    auto eqpow  = buildFadeCurve (zone, FadeShape::EqualPower,  /*rising*/ true);

    // Both span the full zone width/height.
    REQUIRE_THAT (linear.getBounds().getWidth(),  WithinAbs (zone.getWidth(),  1.0f));
    REQUIRE_THAT (eqpow .getBounds().getHeight(), WithinAbs (zone.getHeight(), 1.0f));

    // Sample the y at the centre x by intersecting a vertical line.
    auto yAtCentreX = [] (const juce::Path& p, float cx)
    {
        // Walk the path's points; return the y of the vertex nearest cx.
        juce::Path::Iterator it (p);
        float bestY = 0.0f, bestDx = 1.0e9f;
        while (it.next())
        {
            const float dx = std::abs (it.x1 - cx);
            if (dx < bestDx) { bestDx = dx; bestY = it.y1; }
        }
        return bestY;
    };
    const float linMid = yAtCentreX (linear, 50.0f);
    const float eqMid  = yAtCentreX (eqpow,  50.0f);
    REQUIRE_THAT (linMid, WithinAbs (50.0f, 2.0f));   // gain 0.5 → y 50
    REQUIRE (eqMid < linMid);                          // higher gain → smaller y
}
