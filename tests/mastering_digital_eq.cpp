// Unit tests for MasteringDigitalEq — verifies that the shared coefficient
// math (computeCoeffs / magnitudeDb) is what the audio path actually applies,
// so the UI curve and the DSP can never disagree, and that the allocation-free
// in-place rebuild produces the expected RBJ shelf/bell response.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/MasteringDigitalEq.h"

#include <cmath>
#include <vector>

using duskstudio::MasteringDigitalEq;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr = 48000.0;

// RMS of a buffer's tail (after the filter has reached steady state).
float tailRms (const std::vector<float>& x, int fromIndex)
{
    double acc = 0.0;
    int n = 0;
    for (int i = fromIndex; i < (int) x.size(); ++i)
    {
        const double v = (double) x[(size_t) i];
        acc += v * v; ++n;
    }
    return n > 0 ? (float) std::sqrt (acc / n) : 0.0f;
}
} // namespace

TEST_CASE ("MasteringDigitalEq magnitudeDb matches expected RBJ response", "[mastering][eq]")
{
    SECTION ("peak band hits its gain at centre, unity far away")
    {
        // Band 2 is a peaking bell. +6 dB at fc.
        const float atCentre = MasteringDigitalEq::magnitudeDb (2, kSr, 1000.0f, 1.0f, 6.0f, 1000.0);
        REQUIRE_THAT (atCentre, WithinAbs (6.0f, 0.15f));

        const float wayBelow = MasteringDigitalEq::magnitudeDb (2, kSr, 1000.0f, 1.0f, 6.0f, 40.0);
        const float wayAbove = MasteringDigitalEq::magnitudeDb (2, kSr, 1000.0f, 1.0f, 6.0f, 18000.0);
        REQUIRE_THAT (wayBelow, WithinAbs (0.0f, 0.5f));
        REQUIRE_THAT (wayAbove, WithinAbs (0.0f, 0.5f));
    }

    SECTION ("unity gain is exactly flat")
    {
        for (double f : { 50.0, 500.0, 5000.0, 19000.0 })
            REQUIRE_THAT (MasteringDigitalEq::magnitudeDb (2, kSr, 1000.0f, 1.0f, 0.0f, f),
                          WithinAbs (0.0f, 1.0e-9f));
    }

    SECTION ("low shelf boosts the low end, leaves the top alone")
    {
        const float low  = MasteringDigitalEq::magnitudeDb (0, kSr, 100.0f, 0.7f, 6.0f, 30.0);
        const float high = MasteringDigitalEq::magnitudeDb (0, kSr, 100.0f, 0.7f, 6.0f, 12000.0);
        REQUIRE_THAT (low,  WithinAbs (6.0f, 0.6f));
        REQUIRE_THAT (high, WithinAbs (0.0f, 0.3f));
    }

    SECTION ("high shelf boosts the top, leaves the bottom alone")
    {
        const float low  = MasteringDigitalEq::magnitudeDb (4, kSr, 10000.0f, 0.7f, 6.0f, 100.0);
        const float high = MasteringDigitalEq::magnitudeDb (4, kSr, 10000.0f, 0.7f, 6.0f, 20000.0);
        REQUIRE_THAT (low,  WithinAbs (0.0f, 0.3f));
        REQUIRE_THAT (high, WithinAbs (6.0f, 0.8f));   // wider tol: HF shelf cramps near Nyquist
    }
}

TEST_CASE ("MasteringDigitalEq processInPlace applies the shared response", "[mastering][eq]")
{
    MasteringDigitalEq eq;
    eq.prepare (kSr, 512);
    eq.setEnabled (true);
    eq.setBandFreq   (2, 1000.0f);
    eq.setBandQ      (2, 1.0f);
    eq.setBandGainDb (2, 6.0f);

    const int total = 48000;            // 1 s
    const float w = juce::MathConstants<float>::twoPi * 1000.0f / (float) kSr;
    std::vector<float> in ((size_t) total), L ((size_t) total), R ((size_t) total);
    for (int i = 0; i < total; ++i)
    {
        in[(size_t) i] = std::sin (w * (float) i);
        L[(size_t) i]  = in[(size_t) i];
        R[(size_t) i]  = in[(size_t) i];
    }

    for (int i = 0; i < total; i += 512)
    {
        const int n = std::min (512, total - i);
        eq.processInPlace (&L[(size_t) i], &R[(size_t) i], n);
    }

    // Measure over the settled tail. |H(fc)| for +6 dB = 10^(6/20) ≈ 1.995.
    const int from = total / 2;
    const float gainL = tailRms (L, from) / tailRms (in, from);
    const float gainR = tailRms (R, from) / tailRms (in, from);
    REQUIRE_THAT (gainL, WithinAbs (1.995f, 0.05f));
    REQUIRE_THAT (gainR, WithinAbs (1.995f, 0.05f));

    // The DSP gain must agree with the magnitude the UI would plot.
    const float curveDb = MasteringDigitalEq::magnitudeDb (2, kSr, 1000.0f, 1.0f, 6.0f, 1000.0);
    const float dspDb   = juce::Decibels::gainToDecibels (gainL);
    REQUIRE_THAT (dspDb, WithinAbs (curveDb, 0.2f));
}

TEST_CASE ("MasteringDigitalEq per-block param re-push is inert", "[mastering][eq]")
{
    // The chain re-pushes every band's params on every audio block. With
    // dirty-on-change, an unchanged push must not rebuild coefficients or
    // perturb the output: an EQ whose params are set once must produce output
    // bit-identical to one re-pushed the same values every block.
    MasteringDigitalEq once, every;
    for (auto* eq : { &once, &every })
    {
        eq->prepare (kSr, 256);
        eq->setEnabled (true);
        eq->setBandFreq (1, 250.0f);
        eq->setBandQ (1, 1.0f);
        eq->setBandGainDb (1, -4.0f);
    }

    const float w = juce::MathConstants<float>::twoPi * 250.0f / (float) kSr;
    bool identical = true;
    for (int blk = 0; blk < 60; ++blk)
    {
        std::vector<float> la (256), ra (256), lb (256), rb (256);
        for (int i = 0; i < 256; ++i)
        {
            const float s = std::sin (w * (float) (blk * 256 + i));
            la[(size_t) i] = lb[(size_t) i] = s;
            ra[(size_t) i] = rb[(size_t) i] = s;
        }
        once.processInPlace (la.data(), ra.data(), 256);

        every.setBandFreq (1, 250.0f);   // same values, every block
        every.setBandQ (1, 1.0f);
        every.setBandGainDb (1, -4.0f);
        every.processInPlace (lb.data(), rb.data(), 256);

        for (int i = 0; i < 256; ++i)
            if (! juce::exactlyEqual (la[(size_t) i], lb[(size_t) i])) { identical = false; break; }
        if (! identical) break;
    }
    REQUIRE (identical);
}
