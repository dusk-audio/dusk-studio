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

float peak (const std::vector<float>& v)
{
    float p = 0.0f;
    for (float x : v) p = std::max (p, std::abs (x));
    return p;
}

// Inter-sample (true) peak of a mono buffer, measured by 4x oversampling -
// the same resolution the limiter controls internally. The FIR startup/flush
// transient at the buffer ends is discarded: oversampling a buffer that begins
// or ends mid-waveform rings the cold filter and reports a spurious peak ~1 dB
// over the real one (the value sits in the first ~100 oversampled samples),
// unrelated to the signal. Measuring only the warmed-up interior is the
// standard way to read a true-peak.
float truePeak4x (std::vector<float> mono)
{
    juce::dsp::Oversampling<float> os (
        1, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    os.initProcessing ((size_t) mono.size());
    os.reset();

    float* p[1] = { mono.data() };
    juce::dsp::AudioBlock<float> blk (p, 1, mono.size());
    auto up = os.processSamplesUp (blk);

    const int   n   = (int) up.getNumSamples();
    const int   lat = (int) std::lround (os.getLatencyInSamples());
    const int   skip = std::min (n / 4, 2 * lat + 256);   // FIR transient both ends
    const float* u   = up.getChannelPointer (0);
    float peak = 0.0f;
    for (int i = skip; i < n - skip; ++i)
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

TEST_CASE ("BrickwallLimiter: stereo-link off limits channels independently",
           "[BrickwallLimiter]")
{
    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);
    const float hot  = juce::Decibels::decibelsToGain (12.0f);   // well over ceiling
    const float quiet = juce::Decibels::decibelsToGain (-12.0f); // under ceiling

    // L hammered, R sits below the ceiling. With link OFF, R must pass at unity
    // (its own peak never exceeds the ceiling); with link ON, R is pulled down
    // to match L's gain reduction.
    auto run = [&] (bool linked)
    {
        duskstudio::BrickwallLimiter lim;
        lim.prepare (kSr, kBlock, kLookahead);
        lim.setCeilingDb (-1.0f);
        lim.setStereoLink (linked);
        lim.setEnabled (true);

        std::vector<float> L (kBlock), R (kBlock);
        double phase = 0.0;
        float rPeak = 0.0f;
        for (int b = 0; b < 12; ++b)
        {
            const double inc = 2.0 * juce::MathConstants<double>::pi * 1000.0 / kSr;
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = (float) std::sin (phase);
                phase += inc;
                L[(size_t) i] = hot * s;
                R[(size_t) i] = quiet * s;
            }
            lim.processInPlace (L.data(), R.data(), kBlock);
            if (b >= 6) rPeak = std::max (rPeak, peak (R));
        }
        return rPeak;
    };

    const float rUnlinked = run (false);
    const float rLinked   = run (true);

    REQUIRE (rLinked <= ceiling + 1.0e-4f);          // L's reduction drags R down
    REQUIRE_THAT (rUnlinked, WithinRel (quiet, 0.02f)); // R untouched on its own
    REQUIRE (rUnlinked > rLinked + 0.05f);           // the two modes clearly differ
}

TEST_CASE ("BrickwallLimiter: every mode holds the ceiling", "[BrickwallLimiter]")
{
    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);

    for (int mode = 0; mode < 3; ++mode)
    {
        duskstudio::BrickwallLimiter lim;
        lim.prepare (kSr, kBlock, kLookahead);
        lim.setCeilingDb (-1.0f);
        lim.setMode (mode);
        lim.setEnabled (true);

        std::vector<float> L (kBlock), R (kBlock);
        double phase = 0.0;
        float steadyPeak = 0.0f;
        for (int b = 0; b < 12; ++b)
        {
            fillSine (L, R, phase, 1000.0, juce::Decibels::decibelsToGain (9.0f));
            lim.processInPlace (L.data(), R.data(), kBlock);
            if (b >= 4) steadyPeak = std::max (steadyPeak, blockPeak (L, R));
        }
        INFO ("mode = " << mode);
        REQUIRE (steadyPeak <= ceiling + 1.0e-4f);
        REQUIRE (lim.getCurrentGrDb() < 0.0f);
    }
}

TEST_CASE ("BrickwallLimiter: release is program-dependent (deep GR recovers slower)",
           "[BrickwallLimiter]")
{
    // After a sustained limiting passage, release into silence and measure the
    // fraction of the original reduction still present over a fixed window. The
    // fraction is normalised by the starting depth, so it isolates the release
    // RATE (not the distance): a deeper, more sustained reduction must hold a
    // higher fraction (slower recovery) than a shallow one.
    auto remainingFraction = [] (float driveDb) -> float
    {
        duskstudio::BrickwallLimiter lim;
        lim.prepare (kSr, kBlock, kLookahead);
        lim.setCeilingDb (-1.0f);
        lim.setReleaseMs (200.0f);
        lim.setMode (0);              // Modern
        lim.setEnabled (true);

        std::vector<float> L (kBlock), R (kBlock);
        double phase = 0.0;
        const float amp = juce::Decibels::decibelsToGain (driveDb);

        float gr0 = 0.0f;            // settle the depth tracker
        for (int b = 0; b < 40; ++b)
        {
            fillSine (L, R, phase, 1000.0, amp);
            lim.processInPlace (L.data(), R.data(), kBlock);
            gr0 = lim.getCurrentGrDb();
        }
        const float env0 = juce::Decibels::decibelsToGain (gr0);
        if (1.0f - env0 < 1.0e-3f) return 0.0f;   // no meaningful reduction

        float fracSum = 0.0f; int n = 0;
        for (int b = 0; b < 6; ++b)
        {
            std::fill (L.begin(), L.end(), 0.0f);
            std::fill (R.begin(), R.end(), 0.0f);
            lim.processInPlace (L.data(), R.data(), kBlock);
            if (b >= 2)   // skip the blocks still flushing the lookahead tail
            {
                const float envT = juce::Decibels::decibelsToGain (lim.getCurrentGrDb());
                fracSum += (1.0f - envT) / (1.0f - env0);
                ++n;
            }
        }
        return fracSum / (float) n;
    };

    const float deep    = remainingFraction (12.0f);   // deep, sustained GR
    const float shallow = remainingFraction (1.5f);    // shallow GR
    INFO ("remaining-GR fraction: deep=" << deep << "  shallow=" << shallow);
    REQUIRE (deep > shallow + 0.05f);   // deeper reduction releases more slowly
}

TEST_CASE ("BrickwallLimiter: lookahead is adjustable and holds the ceiling across its range",
           "[BrickwallLimiter]")
{
    const float ceiling = juce::Decibels::decibelsToGain (-1.0f);

    SECTION ("setter clamps and the reported latency tracks the lookahead")
    {
        duskstudio::BrickwallLimiter lim;
        lim.prepare (kSr, kBlock, 2.0);

        lim.setLookaheadMs (0.5f);
        REQUIRE_THAT (lim.getLookaheadMs(), WithinAbs (0.5f, 1.0e-6f));
        const int latShort = lim.getLatencySamples();

        lim.setLookaheadMs (8.0f);
        REQUIRE_THAT (lim.getLookaheadMs(), WithinAbs (8.0f, 1.0e-6f));
        // The latency DELTA must track the lookahead delta (7.5 ms at the base
        // rate), not merely move the right direction — this guards the read-offset
        // math, not just its sign.
        const int expectedDelta = (int) std::lround ((8.0 - 0.5) * 0.001 * kSr);   // 360
        REQUIRE (std::abs ((lim.getLatencySamples() - latShort) - expectedDelta) <= 1);

        lim.setLookaheadMs (100.0f);  REQUIRE (lim.getLookaheadMs() <= 10.0f);   // clamped high
        lim.setLookaheadMs (0.0f);    REQUIRE (lim.getLookaheadMs() >= 0.1f);    // clamped low
    }

    SECTION ("ceiling held at the lookahead extremes (variable read offset is correct)")
    {
        for (float la : { 0.2f, 9.5f })
        {
            duskstudio::BrickwallLimiter lim;
            lim.prepare (kSr, kBlock, 2.0);
            lim.setLookaheadMs (la);
            lim.setCeilingDb (-1.0f);
            lim.setEnabled  (true);

            std::vector<float> L (kBlock), R (kBlock), captured;
            double phase = 0.0;
            for (int b = 0; b < 14; ++b)
            {
                fillSine (L, R, phase, 1000.0, juce::Decibels::decibelsToGain (9.0f));
                lim.processInPlace (L.data(), R.data(), kBlock);
                if (b >= 6) captured.insert (captured.end(), L.begin(), L.end());
            }
            INFO ("lookahead = " << la);
            // Measure the held ceiling at 4x (true peak) — the limiter's actual
            // control target. A read-offset bug at the lookahead extreme would
            // hold the SAMPLE peak but leak an inter-sample peak a base-rate
            // check misses. Same tolerance as the dedicated true-peak test.
            REQUIRE (truePeak4x (captured) <= ceiling * 1.04f);
            REQUIRE (lim.getCurrentGrDb() < 0.0f);
        }
    }
}
