#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MasteringChain.h"
#include "dsp/BrickwallLimiter.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr    = 48000.0;
constexpr int    kBlock = 512;

float peak (const float* v, int n)
{
    float p = 0.0f;
    for (int i = 0; i < n; ++i) p = std::max (p, std::abs (v[i]));
    return p;
}

double driveSine (duskstudio::MasteringChain& chain, double phase, double freqHz,
                  float amp, int blocks,
                  std::vector<float>* captureL = nullptr,
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
        chain.processInPlace (L.data(), R.data(), kBlock);
        if (captureL != nullptr && b >= captureFromBlock)
            captureL->insert (captureL->end(), L.begin(), L.end());
    }
    return phase;
}
} // namespace

TEST_CASE ("MasteringChain: silence in -> silence out", "[MasteringChain]")
{
    duskstudio::MasteringParams params;   // limiter ON by default; EQ/comp off
    duskstudio::MasteringChain chain;
    chain.prepare (kSr, kBlock, 1);
    chain.bind (params);

    std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
    float outPeak = 0.0f;
    // The limiter's lookahead delay line starts cold; flush it before
    // measuring so the residual zeros from cold state don't mask a noise floor.
    for (int b = 0; b < 12; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        chain.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 4)
            outPeak = std::max (outPeak, std::max (peak (L.data(), kBlock),
                                                   peak (R.data(), kBlock)));
    }

    REQUIRE (outPeak < 1.0e-4f);
}

TEST_CASE ("MasteringChain: latency matches the brickwall limiter", "[MasteringChain]")
{
    duskstudio::MasteringChain chain;
    chain.prepare (kSr, kBlock, 1);

    duskstudio::BrickwallLimiter ref;
    ref.prepare (kSr, kBlock);

    REQUIRE (chain.getLatencySamples() > 0);
    REQUIRE (chain.getLatencySamples() == ref.getLatencySamples());
}

TEST_CASE ("MasteringChain: comp over silent input adds no noise floor",
           "[MasteringChain]")
{
    // Mirror of AudioPipelineSelfTest::testMasterCompNoNoiseFloor — the donor
    // comp's analog-noise generator must stay disabled so an engaged comp over
    // silence stays silent (no continuous -67 dB floor into every bounce).
    duskstudio::MasteringParams params;
    params.compEnabled.store (true, std::memory_order_relaxed);
    params.limiterEnabled.store (false, std::memory_order_relaxed);   // isolate the comp

    duskstudio::MasteringChain chain;
    chain.prepare (kSr, kBlock, 1);
    chain.bind (params);

    std::vector<float> L (kBlock, 0.0f), R (kBlock, 0.0f);
    float outPeak = 0.0f;
    for (int b = 0; b < 16; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        chain.processInPlace (L.data(), R.data(), kBlock);
        if (b >= 4)
            outPeak = std::max (outPeak, std::max (peak (L.data(), kBlock),
                                                   peak (R.data(), kBlock)));
    }

    REQUIRE (outPeak < 1.0e-4f);   // -80 dBFS
}

TEST_CASE ("MasteringChain: hot input stays finite and at/below the ceiling",
           "[MasteringChain]")
{
    duskstudio::MasteringParams params;
    params.limiterEnabled.store (true, std::memory_order_relaxed);
    params.limiterCeilingDb.store (-1.0f, std::memory_order_relaxed);
    params.limiterDriveDb.store (6.0f, std::memory_order_relaxed);

    duskstudio::MasteringChain chain;
    chain.prepare (kSr, kBlock, 1);
    chain.bind (params);

    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);
    const float amp = juce::Decibels::decibelsToGain (6.0f);   // hot, +6 dBFS

    std::vector<float> captured;
    driveSine (chain, 0.0, 1000.0, amp, 24, &captured, 12);   // capture steady state

    for (float x : captured)
        REQUIRE (std::isfinite (x));

    float steadyPeak = 0.0f;
    for (float x : captured) steadyPeak = std::max (steadyPeak, std::abs (x));

    // 4x oversampled FIR ripple ⇒ ~0.3 dB tolerance above the linear ceiling
    // (same allowance the BrickwallLimiter true-peak test uses).
    REQUIRE (steadyPeak <= ceiling * 1.04f);
}
