#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/PitchDetector.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
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

// The scan runs on a fresh quarter of history rather than every block, so a
// small buffer feeds several blocks between scans. The reading has to survive
// that gap: clearing it would make the tuner needle flicker to "no signal"
// seven blocks out of eight at 64 samples.
TEST_CASE ("PitchDetector: holds its reading between scans", "[pitch]")
{
    constexpr double sr = 48000.0;
    // 2048-sample history at 64 per block needs 32 blocks to fill, plus a scan
    // interval on top before the first meaningful estimate lands.
    const float settled = detectTone (220.0, sr, 0.5f, 64, 64);
    REQUIRE_THAT (settled, WithinRel (220.0f, 0.01f));

    // Every block from here reports a pitch, not just the ones that rescan.
    constexpr int blockSize = 64;
    constexpr int blocksPerScan = (2048 / 4) / blockSize;
    PitchDetector d;
    d.prepare (sr);
    std::vector<float> buf (blockSize);
    long n = 0;
    float scanHz = 0.0f;
    for (int b = 0; b < 80; ++b)
    {
        for (int i = 0; i < blockSize; ++i, ++n)
            buf[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 220.0 * (double) n / sr);
        d.pushBlock (buf.data(), blockSize);

        const bool isScanBlock = b == 0 || (b + 1) % blocksPerScan == 0;
        if (isScanBlock)
        {
            scanHz = d.getLatestHz();
            if (b >= 40)
                REQUIRE_THAT (scanHz, WithinRel (220.0f, 0.01f));
        }
        else if (b >= 40)
        {
            REQUIRE_THAT (d.getLatestHz(), WithinAbs (scanHz, 1.0e-6f));
        }
    }
}

// The frame stride subsamples the difference function, so accuracy has to hold
// across the advertised range rather than only at the octaves the first case
// covers. A tuner is useful at ~1 cent; 2% here is a loose bound that still
// catches the stride landing on a wrong period.
TEST_CASE ("PitchDetector: accurate across the 50-1500 Hz range", "[pitch]")
{
    constexpr double sr = 48000.0;
    for (double f : { 50.0, 82.41, 146.83, 329.63, 659.26, 1500.0 })
    {
        DYNAMIC_SECTION ("frequency = " << f << " Hz")
        {
            REQUIRE_THAT (detectTone (f, sr, 0.5f, 16), WithinRel ((float) f, 0.02f));
        }
    }
}
