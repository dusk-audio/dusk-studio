#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BusStrip.h"
#include "dsp/BusToneEq.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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
    strip.prepare (kSr, kBlock);
    strip.bind (params);

    const float amp = juce::Decibels::decibelsToGain (-12.0f);
    std::vector<float> outL, outR;
    // Settle past the smoother ramp, then capture.
    driveSine (strip, 0.0, 1000.0, amp, 16, &outL, &outR, 8);

    // EQ and comp are disabled, so the only gain is the equal-power
    // center-pan law (cos/sin(pi/4) * sqrt2 = 1.0, so the pan contributes no
    // gain change at center).
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
    strip.prepare (kSr, kBlock);
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
    strip.prepare (kSr, kBlock);
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
    strip.prepare (kSr, kBlock);
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
    // (Multi-Comp 2's Analog Noise is off, and no stage stores it).
    REQUIRE (outPeak < 1.0e-4f);
}

namespace
{
// Steady-state gain in dB the strip applies to a sine at `hz`, measured over
// whole periods (every frequency used here has an integer period at 48 kHz).
double stripGainDb (duskstudio::BusParams& params, double hz)
{
    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock);
    strip.bind (params);
    const float amp = 0.1f;
    std::vector<float> outL, outR;
    driveSine (strip, 0.0, hz, amp, 40, &outL, &outR, 20);
    const auto period = (size_t) std::lround (kSr / hz);
    outL.resize (outL.size() / period * period);
    return 20.0 * std::log10 ((double) rms (outL) / ((double) amp / std::sqrt (2.0)));
}
} // namespace

TEST_CASE ("BusStrip: the bus EQ plays the Tone EQ spec", "[BusStrip]")
{
    // +/-9 dB is 9 dB at the band: the MID bell at its 800 Hz centre, and the
    // shelves on their plateaux (half the gain at their 300 Hz / 2 kHz points).
    using duskstudio::BusToneEq;
    struct Case { std::atomic<float> duskstudio::BusParams::* gain; BusToneEq::Band band; float db; double hz; };
    const Case cases[] {
        { &duskstudio::BusParams::eqMidGainDb, BusToneEq::Mid,  -9.0f,   800.0 },
        { &duskstudio::BusParams::eqMidGainDb, BusToneEq::Mid,   9.0f,   800.0 },
        { &duskstudio::BusParams::eqLfGainDb,  BusToneEq::Low,   9.0f,   300.0 },
        { &duskstudio::BusParams::eqLfGainDb,  BusToneEq::Low,   9.0f,    40.0 },
        { &duskstudio::BusParams::eqHfGainDb,  BusToneEq::High, -9.0f,  2000.0 },
        { &duskstudio::BusParams::eqHfGainDb,  BusToneEq::High, -9.0f, 12000.0 },
    };
    for (const auto& c : cases)
    {
        duskstudio::BusParams params;
        params.eqEnabled.store (true);
        (params.*(c.gain)).store (c.db);
        const double expected = BusToneEq::magnitudeDb (BusToneEq::bandCoeffs (c.band, kSr, c.db), kSr, c.hz);
        CAPTURE ((int) c.band, c.db, c.hz, expected);
        CHECK_THAT (stripGainDb (params, c.hz), WithinAbs (expected, 0.02));
    }
    duskstudio::BusParams mid;
    mid.eqEnabled.store (true);
    mid.eqMidGainDb.store (9.0f);
    CHECK_THAT (stripGainDb (mid, 800.0), WithinAbs (9.0, 0.02));
}

TEST_CASE ("BusStrip: the highpass cuts below its corner and follows the EQ switch", "[BusStrip]")
{
    // The session's knob range is the one the EQ plays.
    CHECK_THAT (duskstudio::BusParams::kHpfMinHz, WithinAbs (duskstudio::BusToneEq::kHpfMinHz, 0.0f));
    CHECK_THAT (duskstudio::BusParams::kHpfMaxHz, WithinAbs (duskstudio::BusToneEq::kHpfMaxHz, 0.0f));

    duskstudio::BusParams params;
    params.eqEnabled.store (true);
    params.hpfEnabled.store (true);
    params.hpfFreq.store (3000.0f);
    CHECK_THAT (stripGainDb (params, 3000.0), WithinAbs (-3.01, 0.02));
    CHECK_THAT (stripGainDb (params, 300.0),  WithinAbs (-40.0, 0.1));

    // The EQ's status light takes the highpass out with the bands.
    params.eqEnabled.store (false);
    CHECK_THAT (stripGainDb (params, 300.0), WithinAbs (0.0, 0.01));

    // So does its own switch (the knob's OFF floor).
    params.eqEnabled.store (true);
    params.hpfEnabled.store (false);
    CHECK_THAT (stripGainDb (params, 300.0), WithinAbs (0.0, 0.01));
}

// The knobs, a MIDI binding and anything else that moves the bus EQ go through
// BusParams' move helpers: a band move engages a bypassed EQ, which then plays
// it, and so does the highpass leaving OFF. Turned back to OFF, the highpass
// switches off and leaves the EQ engaged.
TEST_CASE ("BusStrip: a band move or the highpass leaving OFF engages the bus EQ", "[BusStrip]")
{
    duskstudio::BusParams band;
    REQUIRE_FALSE (band.eqEnabled.load());
    band.moveEqGain (band.eqMidGainDb, 9.0f);
    CHECK (band.eqEnabled.load());
    CHECK_THAT (stripGainDb (band, 800.0), WithinAbs (9.0, 0.02));

    duskstudio::BusParams hpf;
    CHECK_FALSE (hpf.moveHpf (duskstudio::BusParams::kHpfOffHz));
    CHECK_FALSE (hpf.eqEnabled.load());
    CHECK (hpf.moveHpf (3000.0f));
    CHECK (hpf.hpfEnabled.load());
    CHECK (hpf.eqEnabled.load());
    CHECK_THAT (stripGainDb (hpf, 300.0), WithinAbs (-40.0, 0.1));

    CHECK_FALSE (hpf.moveHpf (duskstudio::BusParams::kHpfOffHz));
    CHECK_FALSE (hpf.hpfEnabled.load());
    CHECK (hpf.eqEnabled.load());
}

TEST_CASE ("BusStrip: turning the comp off leaves no silent gap", "[BusStrip]")
{
    // The first comp-off block plays the input straight through, not a gap
    // while anything refills.
    duskstudio::BusParams params;
    params.compEnabled.store (true, std::memory_order_relaxed);

    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock);
    strip.bind (params);

    const float amp = juce::Decibels::decibelsToGain (-12.0f);
    double phase = driveSine (strip, 0.0, 1000.0, amp, 16);   // settle comp-on

    params.compEnabled.store (false, std::memory_order_relaxed);
    std::vector<float> outL, outR;
    driveSine (strip, phase, 1000.0, amp, 1, &outL, &outR, 0);

    // Longest run of near-silent samples in the first comp-off block. A 1 kHz
    // sine at 48 k crosses zero one sample at a time.
    int run = 0, maxRun = 0;
    for (float x : outL)
    {
        run    = (std::abs (x) < 1.0e-6f) ? run + 1 : 0;
        maxRun = std::max (maxRun, run);
    }
    REQUIRE (maxRun < 8);
}

TEST_CASE ("BusStrip: hot input stays finite", "[BusStrip]")
{
    duskstudio::BusParams params;
    params.eqEnabled.store (true, std::memory_order_relaxed);
    params.eqLfGainDb.store (9.0f, std::memory_order_relaxed);
    params.eqHfGainDb.store (9.0f, std::memory_order_relaxed);
    params.compEnabled.store (true, std::memory_order_relaxed);
    params.comp.set (duskstudio::CompParamId::BusThreshold, -30.0f);
    params.comp.set (duskstudio::CompParamId::BusMakeup, 12.0f);

    duskstudio::BusStrip strip;
    strip.prepare (kSr, kBlock);
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

TEST_CASE ("BusStrip: meters read the post-fader peak and a 300 ms VU per side", "[BusStrip][meters]")
{
    // 480 samples is 10 ms, so a whole number of blocks lands on the VU's
    // 300 ms time constant.
    constexpr int kMeterBlock = 480;
    duskstudio::BusParams params;
    duskstudio::BusStrip strip;
    strip.prepare (kSr, kMeterBlock);
    strip.bind (params);

    const float ampL = 0.5f, ampR = 0.25f;
    const double steadyL = ampL / std::sqrt (2.0);
    const double steadyR = ampR / std::sqrt (2.0);
    double phase = 0.0;
    auto drive = [&] (int blocks)
    {
        const double inc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSr;
        std::vector<float> L (kMeterBlock), R (kMeterBlock);
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kMeterBlock; ++i)
            {
                const float s = (float) std::sin (phase);
                phase += inc;
                L[(size_t) i] = ampL * s;
                R[(size_t) i] = ampR * s;
            }
            strip.processInPlace (L.data(), R.data(), kMeterBlock);
        }
    };

    SECTION ("the VU integrates over 300 ms")
    {
        // From silence, a first-order 300 ms integrator reaches 1 - 1/e of the
        // tone's RMS after 300 ms and 1 - 1/e^3 after 900 ms.
        drive (30);
        REQUIRE_THAT (params.meterPostBusRmsL.load(), WithinRel ((float) (steadyL * (1.0 - std::exp (-1.0))), 0.02f));
        REQUIRE_THAT (params.meterPostBusRmsR.load(), WithinRel ((float) (steadyR * (1.0 - std::exp (-1.0))), 0.02f));
        drive (60);
        REQUIRE_THAT (params.meterPostBusRmsL.load(), WithinRel ((float) (steadyL * (1.0 - std::exp (-3.0))), 0.02f));
        REQUIRE_THAT (params.meterPostBusRmsR.load(), WithinRel ((float) (steadyR * (1.0 - std::exp (-3.0))), 0.02f));
    }

    SECTION ("peak and VU read after the fader")
    {
        params.liveFaderDb.store (-6.0f);
        drive (400);
        const double g = std::pow (10.0, -6.0 / 20.0);
        // A 1 kHz sine puts a sample on its crest every period at 48 kHz.
        REQUIRE_THAT (params.meterPostBusLDb.load(), WithinAbs (20.0 * std::log10 (ampL * g), 0.05));
        REQUIRE_THAT (params.meterPostBusRDb.load(), WithinAbs (20.0 * std::log10 (ampR * g), 0.05));
        REQUIRE_THAT (params.meterPostBusRmsL.load(), WithinRel ((float) (steadyL * g), 0.01f));
        REQUIRE_THAT (params.meterPostBusRmsR.load(), WithinRel ((float) (steadyR * g), 0.01f));
    }
}

TEST_CASE ("BusStrip: silent-input skipping preserves delayed and filtered output", "[BusStrip][drain]")
{
    for (int blockSize : { 1, 16, 64, 512 })
        for (int effect : { 0, 1, 2 })
        {
            CAPTURE (blockSize, effect);
            duskstudio::BusParams params;
            params.eqEnabled.store (effect == 1);
            params.hpfEnabled.store (effect == 1);
            params.hpfFreq.store (20.0f);
            params.compEnabled.store (effect == 2);
            duskstudio::BusStrip gated, reference;
            for (auto* strip : { &gated, &reference })
            {
                strip->bind (params);
                strip->prepare (kSr, blockSize);
            }
            REQUIRE_FALSE (gated.hasPendingDrain());
            std::vector<float> left ((std::size_t) blockSize), right (left),
                               expectedLeft (left), expectedRight (left);
            float worst = 0.0f, referenceTail = 0.0f;
            bool slept = false;
            for (int block = 0; block < 48000 / blockSize + 2; ++block)
            {
                std::fill (left.begin(), left.end(), 0.0f);
                std::fill (right.begin(), right.end(), 0.0f);
                if (block == 0)
                {
                    left.back() = 0.5f;
                    right.back() = -0.25f;
                }
                expectedLeft = left;
                expectedRight = right;
                reference.processInPlace (expectedLeft.data(), expectedRight.data(), blockSize);
                if (block == 0 || gated.hasPendingDrain())
                    gated.processInPlace (left.data(), right.data(), blockSize);
                else
                    slept = true;
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto i = (std::size_t) sample;
                    worst = std::max (worst, std::abs (left[i] - expectedLeft[i]));
                    worst = std::max (worst, std::abs (right[i] - expectedRight[i]));
                    if (block > 0) referenceTail = std::max (referenceTail, std::abs (expectedLeft[i]));
                }
            }
            CHECK (worst < 2.0e-6f);
            CHECK (slept);
            CHECK_FALSE (gated.hasPendingDrain());
            // The filter rings past the impulse and the compressor delays it.
            if (effect != 0) CHECK (referenceTail > 1.0e-6f);
        }
}

TEST_CASE ("BusStrip: a closed fader cannot hide a pending filter tail", "[BusStrip][drain]")
{
    constexpr int blockSize = 64;
    duskstudio::BusParams params;
    params.eqEnabled.store (true);
    params.hpfEnabled.store (true);
    params.hpfFreq.store (20.0f);
    params.liveFaderDb.store (duskstudio::ChannelStripParams::kFaderMinDb);
    duskstudio::BusStrip gated, reference;
    for (auto* strip : { &gated, &reference })
    {
        strip->bind (params);
        strip->prepare (kSr, blockSize);
    }
    std::array<float, blockSize> left {}, right {}, expectedLeft {}, expectedRight {};
    float worst = 0.0f, reopenedTail = 0.0f;
    for (int block = 0; block < 1000; ++block)
    {
        if (block == 60)
        {
            CHECK (gated.hasPendingDrain());
            params.liveFaderDb.store (0.0f);
        }
        left.fill (block < 20 ? 0.5f : 0.0f);
        right = expectedLeft = expectedRight = left;
        reference.processInPlace (expectedLeft.data(), expectedRight.data(), blockSize);
        if (block < 20 || gated.hasPendingDrain())
            gated.processInPlace (left.data(), right.data(), blockSize);
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto i = (std::size_t) sample;
            worst = std::max (worst, std::abs (left[i] - expectedLeft[i]));
            if (block >= 60) reopenedTail = std::max (reopenedTail, std::abs (expectedLeft[i]));
        }
    }
    CHECK (reopenedTail > 1.0e-5f);
    CHECK (worst < 2.0e-6f);
    CHECK_FALSE (gated.hasPendingDrain());
    left.fill (0.5f);
    right = left;
    gated.processInPlace (left.data(), right.data(), blockSize);
    REQUIRE (gated.hasPendingDrain());
    gated.prepare (kSr, blockSize);
    CHECK_FALSE (gated.hasPendingDrain());
}

TEST_CASE ("BusStrip: an explicit output gate discards held audio before resuming", "[BusStrip][drain]")
{
    for (bool compressor : { false, true })
    {
        CAPTURE (compressor);
        duskstudio::BusParams params;
        params.eqEnabled.store (true);
        params.hpfEnabled.store (true);
        params.hpfFreq.store (20.0f);
        params.compEnabled.store (compressor);
        duskstudio::BusStrip strip, kept;
        for (auto* bus : { &strip, &kept })
        {
            bus->bind (params);
            bus->prepare (kSr, 16);
            std::array<float, 16> left {}, right {};
            left.back() = right.back() = 0.5f;
            bus->processInPlace (left.data(), right.data(), 16);
            REQUIRE (bus->hasPendingDrain());
        }
        strip.discardPendingDrain();
        REQUIRE_FALSE (strip.hasPendingDrain());

        // Silence through both, until the discarded bus has drained or 2 s
        // have gone by.
        float worst = 0.0f, keptWorst = 0.0f;
        for (int block = 0; block < 6000 && (block < 300 || strip.hasPendingDrain()); ++block)
        {
            std::array<float, 16> left {}, right {}, keptLeft {}, keptRight {};
            strip.processInPlace (left.data(), right.data(), 16);
            kept.processInPlace (keptLeft.data(), keptRight.data(), 16);
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                worst = std::max ({ worst, std::abs (left[i]), std::abs (right[i]) });
                keptWorst = std::max ({ keptWorst, std::abs (keptLeft[i]), std::abs (keptRight[i]) });
            }
        }
        CAPTURE (worst, keptWorst);
        CHECK_FALSE (strip.hasPendingDrain());
        if (! compressor)
        {
            CHECK (worst < 1.0e-7f);
            continue;
        }
        // The compressor's delay holds the impulse past its block, so the
        // kept bus plays it and the discarded one must not. Its
        // analog-modelled filters keep their state and decay on their own,
        // which Multi-Comp 2 offers a host no audio-thread reset for, so
        // the discarded bus is held to staying far under the impulse.
        CHECK (keptWorst > 0.1f);
        CHECK (worst < 1.0e-3f);
    }
}
