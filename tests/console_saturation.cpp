// Regression test for the channel-strip console saturation.
//
// The channel EQ (the DAF 4k-eq-2 FourKEQDSP core) runs an always-on console drive
// (ChannelStrip kConsoleSaturationDrive) so every strip carries the subtle
// large-format-console harmonic floor. This test drives a clean on-bin sine
// through the real processor and measures the H2/H3 levels so the hard-coded
// drive remains anchored to the DAF voicing (Brown/E has the hotter H2 while
// Black/G has the stronger odd-harmonic component). It also guards that the
// character stays subtle.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <dsp/FourKEQDSP.hpp>
#include "foundation/Fft.h"

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

float gainToDb (float gain)
{
    return gain > 1.0e-10f ? 20.0f * std::log10 (gain) : -200.0f;
}

// Drive a continuous on-bin sine through a flat FourKEQDSP at the given
// saturation drive and console mode, then FFT the settled block and return the
// H2 / H3 levels relative to the fundamental, in dB.
Harmonics measure (float drive, bool blackMode, int h1Bin = kH1Bin)
{
    duskaudio::FourKEQDSP eq;
    eq.setEqType (blackMode ? 1 : 0);
    eq.setSaturation (drive);
    eq.setOversampling (0);
    eq.setAutoGain (false);
    eq.prepare (kSr, kN);
    eq.reset();

    const double freq = (double) h1Bin * kSr / (double) kN;   // exact bin
    const float  amp  = std::pow (10.0f, -18.0f * 0.05f); // nominal -18 dBFS
    const double w    = 6.28318530717958647692 * freq / kSr;

    // Warm up the emphasis / DC-blocker / ADAA state, keeping sine phase
    // continuous into the measured block.
    std::vector<float> buf ((size_t) kN, 0.0f);
    const float* input[1] = { buf.data() };
    float* output[1] = { buf.data() };
    long n = 0;
    for (int warm = 0; warm < 4; ++warm)
    {
        for (int i = 0; i < kN; ++i) buf[(size_t) i] = amp * (float) std::sin (w * (double) n++);
        eq.processBlock (input, output, 1, kN);
    }
    for (int i = 0; i < kN; ++i) buf[(size_t) i] = amp * (float) std::sin (w * (double) n++);
    eq.processBlock (input, output, 1, kN);

    // Real-only FFT magnitude. Rectangular window is fine: the sine sits exactly
    // on bin kH1Bin (integer cycles in kN samples) so there is no spectral leak.
    std::vector<float> fftData ((size_t) (2 * kN), 0.0f);
    for (int i = 0; i < kN; ++i) fftData[(size_t) i] = buf[(size_t) i];
    dusk::audio::Fft fft (kOrder);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float h1 = fftData[(size_t) h1Bin];
    const float h2 = fftData[(size_t) (2 * h1Bin)];
    const float h3 = fftData[(size_t) (3 * h1Bin)];

    Harmonics out;
    out.h1Db    = gainToDb (h1);
    out.h2RelDb = gainToDb (h2 / std::max (1.0e-12f, h1));
    out.h3RelDb = gainToDb (h3 / std::max (1.0e-12f, h1));
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

    SECTION ("Brown/E keeps the shipped H2 floor")
    {
        REQUIRE_THAT (e.h2RelDb, WithinAbs (-67.2f, 1.0f));
        REQUIRE (e.h2RelDb < -60.0f);
    }

    SECTION ("Black/G keeps the contrasting DAF harmonic signature")
    {
        REQUIRE_THAT (g.h2RelDb, WithinAbs (-72.4f, 1.0f));
        REQUIRE (e.h2RelDb > g.h2RelDb + 4.0f);
        REQUIRE (g.h3RelDb > e.h3RelDb + 12.0f);
    }
}

TEST_CASE ("4k-eq-2 retains native console colour below the user drive", "[console][saturation]")
{
    // The DAF core deliberately models native, mode-dependent residue at 0%;
    // the user control adds colour above that measured baseline.
    constexpr int kLfBin = 27; // ~79 Hz, exactly on a bin
    const Harmonics eNative = measure (0.0f, false, kLfBin);
    const Harmonics gNative = measure (0.0f, true,  kLfBin);
    const Harmonics eDriven = measure (60.0f, false, kLfBin);
    const Harmonics gDriven = measure (60.0f, true,  kLfBin);

    REQUIRE (eNative.h2RelDb > -90.0f);
    REQUIRE (gNative.h3RelDb > -100.0f);
    REQUIRE (eDriven.h2RelDb > eNative.h2RelDb + 12.0f);
    REQUIRE (gDriven.h3RelDb > gNative.h3RelDb + 8.0f);
    REQUIRE (eDriven.h2RelDb > gDriven.h2RelDb + 8.0f);
    REQUIRE (gDriven.h3RelDb > eDriven.h3RelDb + 6.0f);
}
