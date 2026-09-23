// Calibration + regression test for the channel-strip console saturation.
//
// The channel EQ (FourKEQDSP, the 4K EQ 2 core) runs an always-on console drive
// (ChannelStrip kConsoleSaturationDrive) so every strip carries the subtle
// large-format-console harmonic floor. This test drives a clean on-bin sine
// through the core at the rate the strips run it and measures the H2/H3 levels,
// so a donor revision that moves the console character fails here rather than
// passing unheard. The expectations were measured from the core at the pinned
// donor revision.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <FourKEQDSP.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSr     = 48000.0;
constexpr int    kN      = 16384;
constexpr int    kBlock  = 512;
constexpr int    kH1Bin  = 341;   // ~999 Hz, exactly on a bin (no leakage)
constexpr double kTwoPi  = 6.28318530717958647692;

// ChannelStrip::kConsoleSaturationDrive
constexpr float kShipDrive = 22.0f;

struct Harmonics { float h2RelDb, h3RelDb; };

double binMagnitude (const std::vector<float>& x, int bin)
{
    double re = 0.0, im = 0.0;
    const double w = kTwoPi * (double) bin / (double) kN;
    for (int i = 0; i < kN; ++i)
    {
        re += (double) x[(size_t) i] * std::cos (w * (double) i);
        im -= (double) x[(size_t) i] * std::sin (w * (double) i);
    }
    return std::sqrt (re * re + im * im);
}

float relDb (double h, double h1)
{
    return (float) (20.0 * std::log10 (std::max (1.0e-12, h) / std::max (1.0e-12, h1)));
}

// A flat strip (every band at 0 dB, filters out) at the given drive and
// console mode, fed a continuous on-bin sine at the nominal -18 dBFS. The
// rectangular window is exact because the sine completes whole cycles in kN.
Harmonics measure (float drive, bool black)
{
    duskaudio::FourKEQDSP eq;
    eq.setOversampling (0);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.setEqType (black ? 1 : 0);
    eq.setHpfEnabled (false);
    eq.setLpfEnabled (false);
    eq.setLfGain (0.0f);
    eq.setLmGain (0.0f);
    eq.setHmGain (0.0f);
    eq.setHfGain (0.0f);
    eq.setInputGainDb (0.0f);
    eq.setOutputGainDb (0.0f);
    eq.setSaturation (drive);
    eq.prepare (kSr, kBlock);
    eq.reset();

    const double amp = std::pow (10.0, -18.0 / 20.0);
    const double w   = kTwoPi * (double) kH1Bin / (double) kN;

    // Four warm-up passes settle the emphasis, DC blocker and ADAA state; the
    // sine's phase runs on into the measured fifth pass.
    std::vector<float> buf ((size_t) kN);
    long n = 0;
    for (int pass = 0; pass < 5; ++pass)
    {
        for (int i = 0; i < kN; ++i)
            buf[(size_t) i] = (float) (amp * std::sin (w * (double) n++));
        for (int off = 0; off < kN; off += kBlock)
        {
            float* ch[1] = { buf.data() + off };
            eq.processBlock (ch, ch, 1, kBlock);
        }
    }

    const double h1 = binMagnitude (buf, kH1Bin);
    return { relDb (binMagnitude (buf, 2 * kH1Bin), h1),
             relDb (binMagnitude (buf, 3 * kH1Bin), h1) };
}
} // namespace

TEST_CASE ("Channel console saturation sits at the calibrated harmonic floor", "[console][saturation]")
{
    const Harmonics e = measure (kShipDrive, /*black (G)=*/false);
    const Harmonics g = measure (kShipDrive, /*black (G)=*/true);

    WARN ("E-series  H2=" << e.h2RelDb << " dB  H3=" << e.h3RelDb << " dB");
    WARN ("G-series  H2=" << g.h2RelDb << " dB  H3=" << g.h3RelDb << " dB");

    SECTION ("E-series: about 0.044 % THD, almost all of it H2")
    {
        REQUIRE_THAT (e.h2RelDb, WithinAbs (-67.2f, 2.0f));
        REQUIRE (e.h3RelDb < -95.0f);
    }

    SECTION ("G-series: about 0.025 % THD, H2 with an odd-order H3 under it")
    {
        REQUIRE_THAT (g.h2RelDb, WithinAbs (-72.4f, 2.0f));
        REQUIRE_THAT (g.h3RelDb, WithinAbs (-84.4f, 2.0f));
    }

    SECTION ("the colour stays subtle")
    {
        REQUIRE (e.h2RelDb < -60.0f);
        REQUIRE (g.h2RelDb < -60.0f);
    }
}

TEST_CASE ("E and G console characters differ at the shipped drive", "[console][saturation]")
{
    const Harmonics e = measure (kShipDrive, /*black (G)=*/false);
    const Harmonics g = measure (kShipDrive, /*black (G)=*/true);

    // E is the grittier, even-order unit; G is cleaner in H2 and carries the
    // odd-order content.
    REQUIRE (e.h2RelDb > g.h2RelDb + 3.0f);
    REQUIRE (g.h3RelDb > e.h3RelDb + 10.0f);

    // The H2 gap is the drive's doing: with the drive at zero both modes sit on
    // the same native residue, and the gap opens only once the drive engages.
    const Harmonics eOff = measure (0.0f, false);
    const Harmonics gOff = measure (0.0f, true);
    const float offGap = std::abs (eOff.h2RelDb - gOff.h2RelDb);
    REQUIRE (offGap < 1.0f);
    REQUIRE (e.h2RelDb - g.h2RelDb > offGap + 3.0f);
}
