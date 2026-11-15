#include <catch2/catch_test_macros.hpp>

#include "ui/LevelMeterDraw.h"

#include <cstring>
#include <initializer_list>

using namespace duskstudio;

namespace
{
constexpr float kFloorDb = -100.0f;
constexpr float kTopDb   = 12.0f;

float fraction (float db) { return faderSkewMeterFrac (db, kFloorDb, kTopDb); }

// Plays `holdTicks` ticks at startDb, then silence, through the same ballistics,
// draw and gate the console runs. Returns what the last requested repaint drew
// and what the meter would draw at the end.
struct Rest
{
    LevelBarDraw painted, resting;
};

template <typename Draw>
Rest decayToRest (float startDb, int holdTicks, Draw draw)
{
    LevelBallistics meter;
    LevelBarGate gate;
    Rest rest;
    auto tick = [&] (float incoming)
    {
        meter.step (incoming);
        const auto now = draw (meter);
        if (gate.changed (now)) rest.painted = now;
        return now;
    };
    for (int i = 0; i < holdTicks; ++i) tick (startDb);
    for (int i = 0; i < 900; ++i) rest.resting = tick (kFloorDb);
    return rest;
}
} // namespace

TEST_CASE ("A channel input bar comes to rest with nothing drawn on it", "[ui][meter]")
{
    for (const float span : { 96.0f, 118.0f, 138.0f, 148.0f, 158.0f, 178.0f, 198.0f, 218.0f,
                              228.0f, 238.0f, 248.0f, 298.0f, 396.0f })
        for (const float startDb : { 12.0f, 6.0f, 0.0f, -3.0f, -6.0f, -12.0f, -18.0f, -24.0f,
                                     -40.0f, -55.0f, -70.0f })
            for (const int holdTicks : { 1, 3, 20, 60 })
            {
                const auto rest = decayToRest (startDb, holdTicks, [span] (const LevelBallistics& m)
                {
                    return inputBarDraw (m.displayed, fraction (m.displayed),
                                         m.peak, fraction (m.peak), span);
                });
                CAPTURE (span, startDb, holdTicks);
                REQUIRE_FALSE (rest.resting.fill.shown);
                REQUIRE_FALSE (rest.resting.peak.shown);
                REQUIRE_FALSE (rest.painted.fill.shown);
                REQUIRE_FALSE (rest.painted.peak.shown);
            }
}

TEST_CASE ("A bus or master column comes to rest with nothing drawn on it", "[ui][meter]")
{
    for (const float span : { 96.0f, 148.0f, 198.0f, 248.0f, 396.0f })
        for (const float startDb : { 12.0f, 0.0f, -12.0f, -40.0f, -70.0f })
            for (const int holdTicks : { 1, 20 })
            {
                const auto rest = decayToRest (startDb, holdTicks, [span] (const LevelBallistics& m)
                {
                    return outputColumnDraw (m.displayed, fraction (m.displayed), span);
                });
                CAPTURE (span, startDb, holdTicks);
                REQUIRE_FALSE (rest.resting.fill.shown);
                REQUIRE_FALSE (rest.painted.fill.shown);
            }
}

TEST_CASE ("A level bar repaints when its colour zone or peak tint changes", "[ui][meter]")
{
    LevelBarGate gate;
    REQUIRE (gate.changed (inputBarDraw (-5.01f, 0.6f, -20.0f, 0.4f, 200.0f)));
    REQUIRE (gate.changed (inputBarDraw (-4.99f, 0.6f, -20.0f, 0.4f, 200.0f)));
    REQUIRE_FALSE (gate.changed (inputBarDraw (-4.98f, 0.6f, -20.0f, 0.4f, 200.0f)));
    REQUIRE (gate.changed (inputBarDraw (-4.98f, 0.6f, 5.0f, 0.4f, 200.0f)));
}

TEST_CASE ("A peak readout changes only when its text does", "[ui][meter]")
{
    PeakReadout readout;
    REQUIRE (readout.update (-100.0f));
    REQUIRE (std::strcmp (readout.text, "-inf") == 0);
    REQUIRE_FALSE (readout.update (-70.0f));
    REQUIRE (readout.update (-12.24f));
    REQUIRE (std::strcmp (readout.text, "-12.2") == 0);
    REQUIRE_FALSE (readout.update (-12.2f));
    REQUIRE (readout.update (-12.26f));
    REQUIRE (std::strcmp (readout.text, "-12.3") == 0);
    REQUIRE (readout.update (-60.0f));
    REQUIRE (std::strcmp (readout.text, "-inf") == 0);
}
