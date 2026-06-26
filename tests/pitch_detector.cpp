#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/PitchDetector.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinRel;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Push `blocks` blocks of a continuous, harmonic-rich tone (phase carried across
// blocks). A guitar-like spectrum (fundamental + decaying harmonics) is what
// this detector is tuned for: it gives the YIN CMNDF a sharp dip at the true
// period. A PURE sine is YIN's worst case (broad dip -> the threshold break
// fires early and reads sharp), so it's deliberately not used here.
float detectTone (double f0, double sr, float amp, int blocks, int blockSize = 512)
{
    PitchDetector d;
    d.prepare (sr);   // default 2048-sample history, as the engine uses
    std::vector<float> buf ((size_t) blockSize);
    constexpr double harm[4] = { 1.0, 0.5, 0.33, 0.25 };
    double norm = 0.0; for (double h : harm) norm += h;
    long n = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < blockSize; ++i, ++n)
        {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += harm[k] * std::sin (2.0 * kPi * f0 * (double) (k + 1) * (double) n / sr);
            buf[(size_t) i] = (float) (amp * s / norm);
        }
        d.pushBlock (buf.data(), blockSize);
    }
    return d.getLatestHz();
}
} // namespace

// YIN/CMNDF monophonic detector. History must fill (>=2048 samples) before the
// estimate is meaningful, so every case drives several blocks first.
TEST_CASE ("PitchDetector: recovers a harmonic tone within a few cents", "[pitch]")
{
    const double sr = 48000.0;
    // 16 * 512 = 8192 samples >> 2048-sample history. 1% covers low-frequency
    // tau quantization; the harmonic dip + parabolic interp lands far tighter.
    REQUIRE_THAT (detectTone (110.0, sr, 0.5f, 16), WithinRel (110.0f, 0.01f));
    REQUIRE_THAT (detectTone (220.0, sr, 0.5f, 16), WithinRel (220.0f, 0.01f));
    REQUIRE_THAT (detectTone (440.0, sr, 0.5f, 16), WithinRel (440.0f, 0.01f));
    REQUIRE_THAT (detectTone (880.0, sr, 0.5f, 16), WithinRel (880.0f, 0.01f));
}

TEST_CASE ("PitchDetector: silence reports 0 Hz", "[pitch]")
{
    PitchDetector d;
    d.prepare (48000.0);
    std::vector<float> zeros (512, 0.0f);
    for (int b = 0; b < 8; ++b)
        d.pushBlock (zeros.data(), 512);

    REQUIRE (d.getLatestHz() == 0.0f);
    REQUIRE (d.getLatestLevel() < PitchDetector::kSilenceThreshold);
}

TEST_CASE ("PitchDetector: sub-threshold tone gates to 0 Hz", "[pitch]")
{
    // 0.001 amplitude -> RMS well below the 5e-3 silence gate.
    REQUIRE (detectTone (440.0, 48000.0, 0.001f, 16) == 0.0f);
}
