#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <FourKEQDSP.hpp>
#include <FourKEQParams.hpp>

#include "session/Session.h"
#include "ui/EqKnobTravel.h"

#include <cmath>
#include <vector>

// The channel EQ's band frequency and Q knobs travel as 4K EQ 2's do, through
// the core's anchors at each twentieth of the travel, so a knob at a position
// reads on the strip what the plug-in's knob reads there.

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;
using EQ = duskaudio::FourKEQDSP;
using duskstudio::ChannelStripParams;
using duskstudio::EqKnob;
using duskstudio::EqKnobTravel;

namespace
{
constexpr int kLast = EQ::kControlAnchorCount - 1;

struct Knob
{
    const char* name;
    EqKnobTravel travel;
    const float* anchors;
    int pluginParam;
};

std::vector<Knob> knobs()
{
    using P = ChannelStripParams;
    return {
        { "LF frequency", { EqKnob::LfFreq, P::kLfFreqMin, P::kLfFreqMax, 100.0 },
          EQ::frequencyControlAnchors (EQ::Band::LF), kLfFreq },
        { "LM frequency", { EqKnob::LmFreq, P::kLmFreqMin, P::kLmFreqMax, 600.0 },
          EQ::frequencyControlAnchors (EQ::Band::LM), kLmFreq },
        { "HM frequency", { EqKnob::HmFreq, P::kHmFreqMin, P::kHmFreqMax, 2000.0 },
          EQ::frequencyControlAnchors (EQ::Band::HM), kHmFreq },
        { "HF frequency", { EqKnob::HfFreq, P::kHfFreqMin, P::kHfFreqMax, 8000.0 },
          EQ::frequencyControlAnchors (EQ::Band::HF), kHfFreq },
        { "Q", { EqKnob::Q, P::kBandQMin, P::kBandQMax, 1.0 }, EQ::qControlAnchors(), kLmQ },
    };
}

// The anchor a knob reads at k/20 of its travel: a Q knob descends through
// the ascending Q anchors.
float anchorAt (const Knob& knob, int k)
{
    return knob.anchors[knob.travel.knob == EqKnob::Q ? kLast - k : k];
}
} // namespace

TEST_CASE ("Channel EQ knobs read the core's anchor at each twentieth of their travel")
{
    for (const auto& knob : knobs())
    {
        INFO (knob.name);
        for (int k = 0; k <= kLast; ++k)
        {
            INFO ("position " << k << "/20");
            const auto value = (float) knob.travel.valueAt (k / 20.0);
            CHECK_THAT (value, WithinULP (anchorAt (knob, k), 0));
            CHECK_THAT (knob.travel.positionOf (anchorAt (knob, k)), WithinAbs (k / 20.0, 1.0e-12));
        }
    }
}

TEST_CASE ("Channel EQ knobs travel linearly between the core's anchors")
{
    for (const auto& knob : knobs())
    {
        INFO (knob.name);
        for (int k = 0; k < kLast; ++k)
            for (const double f : { 0.25, 0.5, 0.75 })
            {
                const double from = anchorAt (knob, k), to = anchorAt (knob, k + 1);
                CHECK_THAT (knob.travel.valueAt ((k + f) / 20.0), WithinRel (from + f * (to - from), 1.0e-12));
            }
    }
}

TEST_CASE ("Channel EQ frequency knobs rise and Q knobs fall over the whole travel")
{
    for (const auto& knob : knobs())
    {
        INFO (knob.name);
        const bool falls = knob.travel.knob == EqKnob::Q;
        double previous = knob.travel.valueAt (0.0);
        for (int step = 1; step <= 2000; ++step)
        {
            const double value = knob.travel.valueAt (step / 2000.0);
            INFO ("position " << step / 2000.0);
            CHECK ((falls ? value < previous : value > previous));
            previous = value;
        }
    }
}

// Both directions are linear interpolation in double over float anchors, so
// a round trip loses only rounding: well inside a millionth of the value or
// of the travel, far below the knobs' 1 Hz and 0.01 steps.
TEST_CASE ("Channel EQ knob value and position round-trip")
{
    for (const auto& knob : knobs())
    {
        INFO (knob.name);
        const double lo = anchorAt (knob, 0), hi = anchorAt (knob, kLast);
        for (int step = 0; step <= 1000; ++step)
        {
            const double value = lo + (hi - lo) * step / 1000.0;
            CHECK_THAT (knob.travel.valueAt (knob.travel.positionOf (value)), WithinRel (value, 1.0e-9));
            const double position = step / 1000.0;
            CHECK_THAT (knob.travel.positionOf (knob.travel.valueAt (position)), WithinAbs (position, 1.0e-9));
        }
    }
}

TEST_CASE ("Channel EQ knob ends: a frequency knob spans its stored range, a Q value past the anchors rests on an end")
{
    for (const auto& knob : knobs())
    {
        INFO (knob.name);
        const auto& travel = knob.travel;
        if (travel.knob == EqKnob::Q)
        {
            CHECK (travel.valueAt (0.0) > travel.valueAt (1.0));
            CHECK_THAT (travel.valueAt (0.0), WithinULP (3.0, 0));
            CHECK_THAT (travel.valueAt (1.0), WithinULP (0.5, 0));
            CHECK_THAT (travel.positionOf (ChannelStripParams::kBandQMax), WithinAbs (0.0, 0.0));
            CHECK_THAT (travel.positionOf (ChannelStripParams::kBandQMin), WithinAbs (1.0, 0.0));
        }
        else
        {
            CHECK_THAT (travel.valueAt (0.0), WithinULP (travel.lo, 0));
            CHECK_THAT (travel.valueAt (1.0), WithinULP (travel.hi, 0));
            CHECK_THAT (travel.positionOf (travel.lo - 100.0), WithinAbs (0.0, 0.0));
            CHECK_THAT (travel.positionOf (travel.hi + 100.0), WithinAbs (1.0, 0.0));
        }
        CHECK_THAT (travel.positionOf (-1.0), WithinAbs (travel.knob == EqKnob::Q ? 1.0 : 0.0, 0.0));
        CHECK_THAT (travel.valueAt (-1.0), WithinULP (travel.valueAt (0.0), 0));
        CHECK_THAT (travel.valueAt (2.0), WithinULP (travel.valueAt (1.0), 0));
    }
    const auto lm = knobs()[1].travel;
    const auto hm = knobs()[2].travel;
    CHECK_THAT (lm.positionOf (ChannelStripParams::kLmFreqHeldMin), WithinAbs (0.0, 0.0));
    CHECK_THAT (hm.positionOf (ChannelStripParams::kHmFreqHeldMax), WithinAbs (1.0, 0.0));
}

TEST_CASE ("Channel EQ knob defaults land where 4K EQ 2 puts them")
{
    for (const auto& knob : knobs())
    {
        INFO (knob.name);
        const auto& param = kFourKParams[knob.pluginParam];
        if (knob.travel.knob != EqKnob::Q)
        {
            CHECK_THAT (param.min, WithinULP ((float) knob.travel.lo, 0));
            CHECK_THAT (param.max, WithinULP ((float) knob.travel.hi, 0));
        }

        int pluginStep = -1;
        for (int k = 0; k <= kLast; ++k)
            if (std::abs (anchorAt (knob, k) - param.def) < 1.0e-4f) pluginStep = k;
        REQUIRE (pluginStep >= 0);
        CHECK_THAT (knob.travel.positionOf (param.def), WithinAbs (pluginStep / 20.0, 1.0e-12));

        const double stripDefault = knob.travel.knob == EqKnob::Q ? 0.7 : knob.travel.mid;
        const double position = knob.travel.positionOf (stripDefault);
        CHECK (position > 0.0);
        CHECK (position < 1.0);
        CHECK_THAT (knob.travel.valueAt (position), WithinRel (stripDefault, 1.0e-9));
    }
    CHECK_THAT (kFourKParams[kHmQ].def, WithinULP (kFourKParams[kLmQ].def, 0));
}
