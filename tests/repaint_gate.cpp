#include <catch2/catch_test_macros.hpp>

#include "ui/RepaintGate.h"

#include <limits>

using namespace duskstudio;

namespace
{
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
} // namespace

TEST_CASE ("A painted value reports the first value and then only moves past eps",
           "[ui][meter]")
{
    PaintedValue gate;
    REQUIRE (gate.changed (-100.0f, 0.1f));
    REQUIRE_FALSE (gate.changed (-100.0f, 0.1f));
    REQUIRE_FALSE (gate.changed (-99.95f, 0.1f));
    REQUIRE (gate.changed (-99.85f, 0.1f));
    REQUIRE_FALSE (gate.changed (-99.85f, 0.1f));

    gate.forget();
    REQUIRE (gate.changed (-99.85f, 0.1f));
}

TEST_CASE ("A painted value catches a decay slower than eps per step",
           "[ui][meter]")
{
    PaintedValue gate;
    float value = 0.0f;
    REQUIRE (gate.changed (value, 0.1f));

    int reports = 0;
    for (int i = 0; i < 1000; ++i)
    {
        value -= 0.01f;
        if (gate.changed (value, 0.1f)) ++reports;
    }

    REQUIRE (reports >= 90);
    REQUIRE (gate.last <= value + 0.1f);
    REQUIRE (gate.last >= value);
}

TEST_CASE ("A painted value treats NaN and infinity as values it can hold",
           "[ui][meter]")
{
    SECTION ("NaN after a number repaints once, then holds")
    {
        PaintedValue gate;
        REQUIRE (gate.changed (-12.0f, 0.1f));
        REQUIRE (gate.changed (kNaN, 0.1f));
        REQUIRE_FALSE (gate.changed (kNaN, 0.1f));
        REQUIRE (gate.changed (-12.0f, 0.1f));
    }

    SECTION ("A first value of NaN still paints")
    {
        PaintedValue gate;
        REQUIRE (gate.changed (kNaN, 0.1f));
        REQUIRE_FALSE (gate.changed (kNaN, 0.1f));
    }

    SECTION ("Infinity holds against itself and moves against anything else")
    {
        PaintedValue gate;
        REQUIRE (gate.changed (-kInf, 0.1f));
        REQUIRE_FALSE (gate.changed (-kInf, 0.1f));
        REQUIRE (gate.changed (kInf, 0.1f));
        REQUIRE_FALSE (gate.changed (kInf, 0.1f));
        REQUIRE (gate.changed (-60.0f, 0.1f));
        REQUIRE (gate.changed (-kInf, 0.1f));
    }

    SECTION ("A NaN or negative eps never holds a different value back")
    {
        PaintedValue gate;
        REQUIRE (gate.changed (1.0f, kNaN));
        REQUIRE (gate.changed (1.0001f, kNaN));
        REQUIRE_FALSE (gate.changed (1.0001f, kNaN));
        REQUIRE (gate.changed (1.0002f, -1.0f));
    }
}

TEST_CASE ("The gated repaint counter only counts what it is told", "[ui][meter]")
{
    const auto before = gatedRepaintRequests.load();
    countGatedRepaint();
    countGatedRepaint();
    REQUIRE (gatedRepaintRequests.load() == before + 2);
}
