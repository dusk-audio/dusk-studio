#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BrickwallLimiter.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kSr        = 48000.0;
constexpr int    kBlock     = 512;
constexpr double kLookahead = 2.0;

float blockPeak (const std::vector<float>& L, const std::vector<float>& R)
{
    float p = 0.0f;
    for (size_t i = 0; i < L.size(); ++i)
        p = std::max (p, std::max (std::abs (L[i]), std::abs (R[i])));
    return p;
}

// Inter-sample (true) peak of a mono buffer, measured by 4x oversampling -
// the same resolution the limiter controls internally.
float truePeak4x (std::vector<float> mono)
{
    juce::dsp::Oversampling<float> os (
        1, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    os.initProcessing ((size_t) mono.size());
    os.reset();

    float* p[1] = { mono.data() };
    juce::dsp::AudioBlock<float> blk (p, 1, mono.size());
    auto up = os.processSamplesUp (blk);

    float peak = 0.0f;
    const float* u = up.getChannelPointer (0);
    for (size_t i = 0; i < up.getNumSamples(); ++i)
        peak = std::max (peak, std::abs (u[i]));
    return peak;
}

// Fill a block with a sine at `freqHz`, advancing a running phase.
void fillSine (std::vector<float>& L, std::vector<float>& R,
               double& phase, double freqHz, float amp)
{
    const double inc = 2.0 * juce::MathConstants<double>::pi * freqHz / kSr;
    for (size_t i = 0; i < L.size(); ++i)
    {
        const float s = amp * (float) std::sin (phase);
        phase += inc;
        L[i] = s;
        R[i] = s;
    }
}
}

TEST_CASE ("BrickwallLimiter: silence in -> silence out", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);

    std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);

    // Drive several blocks before measuring: the lookahead delay + oversampler
    // start cold, so steady state is reached only after a few blocks.
    for (int b = 0; b < 6; ++b)
        lim.processInPlace (L.data(), R.data(), kBlock);

    REQUIRE_THAT (blockPeak (L, R), WithinAbs (0.0f, 1.0e-9f));
    REQUIRE_THAT (lim.getCurrentGrDb(), WithinAbs (0.0f, 1.0e-6f));
}

TEST_CASE ("BrickwallLimiter: sample peaks above ceiling are clamped", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);
    lim.setCeilingDb (-1.0f);
    lim.setEnabled  (true);

    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);

    std::vector<float> L (kBlock), R (kBlock);
    double phase = 0.0;

    float steadyPeak = 0.0f;
    for (int b = 0; b < 12; ++b)
    {
        fillSine (L, R, phase, 1000.0, juce::Decibels::decibelsToGain (6.0f));
        lim.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 4) steadyPeak = std::max (steadyPeak, blockPeak (L, R));
    }

    REQUIRE (steadyPeak <= ceiling + 1.0e-4f);
    REQUIRE (lim.getCurrentGrDb() < 0.0f);
}

TEST_CASE ("BrickwallLimiter: controls inter-sample (true) peaks", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);
    lim.setCeilingDb (-1.0f);
    lim.setEnabled  (true);

    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);

    // A hot 7 kHz sine has inter-sample peaks ~0.9 dB above its sample peaks at
    // the base rate - the classic case a sample-peak limiter misses entirely.
    // The 4x oversampled limiter controls them; collect the steady-state output
    // and measure its true peak at 4x.
    std::vector<float> L (kBlock), R (kBlock);
    std::vector<float> captured;
    double phase = 0.0;

    for (int b = 0; b < 16; ++b)
    {
        fillSine (L, R, phase, 7000.0, juce::Decibels::decibelsToGain (12.0f));
        lim.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 8)
            captured.insert (captured.end(), L.begin(), L.end());
    }

    // Limiter must have engaged...
    REQUIRE (lim.getCurrentGrDb() < 0.0f);
    // ...and the true peak stays at the ceiling within filter tolerance
    // (~0.3 dB). A base-rate sample-peak limiter would overshoot by ~0.9 dB.
    REQUIRE (truePeak4x (captured) <= ceiling * 1.04f);
}

TEST_CASE ("BrickwallLimiter: transparent below the ceiling (limiter, not compressor)",
           "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);
    lim.setCeilingDb (0.0f);
    lim.setEnabled  (true);

    // A signal entirely below the ceiling must pass at unity gain. A
    // compressor would still pull it down; a limiter leaves it alone.
    const float amp = juce::Decibels::decibelsToGain (-12.0f);
    std::vector<float> L (kBlock), R (kBlock);
    double phase = 0.0;

    float steadyPeak = 0.0f;
    for (int b = 0; b < 10; ++b)
    {
        fillSine (L, R, phase, 1000.0, amp);
        lim.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 4) steadyPeak = std::max (steadyPeak, blockPeak (L, R));
    }

    REQUIRE_THAT (steadyPeak, WithinRel (amp, 0.02f));
    REQUIRE_THAT (lim.getCurrentGrDb(), WithinAbs (0.0f, 1.0e-6f));
}

TEST_CASE ("BrickwallLimiter: reports a positive, bounded latency", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);

    const int latency = lim.getLatencySamples();
    REQUIRE (latency > 0);
    REQUIRE (latency < kBlock);
}

TEST_CASE ("BrickwallLimiter: bypass preserves level", "[BrickwallLimiter]")
{
    duskstudio::BrickwallLimiter lim;
    lim.prepare (kSr, kBlock, kLookahead);
    lim.setEnabled (false);

    const float amp = 0.5f;
    std::vector<float> L (kBlock), R (kBlock);
    double phase = 0.0;

    float steadyPeak = 0.0f;
    for (int b = 0; b < 8; ++b)
    {
        fillSine (L, R, phase, 1000.0, amp);
        lim.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 4) steadyPeak = std::max (steadyPeak, blockPeak (L, R));
    }

    // Bypassed: no gain reduction, level preserved through the OS round-trip.
    REQUIRE_THAT (steadyPeak, WithinRel (amp, 0.01f));
    REQUIRE_THAT (lim.getCurrentGrDb(), WithinAbs (0.0f, 1.0e-6f));
}
