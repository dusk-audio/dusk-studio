#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MasteringChain.h"
#include "dsp/BrickwallLimiter.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <iterator>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE ("Appendix A: active multiband compressor defaults and control ranges",
           "[MasteringChain][appendix]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    duskstudio::MasteringChain chain;
    chain.prepare (48000.0, 512, 1);
    auto* processor = chain.getCompProcessor();
    REQUIRE (processor != nullptr);
    auto& parameters = processor->getParameters();
    const auto* mode = parameters.getRawParameterValue ("mode");
    const auto* autoMakeup = parameters.getRawParameterValue ("auto_makeup");
    REQUIRE (mode != nullptr);
    REQUIRE (autoMakeup != nullptr);
    REQUIRE_THAT (mode->load(), WithinAbs (7, 1e-6));
    REQUIRE_THAT (autoMakeup->load(), WithinAbs (0, 1e-6));

    auto check = [&] (const juce::String& id, float low, float high, float initial)
    {
        CAPTURE (id.toStdString());
        auto* parameter = parameters.getParameter (id);
        auto* value = parameters.getRawParameterValue (id);
        REQUIRE (parameter != nullptr);
        REQUIRE (value != nullptr);
        // Snapping a 0.1 dB interval can leave a sub-micro dB residue with fused arithmetic.
        CHECK_THAT (value->load(), WithinAbs (initial, 1e-6)
                                  || Catch::Matchers::WithinRel (initial, 1e-6f));
        const auto& range = parameter->getNormalisableRange();
        CHECK_THAT (range.start, WithinAbs (low, 1e-5));
        CHECK_THAT (range.end, WithinAbs (high, 1e-5));
        parameter->setValueNotifyingHost (0.0f);
        CHECK_THAT (value->load(), WithinAbs (low, 1e-5));
        parameter->setValueNotifyingHost (1.0f);
        CHECK_THAT (value->load(), WithinAbs (high, 1e-5));
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (initial));
    };

    check ("mb_crossover_1", 20, 500, 200);
    check ("mb_crossover_2", 200, 5000, 2000);
    check ("mb_crossover_3", 2000, 16000, 8000);
    check ("mb_output", -24, 24, 0);
    check ("mix", 0, 100, 100);
    for (const auto* band : { "low", "lowmid", "highmid", "high" })
    {
        const auto prefix = juce::String ("mb_") + band;
        check (prefix + "_threshold", -60, 0, -20);
        check (prefix + "_ratio", 1, 20, 4);
        check (prefix + "_attack", 0.1f, 100, 10);
        check (prefix + "_release", 10, 1000, 100);
        check (prefix + "_makeup", -12, 12, 0);
        check (prefix + "_enabled", 0, 1, 1);
        check (prefix + "_solo", 0, 1, 0);
    }
#else
    SKIP ("requires the mastering compressor donor");
#endif
}

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
                  int captureFromBlock = 0,
                  std::vector<float>* captureR = nullptr)
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
        if (b >= captureFromBlock)
        {
            if (captureL != nullptr) captureL->insert (captureL->end(), L.begin(), L.end());
            if (captureR != nullptr) captureR->insert (captureR->end(), R.begin(), R.end());
        }
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

TEST_CASE ("MasteringChain: latency is the EQ oversampler, the comp and the limiter", "[MasteringChain]")
{
    duskstudio::MasteringChain chain;
    chain.prepare (kSr, kBlock, 1);

    duskstudio::BrickwallLimiter limRef;
    limRef.prepare (kSr, kBlock);
    duskstudio::MasteringDigitalEq eqRef;
    eqRef.prepare (kSr, kBlock);

    int compLatency = 0;
#if DUSKSTUDIO_HAS_DUSK_DSP
    REQUIRE (chain.getCompProcessor() != nullptr);
    compLatency = chain.getCompProcessor()->getLatencySamples();
#endif

    // EQ, comp and limiter are in series; the chain reports the sum.
    REQUIRE (chain.getLatencySamples() > 0);
    REQUIRE (chain.getLatencySamples()
             == eqRef.getLatencySamples() + compLatency + limRef.getLatencySamples());
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

    std::vector<float> capturedL, capturedR;
    driveSine (chain, 0.0, 1000.0, amp, 24, &capturedL, 12, &capturedR);   // capture steady state

    REQUIRE (capturedL.size() == capturedR.size());
    REQUIRE_FALSE (capturedL.empty());

    float steadyPeakL = 0.0f, steadyPeakR = 0.0f;
    for (float x : capturedL) { REQUIRE (std::isfinite (x)); steadyPeakL = std::max (steadyPeakL, std::abs (x)); }
    for (float x : capturedR) { REQUIRE (std::isfinite (x)); steadyPeakR = std::max (steadyPeakR, std::abs (x)); }

    // 4x oversampled FIR ripple ⇒ ~0.3 dB tolerance above the linear ceiling
    // (same allowance the BrickwallLimiter true-peak test uses). Both channels
    // must stay finite + at/below the ceiling — a right-only regression
    // wouldn't show if we only checked L.
    REQUIRE (steadyPeakL <= ceiling * 1.04f);
    REQUIRE (steadyPeakR <= ceiling * 1.04f);
}

#if DUSKSTUDIO_HAS_DUSK_DSP
namespace
{
constexpr int kImpulseAt = 100;
constexpr bool kCompStates[] = { false, true, false };

// A fresh chain on its first prepare: the case where the donor still held its
// default mode when it reported its latency.
void prepareTransparent (duskstudio::MasteringChain& chain, duskstudio::MasteringParams& params,
                         int factor)
{
    chain.bind (params);
    chain.prepare (kSr, kBlock, factor);
    auto* comp = chain.getCompProcessor();
    REQUIRE (comp != nullptr);
    // Thresholds at the top of their range, so the comp passes the impulse unreduced.
    for (const auto* band : { "low", "lowmid", "highmid", "high" })
        if (auto* threshold = comp->getParameters().getParameter (juce::String ("mb_") + band + "_threshold"))
            threshold->setValueNotifyingHost (1.0f);
}

std::vector<float> impulseResponse (duskstudio::MasteringChain& chain,
                                    duskstudio::MasteringParams& params, bool compOn)
{
    params.compEnabled.store (compOn, std::memory_order_relaxed);
    std::vector<float> L (kBlock), R (kBlock), out;
    // Settle the comp's bypass crossfade and smoothers before the impulse.
    for (int b = 0; b < 8; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        chain.processInPlace (L.data(), R.data(), kBlock);
    }
    for (int b = 0; b < 4; ++b)
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        if (b == 0) { L[(size_t) kImpulseAt] = 0.5f; R[(size_t) kImpulseAt] = 0.5f; }
        chain.processInPlace (L.data(), R.data(), kBlock);
        out.insert (out.end(), L.begin(), L.end());
    }
    return out;
}

int landedAt (const std::vector<float>& response)
{
    std::size_t at = 0;
    for (std::size_t i = 1; i < response.size(); ++i)
        if (std::abs (response[i]) > std::abs (response[at])) at = i;
    REQUIRE (std::abs (response[at]) > 0.1f);
    return (int) at - kImpulseAt;
}
} // namespace
#endif

TEST_CASE ("MasteringChain: the output lands at the reported latency with the comp in or out, at 1x, 2x and 4x",
           "[MasteringChain][latency]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    // The EQ's and the limiter's 4x oversamplers each have a fractional latency the
    // integer report rounds, so an impulse's peak can land a sample either side per stage.
    constexpr int kSlack = 2;

    for (const int factor : { 1, 2, 4 })
    {
        CAPTURE (factor);
        duskstudio::MasteringParams params;
        duskstudio::MasteringChain chain;
        prepareTransparent (chain, params, factor);
        const int reported = chain.getLatencySamples();

        for (const bool compOn : kCompStates)
        {
            CAPTURE (compOn);
            const int landed = landedAt (impulseResponse (chain, params, compOn));
            CAPTURE (reported, landed);
            CHECK (chain.getLatencySamples() == reported);
            CHECK (std::abs (landed - reported) <= kSlack);
        }
    }
#else
    SKIP ("requires the mastering compressor donor");
#endif
}

TEST_CASE ("MasteringChain: the Effect Oversampling choice leaves the mastering output unchanged",
           "[MasteringChain][latency]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    // The EQ and the limiter run fixed 4x oversamplers and the multiband comp runs
    // at the session rate, so no stage takes the engine-wide factor.
    std::vector<std::vector<float>> at1x;
    for (const int factor : { 1, 2, 4 })
    {
        CAPTURE (factor);
        duskstudio::MasteringParams params;
        duskstudio::MasteringChain chain;
        prepareTransparent (chain, params, factor);
        for (std::size_t state = 0; state < std::size (kCompStates); ++state)
        {
            CAPTURE (kCompStates[state]);
            const auto response = impulseResponse (chain, params, kCompStates[state]);
            if (factor == 1)
            {
                at1x.push_back (response);
                continue;
            }
            REQUIRE (response.size() == at1x[state].size());
            float maxDiff = 0.0f;
            for (std::size_t i = 0; i < response.size(); ++i)
                maxDiff = std::max (maxDiff, std::abs (response[i] - at1x[state][i]));
            CHECK_THAT (maxDiff, WithinAbs (0.0, 1.0e-6));
        }
    }
#else
    SKIP ("requires the mastering compressor donor");
#endif
}
