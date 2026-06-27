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
Harmonics measure (float drive, bool blackMode, int h1Bin = kH1Bin)
{
    BritishEQProcessor eq;
    eq.prepare (kSr, kN, 1);

    BritishEQProcessor::Parameters p {};   // all bands flat, HPF/LPF off
    p.isBlackMode = blackMode;
    p.saturation  = drive;
    eq.setParameters (p);

    const double freq = (double) h1Bin * kSr / (double) kN;   // exact bin
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

    const float h1 = fftData[(size_t) h1Bin];
    const float h2 = fftData[(size_t) (2 * h1Bin)];
    const float h3 = fftData[(size_t) (3 * h1Bin)];

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

TEST_CASE ("E-series transformer core adds LF-weighted saturation", "[console][transformer]")
{
    // The E-series transformer's iron saturates where flux is highest (LF), so
    // it adds odd-harmonic (H3) content that is weighted toward the low end —
    // the console "weight". G-series has no transformer, only broadband console
    // saturation. Isolate the effect by comparing how much MORE LF harmonics
    // than HF each mode produces: the E-series must be more LF-weighted than G.
    constexpr int   kLfBin = 27;     // ~79 Hz, exactly on a bin (H3 at bin 81)
    constexpr float drive  = 60.0f;  // moderate saturation so the core engages

    const Harmonics eLf = measure (drive, /*G=*/false, kLfBin);
    const Harmonics eHf = measure (drive, /*G=*/false);          // 999 Hz default
    const Harmonics gLf = measure (drive, /*G=*/true,  kLfBin);
    const Harmonics gHf = measure (drive, /*G=*/true);

    const float eLfWeight = eLf.h3RelDb - eHf.h3RelDb;   // LF harmonics minus HF
    const float gLfWeight = gLf.h3RelDb - gHf.h3RelDb;
    WARN ("LF-weighting  E=" << eLfWeight << " dB  G=" << gLfWeight << " dB");
    REQUIRE (eLfWeight > gLfWeight + 3.0f);              // E's transformer adds LF weight

    // Isolate the transformer: the E-vs-G LF gap must be SATURATION-GATED, not an
    // always-on structural difference (e.g. the phase all-pass). With saturation
    // off neither mode has a nonlinearity, so their LF harmonics coincide; the
    // gap only opens up once saturation engages.
    const Harmonics eOff = measure (0.0f, /*G=*/false, kLfBin);
    const Harmonics gOff = measure (0.0f, /*G=*/true,  kLfBin);
    const float onGap  = eLf.h3RelDb  - gLf.h3RelDb;            // E above G, saturation on
    const float offGap = std::abs (eOff.h3RelDb - gOff.h3RelDb);
    REQUIRE (onGap > offGap + 6.0f);

    // The effect must also be present at the shipped channel drive (not only at
    // the over-driven probe level), or the "console weight" is inaudible in use.
    constexpr float shipDrive = 22.0f;   // ChannelStrip::kConsoleSaturationDrive
    const float eShipWeight = measure (shipDrive, false, kLfBin).h3RelDb - measure (shipDrive, false).h3RelDb;
    const float gShipWeight = measure (shipDrive, true,  kLfBin).h3RelDb - measure (shipDrive, true ).h3RelDb;
    REQUIRE (eShipWeight > gShipWeight + 1.0f);
}
