#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MasterBus.h"
#include "session/Session.h"

#include <algorithm>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using duskstudio::MasterBus;
using duskstudio::MasterBusParams;

namespace
{
constexpr double kSr    = 48000.0;
// 480 samples is 10 ms, so a whole number of blocks lands on the 300 ms VU
// time constant.
constexpr int    kBlock = 480;
constexpr double kTwoPi = 6.283185307179586;

struct Capture
{
    std::vector<float> l, r;
};

// Runs `blocks` blocks of a sine on each side (amplitude 0 = silent side) and
// keeps the output of the blocks from `keepFrom` on.
Capture drive (MasterBus& master, double hz, float ampL, float ampR, int blocks, int keepFrom)
{
    Capture out;
    std::vector<float> l ((std::size_t) kBlock), r ((std::size_t) kBlock);
    double phase = 0.0;
    const double inc = kTwoPi * hz / kSr;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = (float) std::sin (phase);
            phase += inc;
            l[(std::size_t) i] = ampL * s;
            r[(std::size_t) i] = ampR * s;
        }
        master.processInPlace (l.data(), r.data(), kBlock);
        if (b >= keepFrom)
        {
            out.l.insert (out.l.end(), l.begin(), l.end());
            out.r.insert (out.r.end(), r.begin(), r.end());
        }
    }
    return out;
}

double rmsDb (const std::vector<float>& v)
{
    double sum = 0.0;
    for (const float x : v) sum += (double) x * (double) x;
    return 10.0 * std::log10 (std::max (sum / (double) std::max<std::size_t> (1, v.size()), 1.0e-24));
}

// Amplitude of the `hz` component, by a single-bin DFT over a Hann window.
double toneDb (const std::vector<float>& v, double hz)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const auto n = v.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * (double) i / (double) (n - 1));
        const double a = kTwoPi * hz * (double) i / kSr;
        re += w * v[i] * std::cos (a);
        im -= w * v[i] * std::sin (a);
        wsum += w;
    }
    return 20.0 * std::log10 (std::max (2.0 * std::hypot (re, im) / wsum, 1.0e-12));
}

void quietTape (MasterBusParams& params)
{
    params.tape.autoCal.store (false);
    params.tape.autoComp.store (false);
    params.tape.noiseAmount.store (0.0f);
    params.tape.wow.store (0.0f);
    params.tape.flutter.store (0.0f);
}

void boostLows (MasterBusParams& params)
{
    params.eqEnabled.store (true);
    params.eqLfFreq.store (100.0f);
    params.eqLfBoost.store (10.0f);
}
} // namespace

TEST_CASE ("master program EQ plays in the master chain", "[master][eq][order]")
{
    // Full LF boost at 100 Hz lifts a 100 Hz tone on the master's output.
    MasterBusParams flatParams, boostedParams;
    boostLows (boostedParams);

    MasterBus flat, boosted;
    flat.bind (flatParams);
    boosted.bind (boostedParams);
    flat.prepare (kSr, kBlock, 1);
    boosted.prepare (kSr, kBlock, 1);

    const auto a = drive (flat,    100.0, 0.1f, 0.1f, 40, 20);
    const auto b = drive (boosted, 100.0, 0.1f, 0.1f, 40, 20);
    CAPTURE (rmsDb (a.l), rmsDb (b.l));
    REQUIRE (rmsDb (b.l) - rmsDb (a.l) > 6.0);
    REQUIRE_THAT (rmsDb (b.r), WithinAbs (rmsDb (b.l), 0.01));
}

TEST_CASE ("master program EQ comes before the master compressor", "[master][eq][order]")
{
    // With the compressor in gain reduction, boosting the tone at the EQ
    // deepens the reduction. An EQ after the compressor could not change what
    // its detector heard.
    auto grDb = [] (bool boost)
    {
        MasterBusParams params;
        params.compEnabled.store (true);
        params.compThreshDb.store (-30.0f);
        if (boost) boostLows (params);
        MasterBus master;
        master.bind (params);
        master.prepare (kSr, kBlock, 1);
        drive (master, 100.0, 0.1f, 0.1f, 60, 60);
        return (double) params.meterGrDb.load();
    };

    const double flat    = grDb (false);
    const double boosted = grDb (true);
    CAPTURE (flat, boosted);
    REQUIRE (flat < -1.0);
    REQUIRE (boosted - flat < -3.0);
}

TEST_CASE ("master program EQ comes before the tape", "[master][eq][tape][order]")
{
    // The tape saturates, so driving it harder raises its third harmonic
    // against the fundamental. A 100 Hz LF boost ahead of the tape drives it
    // harder at 100 Hz, and the 300 Hz harmonic rises relative to 100 Hz. The
    // same boost after the tape would lift 100 Hz more than 300 Hz, so the
    // ratio would fall instead.
    auto h3RelDb = [] (bool boost)
    {
        MasterBusParams params;
        quietTape (params);
        params.tapeEnabled.store (true);
        params.tape.inputGainDb.store (12.0f);
        if (boost) boostLows (params);
        MasterBus master;
        master.bind (params);
        master.prepare (kSr, kBlock, 1);
        const auto out = drive (master, 100.0, 0.1f, 0.1f, 80, 40);
        return toneDb (out.l, 300.0) - toneDb (out.l, 100.0);
    };

    const double flat    = h3RelDb (false);
    const double boosted = h3RelDb (true);
    CAPTURE (flat, boosted);
    REQUIRE (boosted - flat > 3.0);
}

TEST_CASE ("master output meters read post-fader peak and 300 ms VU per side", "[master][meters]")
{
    MasterBusParams params;
    MasterBus master;
    master.bind (params);
    master.prepare (kSr, kBlock, 1);

    const float ampL = 0.5f, ampR = 0.25f;
    // A 1 kHz sine has a sample on its crest every period at 48 kHz, so the
    // block peak is the amplitude exactly.
    const double steadyL = ampL / std::sqrt (2.0);
    const double steadyR = ampR / std::sqrt (2.0);

    SECTION ("the VU integrates over 300 ms")
    {
        // From silence, a first-order 300 ms integrator reaches 1 - 1/e of the
        // tone's RMS after 300 ms and 1 - 1/e^3 after 900 ms.
        drive (master, 1000.0, ampL, ampR, 30, 30);
        REQUIRE_THAT (params.meterPostMasterRmsL.load(), WithinRel (steadyL * (1.0 - std::exp (-1.0)), 0.02));
        REQUIRE_THAT (params.meterPostMasterRmsR.load(), WithinRel (steadyR * (1.0 - std::exp (-1.0)), 0.02));
        drive (master, 1000.0, ampL, ampR, 60, 60);
        REQUIRE_THAT (params.meterPostMasterRmsL.load(), WithinRel (steadyL * (1.0 - std::exp (-3.0)), 0.02));
        REQUIRE_THAT (params.meterPostMasterRmsR.load(), WithinRel (steadyR * (1.0 - std::exp (-3.0)), 0.02));
    }

    SECTION ("peak and VU follow the master fader")
    {
        params.liveFaderDb.store (-6.0f);
        drive (master, 1000.0, ampL, ampR, 400, 400);
        const double g = std::pow (10.0, -6.0 / 20.0);
        REQUIRE_THAT (params.meterPostMasterLDb.load(), WithinAbs (20.0 * std::log10 (ampL * g), 0.05));
        REQUIRE_THAT (params.meterPostMasterRDb.load(), WithinAbs (20.0 * std::log10 (ampR * g), 0.05));
        REQUIRE_THAT (params.meterPostMasterRmsL.load(), WithinRel (steadyL * g, 0.01));
        REQUIRE_THAT (params.meterPostMasterRmsR.load(), WithinRel (steadyR * g, 0.01));
    }
}

TEST_CASE ("master GR meter reads the master compressor's reduction", "[master][meters]")
{
    MasterBusParams params;
    params.compThreshDb.store (-20.0f);
    MasterBus master;
    master.bind (params);
    master.prepare (kSr, kBlock, 1);

    params.compEnabled.store (true);
    drive (master, 1000.0, 0.5f, 0.5f, 60, 60);
    const float engaged = params.meterGrDb.load();

    params.compEnabled.store (false);
    drive (master, 1000.0, 0.5f, 0.5f, 5, 5);
    const float bypassed = params.meterGrDb.load();

    CAPTURE (engaged, bypassed);
    REQUIRE (engaged < -3.0f);
    REQUIRE (bypassed == 0.0f);
}
