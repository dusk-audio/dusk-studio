// Calibration + regression test for the channel-strip console saturation.
//
// The channel EQ (BritishEQProcessor) runs an always-on console drive
// (ChannelStrip kConsoleSaturationDrive) so every strip carries the subtle
// large-format-console harmonic floor. This test drives a clean on-bin sine
// through the real processor and measures the H2/H3 levels so the hard-coded
// drive can be matched to the real E/G units (E grittier / H2-dominant,
// G cleaner / lower THD). It also guards that the character stays subtle.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "BritishEQProcessor.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr   = 48000.0;
constexpr int    kOrder = 14;
constexpr int    kN    = 1 << kOrder;     // 16384
constexpr int    kH1Bin = 341;            // ~999 Hz, exactly on a bin (no leakage)

struct Harmonics { float h1Db, h2RelDb, h3RelDb; };

// Drive a continuous on-bin sine through a flat BritishEQProcessor at the given
// saturation drive and console mode, then FFT the settled block and return the
// H2 / H3 levels relative to the fundamental, in dB.
Harmonics measure (float drive, bool blackMode)
{
    BritishEQProcessor eq;
    eq.prepare (kSr, kN, 1);

    BritishEQProcessor::Parameters p {};   // all bands flat, HPF/LPF off
    p.isBlackMode = blackMode;
    p.saturation  = drive;
    eq.setParameters (p);

    const double freq = (double) kH1Bin * kSr / (double) kN;   // exact bin
    const float  amp  = juce::Decibels::decibelsToGain (-18.0f); // nominal -18 dBFS
    const double w    = juce::MathConstants<double>::twoPi * freq / kSr;

    // Warm up the emphasis / DC-blocker / ADAA state, keeping sine phase
    // continuous into the measured block.
    juce::AudioBuffer<float> buf (1, kN);
    long n = 0;
    for (int warm = 0; warm < 4; ++warm)
    {
        for (int i = 0; i < kN; ++i) buf.setSample (0, i, amp * (float) std::sin (w * (double) n++));
        eq.process (buf);
    }
    for (int i = 0; i < kN; ++i) buf.setSample (0, i, amp * (float) std::sin (w * (double) n++));
    eq.process (buf);

    // Real-only FFT magnitude. Rectangular window is fine: the sine sits exactly
    // on bin kH1Bin (integer cycles in kN samples) so there is no spectral leak.
    std::vector<float> fftData ((size_t) (2 * kN), 0.0f);
    for (int i = 0; i < kN; ++i) fftData[(size_t) i] = buf.getSample (0, i);
    juce::dsp::FFT fft (kOrder);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float h1 = fftData[(size_t) kH1Bin];
    const float h2 = fftData[(size_t) (2 * kH1Bin)];
    const float h3 = fftData[(size_t) (3 * kH1Bin)];

    Harmonics out;
    out.h1Db    = juce::Decibels::gainToDecibels (h1, -200.0f);
    out.h2RelDb = juce::Decibels::gainToDecibels (h2 / juce::jmax (1.0e-12f, h1), -200.0f);
    out.h3RelDb = juce::Decibels::gainToDecibels (h3 / juce::jmax (1.0e-12f, h1), -200.0f);
    return out;
}
} // namespace

TEST_CASE ("Channel console saturation sits at the calibrated harmonic floor", "[console][saturation]")
{
    constexpr float drive = 22.0f;   // ChannelStrip::kConsoleSaturationDrive

    const Harmonics e = measure (drive, /*blackMode (G)=*/false);
    const Harmonics g = measure (drive, /*blackMode (G)=*/true);

    // Emit the measured levels so the drive can be matched to the real units.
    WARN ("E-series  H2=" << e.h2RelDb << " dB  H3=" << e.h3RelDb << " dB");
    WARN ("G-series  H2=" << g.h2RelDb << " dB  H3=" << g.h3RelDb << " dB");

    SECTION ("E-series matches the SSL 4000E bench THD (~0.02 %, -74 dB H2)")
    {
        // drive = 22 places the E-series (gritty, H2-dominant) channel at the
        // published 4000E figure of ~0.02 % THD (H2 ≈ -74 dB) at 0 VU
        // (-18 dBFS). Tolerance absorbs the donor's dithered console noise.
        // Re-derive from the [.sweep] case if the THD target changes.
        REQUIRE_THAT (e.h2RelDb, WithinAbs (-74.0f, 4.0f));
    }

    SECTION ("G-series is the cleaner unit (~0.005 %, ~12 dB less H2)")
    {
        REQUIRE_THAT (g.h2RelDb, WithinAbs (-86.0f, 4.0f));
        REQUIRE (e.h2RelDb > g.h2RelDb);   // E grittier than G
    }
}
