#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MasteringChain.h"
#include "dsp/BrickwallLimiter.h"
#include "engine/builtin/DafPlugin.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using duskstudio::CompBandField;
using duskstudio::CompParamId;
using duskstudio::compBandParam;

TEST_CASE ("MasteringChain: latest scope window wraps and resets on prepare", "[MasteringChain][scope]")
{
    constexpr int capacity = 16384, block = 512;
    duskstudio::MasteringParams params;
    params.compEnabled.store (false);
    params.limiterEnabled.store (false);
    duskstudio::MasteringChain chain;
    chain.bind (params);
    std::vector<float> window (capacity + 1, -7.0f);
    REQUIRE (chain.readScopeLatest (window.data(), capacity) == 0);
    chain.prepare (48000.0, block);
    REQUIRE_THAT (chain.getScopeSampleRate(), WithinAbs (48000.0, 1.0e-6));
    REQUIRE (chain.readScopeLatest (nullptr, capacity) == 0);
    REQUIRE (chain.readScopeLatest (window.data(), 0) == 0);
    REQUIRE (chain.readScopeLatest (window.data(), -1) == 0);
    REQUIRE_THAT (window.front(), WithinAbs (-7.0f, 1.0e-7f));
    REQUIRE (chain.readScopeLatest (window.data(), capacity + 1) == capacity);
    for (int n = 0; n < capacity; ++n)
        REQUIRE_THAT (window[(std::size_t) n], WithinAbs (0.0f, 1.0e-7f));
    REQUIRE_THAT (window.back(), WithinAbs (-7.0f, 1.0e-7f));

    duskstudio::MasteringDigitalEq reference;
    reference.prepare (48000.0, block);
    reference.setEnabled (false);
    std::array<float, block> left {}, right {}, expectedLeft {}, expectedRight {};
    std::vector<float> expected;
    for (int batch = 0; batch < 40; ++batch)
    {
        for (int n = 0; n < block; ++n)
        {
            const int sample = batch * block + n;
            left[(std::size_t) n] = (float) ((sample * 37) % 2047 - 1023) * 0.0001f;
            right[(std::size_t) n] = (float) ((sample * 17) % 1021 - 510) * 0.0002f;
        }
        expectedLeft = left; expectedRight = right;
        reference.processInPlace (expectedLeft.data(), expectedRight.data(), block);
        for (int n = 0; n < block; ++n)
            expected.push_back (0.5f * (expectedLeft[(std::size_t) n] + expectedRight[(std::size_t) n]));
        chain.processInPlace (left.data(), right.data(), block, false);
    }
    REQUIRE (chain.readScopeLatest (window.data(), capacity + 1) == capacity);
    for (int n = 0; n < capacity; ++n)
        REQUIRE_THAT (window[(std::size_t) n], WithinAbs (expected[expected.size() - capacity + (std::size_t) n], 1.0e-7f));
    REQUIRE_THAT (window.back(), WithinAbs (-7.0f, 1.0e-7f));
    REQUIRE (chain.readScopeLatest (window.data(), 17) == 17);
    for (std::size_t n = 0; n < 17; ++n)
        REQUIRE_THAT (window[n], WithinAbs (expected[expected.size() - 17 + n], 1.0e-7f));
    chain.processInPlace (left.data(), right.data(), 0, false);
    REQUIRE (chain.readScopeLatest (window.data(), 1) == 1);
    REQUIRE_THAT (window.front(), WithinAbs (expected.back(), 1.0e-7f));

    chain.prepare (96000.0, 64);
    REQUIRE_THAT (chain.getScopeSampleRate(), WithinAbs (96000.0, 1.0e-6));
    REQUIRE (chain.readScopeLatest (window.data(), capacity) == capacity);
    for (int n = 0; n < capacity; ++n)
        REQUIRE_THAT (window[(std::size_t) n], WithinAbs (0.0f, 1.0e-7f));
}

#if DUSKSTUDIO_HAS_DUSK_DSP
namespace
{
int paramIndex (duskstudio::MultiCompStage& stage, const std::string& symbol)
{
    const auto& descs = stage.plugin().params();
    for (std::size_t i = 0; i < descs.size(); ++i)
        if (descs[i].symbol == symbol)
            return (int) i;
    return -1;
}

// The plain number Multi-Comp 2 prints for a host value: "2 kHz" reads 2000.
double printed (const duskstudio::MultiCompStage& stage, CompParamId id, float host)
{
    const auto text = stage.valueText (id, host);
    CAPTURE (text);
    char* end = nullptr;
    const double number = std::strtod (text.c_str(), &end);
    REQUIRE (end != text.c_str());
    return text.find ("kHz") != std::string::npos ? number * 1000.0 : number;
}
} // namespace
#endif

TEST_CASE ("Appendix A: active multiband compressor defaults and control ranges",
           "[MasteringChain][appendix]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    // The session's defaults are what the compressor plays, read in the
    // plug-in's own units; its ranges are the host ranges the session clamps to.
    duskstudio::MasteringParams params;
    duskstudio::MasteringChain chain;
    chain.bind (params);
    chain.prepare (48000.0, 512);
    auto& stage = chain.getCompressor();
    REQUIRE (stage.kind() == duskstudio::CompStage::Multiband);
    const int mode = paramIndex (stage, "mode");
    REQUIRE (mode >= 0);
    REQUIRE_THAT (stage.plugin().getParameterValue ((std::uint32_t) mode), WithinAbs (7, 0));
    REQUIRE_THAT (params.comp.get (CompParamId::AutoMakeup), WithinAbs (0, 0));

    auto check = [&] (CompParamId id, double low, double high, double initial)
    {
        const auto& spec = duskstudio::compParamSpec (id);
        CAPTURE (spec.symbol);
        const auto about = [] (double want) { return WithinAbs (want, std::max (0.05, std::abs (want) * 0.005)); };
        CHECK_THAT (printed (stage, id, spec.hostMin), about (low));
        CHECK_THAT (printed (stage, id, spec.hostMax), about (high));
        CHECK_THAT (printed (stage, id, params.comp.get (id)), about (initial));
    };
    auto checkSwitch = [&] (CompParamId id, float initial)
    {
        const auto& spec = duskstudio::compParamSpec (id);
        CAPTURE (spec.symbol);
        CHECK (spec.integer);
        CHECK_THAT (spec.hostMin, WithinAbs (0, 0));
        CHECK_THAT (spec.hostMax, WithinAbs (1, 0));
        CHECK_THAT (params.comp.get (id), WithinAbs (initial, 0));
    };

    check (CompParamId::MbCrossover1, 20, 500, 200);
    check (CompParamId::MbCrossover2, 200, 5000, 2000);
    check (CompParamId::MbCrossover3, 2000, 16000, 8000);
    check (CompParamId::MbOutput, -24, 24, 0);
    check (CompParamId::MbMix, 0, 100, 100);
    for (int band = 0; band < duskstudio::kCompBandCount; ++band)
    {
        CAPTURE (band);
        check (compBandParam (band, CompBandField::Threshold), -60, 0, -20);
        check (compBandParam (band, CompBandField::Ratio), 1, 20, 4);
        check (compBandParam (band, CompBandField::Attack), 0.1, 100, 10);
        check (compBandParam (band, CompBandField::Release), 10, 1000, 100);
        check (compBandParam (band, CompBandField::Makeup), -12, 12, 0);
        checkSwitch (compBandParam (band, CompBandField::Enabled), 1.0f);
        checkSwitch (compBandParam (band, CompBandField::Solo), 0.0f);
        checkSwitch (compBandParam (band, CompBandField::Bypass), 0.0f);
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
    chain.prepare (kSr, kBlock);
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
    chain.prepare (kSr, kBlock);

    duskstudio::BrickwallLimiter limRef;
    limRef.prepare (kSr, kBlock);
    duskstudio::MasteringDigitalEq eqRef;
    eqRef.prepare (kSr, kBlock);

    int compLatency = 0;
#if DUSKSTUDIO_HAS_DUSK_DSP
    compLatency = chain.getCompressor().latencySamples();
    REQUIRE (compLatency > 0);
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
    chain.prepare (kSr, kBlock);
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
    chain.prepare (kSr, kBlock);
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

TEST_CASE ("MasteringChain: phase capture observes the stereo output without changing it",
           "[MasteringChain][phase]")
{
    duskstudio::MasteringParams params;
    params.limiterDriveDb.store (6.0f, std::memory_order_relaxed);
    duskstudio::MasteringChain measured, reference;
    measured.bind (params);
    reference.bind (params);
    measured.prepare (kSr, kBlock);
    reference.prepare (kSr, kBlock);
    const int latency = measured.getLatencySamples();
    REQUIRE (latency == reference.getLatencySamples());
    std::vector<float> left (kBlock), right (kBlock), refL (kBlock), refR (kBlock);
    std::vector<float> feedL (kBlock), feedR (kBlock);
    auto& feed = measured.getPhaseFeed();
    duskstudio::PhaseAnalysisFeed::Chunk previous;
    float outputPeak = 0.0f, stereoDifference = 0.0f;
    for (int block = 0; block < 24; ++block)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const double time = (block * kBlock + i) / kSr;
            left[(size_t) i] = refL[(size_t) i] = 1.5f * (float) std::sin (time * 6283.185307);
            right[(size_t) i] = refR[(size_t) i] = 0.75f * (float) std::cos (time * 4398.229715);
        }
        measured.processInPlace (left.data(), right.data(), kBlock);
        reference.processInPlace (refL.data(), refR.data(), kBlock, false);
        const auto chunk = feed.read (feedL.data(), feedR.data(), kBlock);
        REQUIRE (chunk.samples == kBlock);
        REQUIRE_THAT (chunk.sampleRate, WithinAbs (kSr, 1.0e-6));
        if (block > 0)
        {
            REQUIRE (chunk.generation == previous.generation);
            REQUIRE (chunk.sequence == previous.sequence + kBlock);
        }
        for (int i = 0; i < kBlock; ++i)
        {
            const auto index = (size_t) i;
            REQUIRE_THAT (feedL[index], WithinAbs (left[index], 1.0e-6));
            REQUIRE_THAT (feedR[index], WithinAbs (right[index], 1.0e-6));
            REQUIRE_THAT (left[index], WithinAbs (refL[index], 1.0e-6));
            REQUIRE_THAT (right[index], WithinAbs (refR[index], 1.0e-6));
            outputPeak = std::max (outputPeak, std::abs (left[index]));
            stereoDifference = std::max (stereoDifference, std::abs (left[index] - right[index]));
        }
        previous = chunk;
    }
    REQUIRE (outputPeak > 0.1f);
    REQUIRE (stereoDifference > 0.1f);
    REQUIRE (measured.getLatencySamples() == latency);
    REQUIRE (reference.getPhaseFeed().pendingSamples() == 0);

    measured.processInPlace (left.data(), right.data(), kBlock, false);
    REQUIRE (feed.pendingSamples() == 0);
    measured.processInPlace (left.data(), right.data(), kBlock);
    const auto resumed = feed.read (feedL.data(), feedR.data(), kBlock);
    REQUIRE (resumed.samples == kBlock);
    REQUIRE (resumed.generation != previous.generation);
    REQUIRE (resumed.sequence == 0);

    measured.processInPlace (left.data(), right.data(), kBlock);
    measured.prepare (96000.0, kBlock);
    measured.processInPlace (left.data(), right.data(), kBlock);
    const auto old = feed.read (feedL.data(), feedR.data(), kBlock);
    REQUIRE (old.samples == kBlock);
    REQUIRE (old.generation == resumed.generation);
    REQUIRE_THAT (old.sampleRate, WithinAbs (kSr, 1.0e-6));
    const auto prepared = feed.read (feedL.data(), feedR.data(), kBlock);
    REQUIRE (prepared.samples == kBlock);
    REQUIRE (prepared.generation != old.generation);
    REQUIRE_THAT (prepared.sampleRate, WithinAbs (96000.0, 1.0e-6));
}

#if DUSKSTUDIO_HAS_DUSK_DSP
namespace
{
constexpr int kImpulseAt = 100;
constexpr bool kCompStates[] = { false, true, false };

// Thresholds at the top of their range, so the comp passes a quiet signal
// unreduced.
void prepareTransparent (duskstudio::MasteringChain& chain, duskstudio::MasteringParams& params)
{
    for (int band = 0; band < duskstudio::kCompBandCount; ++band)
        params.comp.set (compBandParam (band, CompBandField::Threshold), 0.0f);
    chain.bind (params);
    chain.prepare (kSr, kBlock);
}

std::vector<float> impulseResponse (duskstudio::MasteringChain& chain,
                                    duskstudio::MasteringParams& params, bool compOn)
{
    params.compEnabled.store (compOn, std::memory_order_relaxed);
    std::vector<float> L (kBlock), R (kBlock), out;
    // Settle the switch's crossfade before the impulse.
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

float maxStep (const std::vector<float>& v)
{
    float worst = 0.0f;
    for (std::size_t i = 1; i < v.size(); ++i)
        worst = std::max (worst, std::abs (v[i] - v[i - 1]));
    return worst;
}
} // namespace
#endif

TEST_CASE ("MasteringChain: the output lands at the reported latency with the comp in or out",
           "[MasteringChain][latency]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    // The EQ's and the limiter's 4x oversamplers each have a fractional latency the
    // integer report rounds, so an impulse's peak can land a sample either side per stage.
    constexpr int kSlack = 2;

    duskstudio::MasteringParams params;
    duskstudio::MasteringChain chain;
    prepareTransparent (chain, params);
    const int reported = chain.getLatencySamples();

    int landedOut = 0;
    for (const bool compOn : kCompStates)
    {
        CAPTURE (compOn);
        const int landed = landedAt (impulseResponse (chain, params, compOn));
        CAPTURE (reported, landed);
        CHECK (chain.getLatencySamples() == reported);
        CHECK (std::abs (landed - reported) <= kSlack);
        // In, the band crossovers sum to an allpass whose phase can move the
        // peak a sample or two off the plain delay's.
        if (! compOn) landedOut = landed;
        else          CHECK (std::abs (landed - landedOut) <= kSlack);
    }
#else
    SKIP ("requires the mastering compressor donor");
#endif
}

TEST_CASE ("MasteringChain: switching the compressor while a tone plays crossfades without a click",
           "[MasteringChain][latency]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    duskstudio::MasteringParams params;
    params.limiterEnabled.store (false, std::memory_order_relaxed);
    duskstudio::MasteringChain chain;
    prepareTransparent (chain, params);

    static constexpr double kHz = 440.0;
    static constexpr float kAmp = 0.25f;
    std::vector<float> L (kBlock), R (kBlock), out;
    double phase = 0.0;
    for (int b = 0; b < 72; ++b)
    {
        // In at block 12, out at block 40: each switch while the tone plays.
        params.compEnabled.store (b >= 12 && b < 40, std::memory_order_relaxed);
        for (int i = 0; i < kBlock; ++i)
        {
            L[(size_t) i] = R[(size_t) i] = kAmp * (float) std::sin (phase);
            phase += 2.0 * 3.14159265358979323846 * kHz / kSr;
        }
        chain.processInPlace (L.data(), R.data(), kBlock);
        out.insert (out.end(), L.begin(), L.end());
    }
    // Every sample-to-sample move stays within the tone's own steepest one,
    // with room for the crossover's phase and the fade's slope.
    const float toneStep = kAmp * (float) (2.0 * 3.14159265358979323846 * kHz / kSr);
    CHECK (maxStep (out) < 1.25f * toneStep);
    CHECK_THAT (params.meterCompGrDb.load(), WithinAbs (0.0f, 0.0f));
#else
    SKIP ("requires the mastering compressor donor");
#endif
}

TEST_CASE ("MasteringChain: with the compressor out the plug-in does not run", "[MasteringChain]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    // A hot signal over the default thresholds: any block the plug-in runs
    // moves its gain-reduction outputs.
    duskstudio::MasteringParams params;
    params.limiterEnabled.store (false, std::memory_order_relaxed);
    duskstudio::MasteringChain chain;
    chain.bind (params);
    chain.prepare (kSr, kBlock);
    auto& stage = chain.getCompressor();

    std::vector<int> meters;
    for (const auto* symbol : { "gr_meter", "gr_low", "gr_lowmid", "gr_highmid", "gr_high" })
    {
        meters.push_back (paramIndex (stage, symbol));
        REQUIRE (meters.back() >= 0);
    }
    const auto readings = [&]
    {
        std::vector<float> v;
        for (const int index : meters)
            v.push_back (stage.plugin().getParameterValue ((std::uint32_t) index));
        return v;
    };
    double phase = 0.0;
    const auto drive = [&] (float amp, int blocks)
    {
        std::vector<float> L (kBlock), R (kBlock);
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = amp * (0.25f * (float) std::sin (phase * 0.01) + 0.25f * (float) std::sin (phase * 0.13)
                                       + 0.25f * (float) std::sin (phase * 0.6) + 0.25f * (float) std::sin (phase * 1.6));
                phase += 1.0;
                L[(size_t) i] = R[(size_t) i] = s;
            }
            chain.processInPlace (L.data(), R.data(), kBlock);
        }
    };

    params.compEnabled.store (false, std::memory_order_relaxed);
    drive (1.5f, 40);
    for (const float reading : readings())
        CHECK_THAT (reading, WithinAbs (0.0f, 0.0f));
    CHECK_THAT (params.meterCompGrDb.load(), WithinAbs (0.0f, 0.0f));

    params.compEnabled.store (true, std::memory_order_relaxed);
    drive (1.5f, 40);
    CHECK (stage.gainReductionDb() < -1.0f);
    CHECK (params.meterCompGrDb.load() < -1.0f);

    // Out again: once the fade is over the readings hold, whatever plays.
    params.compEnabled.store (false, std::memory_order_relaxed);
    drive (1.5f, 40);
    const auto held = readings();
    drive (0.3f, 40);
    const auto later = readings();
    for (std::size_t i = 0; i < held.size(); ++i)
        CHECK_THAT (later[i], WithinAbs (held[i], 0.0f));
    CHECK_THAT (params.meterCompGrDb.load(), WithinAbs (0.0f, 0.0f));
#else
    SKIP ("requires the mastering compressor donor");
#endif
}

#if DUSKSTUDIO_HAS_DUSK_DSP
namespace
{
// Drives a chain with a signal loud enough for every band to compress.
struct LoudDrive
{
    explicit LoudDrive (duskstudio::MasteringChain& driven) : chain (driven) {}

    duskstudio::MasteringChain& chain;
    std::vector<float> out;
    double phase = 0.0;

    void blocks (int count)
    {
        std::vector<float> L (kBlock), R (kBlock);
        for (int b = 0; b < count; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const double x = phase;
                phase += 1.0;
                L[(size_t) i] = 1.5f * (0.25f * (float) std::sin (x * 0.01) + 0.25f * (float) std::sin (x * 0.13)
                                        + 0.25f * (float) std::sin (x * 0.6) + 0.25f * (float) std::sin (x * 1.6));
                R[(size_t) i] = 0.8f * L[(size_t) i];
            }
            chain.processInPlace (L.data(), R.data(), kBlock);
            out.insert (out.end(), L.begin(), L.end());
            out.insert (out.end(), R.begin(), R.end());
        }
    }
};
} // namespace
#endif

// The Multiband face reads the three crossovers back from the plug-in every
// frame, and every control when it opens. With the compressor out the plug-in
// does not run, yet while the face is open it holds the session's values.
TEST_CASE ("MasteringChain: with the compressor out, an open editor reads the session's values from the plug-in",
           "[MasteringChain][editor]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    duskstudio::MasteringParams params;
    params.compEnabled.store (false, std::memory_order_relaxed);
    params.limiterEnabled.store (false, std::memory_order_relaxed);
    duskstudio::MasteringChain chain;
    chain.bind (params);
    chain.prepare (kSr, kBlock);
    auto& stage = chain.getCompressor();
    LoudDrive drive { chain };
    drive.blocks (2);

    struct Edit { CompParamId id; const char* symbol; float value; };
    // Host values whose crossover frequencies are already in order, so the
    // plug-in reads back what it was given, to its 1 Hz step.
    const Edit edits[] { { CompParamId::MbCrossover1, "mb_crossover_1", 0.6f },
                         { CompParamId::MbCrossover3, "mb_crossover_3", 0.8f },
                         { compBandParam (1, CompBandField::Threshold), "mb_threshold_1", -33.0f },
                         { CompParamId::MbMix, "mb_mix", 60.0f } };
    stage.setEditorOpen (true);
    for (const auto& edit : edits)
        params.comp.set (edit.id, edit.value);
    drive.blocks (1);
    for (const auto& edit : edits)
    {
        CAPTURE (edit.symbol);
        const int index = paramIndex (stage, edit.symbol);
        REQUIRE (index >= 0);
        CHECK_THAT (stage.editorReading (index), WithinAbs (edit.value, 2.0e-3f));
    }
    CHECK_THAT (params.meterCompGrDb.load(), WithinAbs (0.0f, 0.0f));

    // Closed, a compressor that is out takes nothing.
    stage.setEditorOpen (false);
    params.comp.set (CompParamId::MbCrossover1, 0.3f);
    drive.blocks (2);
    CHECK_THAT (stage.editorReading (paramIndex (stage, "mb_crossover_1")), WithinAbs (0.6f, 2.0e-3f));
#else
    SKIP ("requires the mastering compressor donor");
#endif
}

// What an open editor had the plug-in take while it was out leaves its rest
// state alone: switched in again it plays exactly what it plays when the
// values reach it as it starts, as they do with no editor open.
TEST_CASE ("MasteringChain: values an open editor gave a compressor that is out play as they did without it",
           "[MasteringChain][editor]")
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    duskstudio::MasteringParams watchedParams, plainParams;
    duskstudio::MasteringChain watched, plain;
    for (auto* params : { &watchedParams, &plainParams })
        params->limiterEnabled.store (false, std::memory_order_relaxed);
    watched.bind (watchedParams);
    plain.bind (plainParams);
    watched.prepare (kSr, kBlock);
    plain.prepare (kSr, kBlock);
    LoudDrive watchedDrive { watched }, plainDrive { plain };

    const auto both = [&] (auto&& change, int blocks)
    {
        change (watchedParams);
        change (plainParams);
        watchedDrive.blocks (blocks);
        plainDrive.blocks (blocks);
    };
    using Params = duskstudio::MasteringParams;
    both ([] (Params& p) { p.compEnabled.store (true, std::memory_order_relaxed); }, 30);
    // Out long enough for the fade to end and the plug-in to go back to rest.
    both ([] (Params& p) { p.compEnabled.store (false, std::memory_order_relaxed); }, 30);
    watched.getCompressor().setEditorOpen (true);
    // A first set of values, then the one the compressor comes back with, so
    // the open editor's plug-in takes both and the other only the second.
    both ([] (Params& p)
    {
        p.comp.set (CompParamId::AutoMakeup, 1.0f);
        p.comp.set (CompParamId::MbMix, 30.0f);
        p.comp.set (CompParamId::MbCrossover2, 0.5f);
        p.comp.set (compBandParam (0, CompBandField::Threshold), -40.0f);
    }, 5);
    both ([] (Params& p)
    {
        p.comp.set (CompParamId::MbMix, 70.0f);
        p.comp.set (CompParamId::MbOutput, 3.0f);
        p.comp.set (CompParamId::MbCrossover1, 0.6f);
        p.comp.set (compBandParam (0, CompBandField::Threshold), -45.0f);
        p.comp.set (compBandParam (2, CompBandField::Ratio), 0.8f);
        p.comp.set (compBandParam (3, CompBandField::Enabled), 0.0f);
    }, 5);
    const int mix = paramIndex (watched.getCompressor(), "mb_mix");
    REQUIRE (mix >= 0);
    CHECK_THAT (watched.getCompressor().editorReading (mix), WithinAbs (70.0f, 1.0e-4f));
    CHECK_THAT (plain.getCompressor().editorReading (mix), WithinAbs (100.0f, 1.0e-4f));
    both ([] (Params& p) { p.compEnabled.store (true, std::memory_order_relaxed); }, 30);

    const auto& a = watchedDrive.out;
    const auto& b = plainDrive.out;
    REQUIRE (a.size() == b.size());
    std::size_t firstDifference = a.size();
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::memcmp (&a[i], &b[i], sizeof (float)) != 0)
        {
            firstDifference = i;
            break;
        }
    CAPTURE (firstDifference, a.size());
    CHECK (firstDifference == a.size());
    CHECK (watchedParams.meterCompGrDb.load() < -1.0f);
#else
    SKIP ("requires the mastering compressor donor");
#endif
}
