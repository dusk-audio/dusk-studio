#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BusStrip.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 512;

// Run a steady sine through the strip for `blocks` blocks, returning the
// running phase. The fader/pan smoothers ramp over 20 ms, so several blocks
// are needed before the output reaches the commanded gain.
double driveSine (duskstudio::BusStrip& strip, double phase, double freqHz,
                  float amp, int blocks,
                  std::vector<float>* captureL = nullptr,
                  std::vector<float>* captureR = nullptr,
                  int captureFromBlock = 0)
{
    const double inc = 2.0 * juce::MathConstants<double>::pi * freqHz / kSr;
    std::vector<float> L (kBlock), R (kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = amp * (float) std::sin (phase);
            phase += inc;
            L[(size_t) i] = s;
            R[(size_t) i] = s;
        }
        strip.processInPlace (L.data(), R.data(), kBlock);
        if (b >= captureFromBlock)
        {
            if (captureL != nullptr) captureL->insert (captureL->end(), L.begin(), L.end());
            if (captureR != nullptr) captureR->insert (captureR->end(), R.begin(), R.end());
        }
    }
    return phase;
}

float rms (const std::vector<float>& v)
{
    if (v.empty()) return 0.0f;
    double s = 0.0;
    for (float x : v) s += (double) x * (double) x;
    return (float) std::sqrt (s / (double) v.size());
}

float peak (const std::vector<float>& v)
{
    float p = 0.0f;
    for (float x : v) p = std::max (p, std::abs (x));
    return p;
}
} // namespace

TEST_CASE ("BusStrip: unity fader + center pan + flat EQ + comp off ~= passthrough",
           "[BusStrip]")
{
    duskstudio::BusParams params;   // all defaults: fader 0 dB, pan 0, EQ/comp off
    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock, 1);
    strip.bind (params);

    const float amp = juce::Decibels::decibelsToGain (-12.0f);
    std::vector<float> outL, outR;
    // Settle past the smoother ramp, then capture.
    driveSine (strip, 0.0, 1000.0, amp, 16, &outL, &outR, 8);

    // EQ and comp are disabled, but the donor EQ instance still processes
    // (flat) — allow a small tolerance for its near-DC settling, and for the
    // equal-power center-pan law (cos/sin(pi/4) * sqrt2 = 1.0 exactly, so the
    // pan contributes no gain change at center).
    REQUIRE_THAT (rms (outL), WithinRel (rms (outR), 1.0e-4f));
    REQUIRE_THAT (peak (outL), WithinRel (amp, 0.02f));
    REQUIRE_THAT (peak (outR), WithinRel (amp, 0.02f));
}

TEST_CASE ("BusStrip: -6 dB fader halves the signal", "[BusStrip]")
{
    duskstudio::BusParams params;
    params.faderDb.store (-6.0206f, std::memory_order_relaxed);    // exactly 0.5x
    params.liveFaderDb.store (-6.0206f, std::memory_order_relaxed);

    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock, 1);
    strip.bind (params);

    const float amp = juce::Decibels::decibelsToGain (-12.0f);
    std::vector<float> outL, outR;
    driveSine (strip, 0.0, 1000.0, amp, 16, &outL, &outR, 8);

    REQUIRE_THAT (peak (outL), WithinRel (amp * 0.5f, 0.02f));
    REQUIRE_THAT (peak (outR), WithinRel (amp * 0.5f, 0.02f));
}

TEST_CASE ("BusStrip: center pan is unity on both channels (equal-power law)",
           "[BusStrip]")
{
    duskstudio::BusParams params;   // pan 0 = center
    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock, 1);
    strip.bind (params);

    const float amp = juce::Decibels::decibelsToGain (-12.0f);
    std::vector<float> outL, outR;
    driveSine (strip, 0.0, 1000.0, amp, 16, &outL, &outR, 8);

    // Equal-power: cos(pi/4)*sqrt2 == sin(pi/4)*sqrt2 == 1. Center pan leaves
    // both channels at unity and equal.
    REQUIRE_THAT (rms (outL), WithinRel (rms (outR), 1.0e-4f));
    REQUIRE_THAT (peak (outL), WithinRel (amp, 0.02f));
}

TEST_CASE ("BusStrip: silent input -> silent output", "[BusStrip]")
{
    duskstudio::BusParams params;
    params.eqEnabled.store (true, std::memory_order_relaxed);
    params.compEnabled.store (true, std::memory_order_relaxed);

    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock, 1);
    strip.bind (params);

    std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
    float outPeak = 0.0f;
    for (int b = 0; b < 12; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        strip.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 4)
            outPeak = std::max (outPeak, std::max (peak (L), peak (R)));
    }

    // No analog noise floor on a silent bus even with EQ + comp engaged
    // (BusStrip::bindCompParams forces the donor's noise_enable off).
    REQUIRE (outPeak < 1.0e-4f);
}

// --- OS-skip optimisation guards -------------------------------------------

namespace
{
// Best integer lag in [0, maxLag] that aligns `delayed` to `ref` (minimising
// summed-squared error of ref[n] vs delayed[n + lag]). Returns {lag, error}.
std::pair<int, double> bestLag (const std::vector<float>& ref,
                                const std::vector<float>& delayed, int maxLag)
{
    int bestL = 0; double bestErr = 1e30;
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double err = 0.0; int count = 0;
        for (size_t n = 0; n + (size_t) lag < ref.size(); ++n)
        {
            const double d = (double) ref[n] - (double) delayed[n + (size_t) lag];
            err += d * d; ++count;
        }
        err /= std::max (1, count);
        if (err < bestErr) { bestErr = err; bestL = lag; }
    }
    return { bestL, bestErr };
}
} // namespace

TEST_CASE ("BusStrip: comp-off EQ response is independent of OS factor", "[BusStrip]")
{
    // Guards the rate-mismatch trap: the bus EQ is prepared at native rate and
    // must run identically whether or not an OS factor is selected (the OS now
    // wraps only the comp). A +9 dB LF shelf at 300 Hz would boost wrongly if
    // the EQ saw oversampled-rate coefficients.
    const float amp = juce::Decibels::decibelsToGain (-12.0f);

    auto runAt = [&] (int factor)
    {
        duskstudio::BusParams params;
        params.eqEnabled.store (true, std::memory_order_relaxed);
        params.eqLfGainDb.store (9.0f, std::memory_order_relaxed);  // strong LF boost
        // comp left OFF by default → oversampler is skipped at factor > 1.
        duskstudio::BusStrip strip;
        strip.prepare (kSr, kBlock, factor);
        strip.bind (params);
        std::vector<float> outL, outR;
        driveSine (strip, 0.0, 300.0, amp, 24, &outL, &outR, 14);
        return peak (outL);
    };

    const float p1 = runAt (1);
    const float p4 = runAt (4);
    REQUIRE (p1 > amp);                         // LF shelf actually boosted
    REQUIRE_THAT (p4, WithinRel (p1, 0.01f));   // same boost at 4× (native EQ)
}

TEST_CASE ("BusStrip: comp-off bus is OS-latency compensated at 4x", "[BusStrip]")
{
    // With comp off, factor 4 skips the comp oversampler but delays the signal
    // by the oversampler's latency so the bus stays aligned with comp-on buses.
    // The EQ is identical (native) in both runs, so the factor-4 stream should
    // equal the factor-1 stream shifted by a small positive lag.
    const float amp = juce::Decibels::decibelsToGain (-12.0f);

    auto capture = [&] (int factor)
    {
        duskstudio::BusParams params;   // flat EQ, comp off
        duskstudio::BusStrip strip;
        strip.prepare (kSr, kBlock, factor);
        strip.bind (params);
        std::vector<float> outL, outR;
        driveSine (strip, 0.0, 1000.0, amp, 16, &outL, &outR, 8);
        return outL;
    };

    const auto f1 = capture (1);
    const auto f4 = capture (4);

    const auto [lag, err] = bestLag (f1, f4, 16);   // does f4 == f1 delayed?
    REQUIRE (lag >= 1);                  // a real compensation delay is applied
    REQUIRE (lag <= 8);                  // ~4.4 samples at 4× → rounds to 4
    REQUIRE (err < 1.0e-7);              // and at that lag the streams match
}

TEST_CASE ("BusStrip: hot input stays finite", "[BusStrip]")
{
    duskstudio::BusParams params;
    params.eqEnabled.store (true, std::memory_order_relaxed);
    params.eqLfGainDb.store (9.0f, std::memory_order_relaxed);
    params.eqHfGainDb.store (9.0f, std::memory_order_relaxed);
    params.compEnabled.store (true, std::memory_order_relaxed);
    params.compThreshDb.store (-30.0f, std::memory_order_relaxed);
    params.compMakeupDb.store (12.0f, std::memory_order_relaxed);

    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock, 1);
    strip.bind (params);

    const float amp = juce::Decibels::decibelsToGain (6.0f);   // +6 dBFS, hot
    std::vector<float> outL, outR;
    driveSine (strip, 0.0, 220.0, amp, 24, &outL, &outR, 12);

    for (size_t i = 0; i < outL.size(); ++i)
    {
        REQUIRE (std::isfinite (outL[i]));
        REQUIRE (std::isfinite (outR[i]));
    }
}
