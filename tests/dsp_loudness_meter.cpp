#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/LoudnessMeter.h"

#include <dsp/DuskFilters.hpp>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
// Feeds `n` samples through a juce IIR filter and a donor Biquad carrying the
// same coefficients, and asserts they agree sample-for-sample.
void requireFilterParity (juce::dsp::IIR::Coefficients<float>::Ptr jc,
                          const duskaudio::BiquadCoeffs& dc, unsigned seed)
{
    juce::dsp::IIR::Filter<float> j;
    j.coefficients = jc;
    j.reset();

    duskaudio::Biquad d;
    d.setCoeffs (dc);

    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    for (int i = 0; i < 4096; ++i)
    {
        const float x = dist (rng);
        REQUIRE_THAT (d.process (x), WithinAbs (j.processSample (x), 1.0e-6f));
    }
}
} // namespace

// K-weighting is the measurement front end — it must match the JUCE-designed
// BS.1770 filters it replaces, or every LUFS reading drifts. The donor shelf /
// highPass designers claim juce::dsp::IIR parity; verify it at the exact
// K-weight coefficients across sample rates.
TEST_CASE ("LoudnessMeter K-weighting matches the JUCE-designed filters", "[dsp][loudness]")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        // Stage 1: high-shelf +4 dB @ 1681 Hz, Q = 1/sqrt2.
        const auto js1 = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
            sr, 1681.0, 1.0 / std::sqrt (2.0), juce::Decibels::decibelsToGain (4.0f));
        const auto ds1 = duskaudio::Biquad::shelf (sr, 1681.0f, 4.0f,
                                                   (float) (1.0 / std::sqrt (2.0)), true);
        requireFilterParity (js1, ds1, 0xA1);

        // Stage 2: high-pass @ 38 Hz, Q = 0.5.
        const auto js2 = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 38.0, 0.5);
        const auto ds2 = duskaudio::Biquad::highPass (sr, 38.0f, 0.5f);
        requireFilterParity (js2, ds2, 0xB2);
    }
}

TEST_CASE ("LoudnessMeter true peak tracks a hot signal", "[dsp][loudness]")
{
    constexpr double sr    = 48000.0;
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    constexpr int    N     = 4096;
    duskstudio::LoudnessMeter m;
    m.prepare (sr, N);

    // Full-scale 1 kHz sine: true peak sits at/above 0 dBFS (inter-sample peaks
    // of a near-Nyquist-safe tone are ~0 dB); silence reads the -100 floor.
    std::vector<float> L (N), R (N);
    for (int i = 0; i < N; ++i)
        L[(size_t) i] = R[(size_t) i] = std::sin (kTwoPi * 1000.0 * i / sr);
    for (int blk = 0; blk < 4; ++blk)
        m.process (L.data(), R.data(), N);

    REQUIRE (m.getTruePeakDb() > -1.0f);
    REQUIRE (m.getTruePeakDb() < 1.5f);
    REQUIRE (m.getMomentaryLufs() > -30.0f);   // a hot tone is loud
}

TEST_CASE ("LoudnessMeter requestReset zeroes readings and drops the history", "[dsp][loudness]")
{
    constexpr double sr     = 48000.0;
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    constexpr int    kSecond = 48000;
    duskstudio::LoudnessMeter m;
    m.prepare (sr, 512);

    std::vector<float> L ((size_t) kSecond), R ((size_t) kSecond);
    for (int i = 0; i < kSecond; ++i)
        L[(size_t) i] = R[(size_t) i] = 0.5f * (float) std::sin (kTwoPi * 997.0 * i / sr);
    for (int off = 0; off < kSecond; off += 512)
        m.process (L.data() + off, R.data() + off, std::min (512, kSecond - off));
    const float loud = m.getIntegratedLufs();
    REQUIRE (loud > -40.0f);

    // Published readings zero immediately, before any further processing.
    m.requestReset();
    REQUIRE_THAT (m.getIntegratedLufs(), WithinAbs (-100.0f, 1e-6f));
    REQUIRE_THAT (m.getTruePeakDb(),     WithinAbs (-100.0f, 1e-6f));

    // Quiet program after the reset: integrated must reflect only the new
    // material (fresh block history), not an average with pre-reset blocks.
    for (auto& v : L) v *= 0.01f;
    for (auto& v : R) v *= 0.01f;
    for (int off = 0; off < kSecond; off += 512)
        m.process (L.data() + off, R.data() + off, std::min (512, kSecond - off));
    REQUIRE (m.getIntegratedLufs() < loud - 20.0f);
}
