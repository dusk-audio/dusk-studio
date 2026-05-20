#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BrickwallLimiter.h"

#include <algorithm>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr        = 48000.0;
constexpr int    kBlock     = 512;
constexpr double kLookahead = 3.0;

float blockPeak (const std::vector<float>& L, const std::vector<float>& R)
{
    float p = 0.0f;
    for (size_t i = 0; i < L.size(); ++i)
        p = std::max (p, std::max (std::abs (L[i]), std::abs (R[i])));
    return p;
}
}

TEST_CASE ("BrickwallLimiter: silence in -> silence out", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);

    std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);

    // Drive several blocks before measuring even for silence: the
    // limiter has a 3 ms lookahead delay line and an internal envelope,
    // so the first block runs with cold state. Measuring on a later
    // block guarantees we're in steady state per the project's test
    // guideline ("Drive DSP through several blocks before measuring
    // when the unit has lookahead, smoothing, or filter state").
    for (int b = 0; b < 4; ++b)
        lim.processInPlace (L.data(), R.data(), kBlock);

    REQUIRE_THAT (blockPeak (L, R), WithinAbs (0.0f, 1.0e-9f));
    REQUIRE_THAT (lim.getCurrentGrDb(), WithinAbs (0.0f, 1.0e-6f));
}

TEST_CASE ("BrickwallLimiter: peaks above ceiling are clamped", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);
    lim.setCeilingDb (-1.0f);
    lim.setEnabled  (true);

    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);

    // Run several blocks of a 1 kHz sine at +6 dB so the lookahead has time
    // to engage and the envelope settles. The first block(s) include the
    // pre-lookahead silence, so peak measurement is taken over the steady
    // state.
    std::vector<float> L (kBlock), R (kBlock);
    const float drive = juce::Decibels::decibelsToGain (6.0f);
    double phase = 0.0;
    const double inc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSr;

    float steadyPeak = 0.0f;
    for (int b = 0; b < 8; ++b)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = drive * (float) std::sin (phase);
            phase += inc;
            L[(size_t) i] = s;
            R[(size_t) i] = s;
        }
        lim.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 2) steadyPeak = std::max (steadyPeak, blockPeak (L, R));
    }

    REQUIRE (steadyPeak <= ceiling + 1.0e-4f);
    REQUIRE (lim.getCurrentGrDb() < 0.0f);
}

TEST_CASE ("BrickwallLimiter: bypass passes signal through (delayed)", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);
    lim.setEnabled (false);

    std::vector<float> L (kBlock), R (kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        L[(size_t) i] = 0.5f;
        R[(size_t) i] = -0.5f;
    }
    const auto inputPeak = blockPeak (L, R);

    lim.processInPlace (L.data(), R.data(), kBlock);

    const int latency = lim.getLatencySamples();
    REQUIRE (latency > 0);
    REQUIRE (latency < kBlock);

    // After the lookahead delay, the constant input reaches the output
    // unchanged because the limiter is bypassed.
    const float postLatencyPeak = [&]
    {
        float p = 0.0f;
        for (int i = latency; i < kBlock; ++i)
            p = std::max (p, std::max (std::abs (L[(size_t) i]),
                                       std::abs (R[(size_t) i])));
        return p;
    }();

    REQUIRE_THAT (postLatencyPeak, WithinAbs (inputPeak, 1.0e-6f));
    REQUIRE_THAT (lim.getCurrentGrDb(), WithinAbs (0.0f, 1.0e-6f));
}
