// Proves the bus compressor is a TRUE stereo-linked SSL bus: one shared
// sidechain drives both VCAs, so a loud L pulls down a quiet R (and vice
// versa). On the old dual-mono code each channel compressed independently, so
// a quiet R below threshold was untouched no matter how loud L was — this test
// fails on that behaviour.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "comp_helpers.h"

using namespace duskstudio::comp_test;

namespace
{
// Run the bus comp over a stereo 200 Hz sine with independent L/R amplitudes
// and return the settled R-channel output RMS (dB). internalOs picks the
// oversampled path (standalone default) vs the native-rate path (Dusk).
float busRightOutRmsDb (float ampL, float ampR, bool internalOs, float linkPct = 100.0f)
{
    auto c = makeComp (48000.0, 256);
    setChoice (*c, "mode", (int) CompressorMode::Bus);
    setParam  (*c, "stereo_link", linkPct);
    setChoice (*c, "auto_makeup", 0);
    setParam  (*c, "bus_threshold", -25.0f);
    setChoice (*c, "bus_ratio", 2);          // 10:1
    setChoice (*c, "bus_attack", 0);         // fastest (0.1 ms)
    setChoice (*c, "bus_release", 0);        // 100 ms
    setParam  (*c, "bus_makeup", 0.0f);
    c->setInternalOversamplingEnabled (internalOs);

    const int total = 48000;   // 1 s — well past attack/release settling
    const double w = 2.0 * juce::MathConstants<double>::pi * 200.0 / 48000.0;
    double phase = 0.0;

    juce::AudioBuffer<float> work (2, total);
    runBlocks (*c, work, total, 256,
        [&] (int, juce::AudioBuffer<float>& blk)
        {
            const int n = blk.getNumSamples();
            auto* L = blk.getWritePointer (0);
            auto* R = blk.getWritePointer (1);
            double p = phase;
            for (int i = 0; i < n; ++i)
            {
                const float s = (float) std::sin (p);
                L[i] = ampL * s;
                R[i] = ampR * s;
                p += w;
            }
            phase += w * (double) n;
        });

    // Settled tail, R channel.
    return rmsDb (work, 1, total / 2, total / 2);
}
} // namespace

TEST_CASE ("Bus comp stereo-links L/R: a loud L pulls down a quiet R", "[compressor][bus][stereo]")
{
    // R sits at -26 dBFS, just below the -25 dB threshold, so on its own it
    // barely compresses. With a loud L (-6 dBFS, ~19 dB over threshold at 10:1),
    // a stereo-linked bus drags R down by L's gain reduction.
    SECTION ("native-rate path (Dusk Studio)")
    {
        const float rWithLoudL = busRightOutRmsDb (0.5f, 0.05f, /*internalOs*/ false);
        const float rAlone     = busRightOutRmsDb (0.0f, 0.05f, /*internalOs*/ false);
        INFO ("R with loud L = " << rWithLoudL << " dB,  R alone = " << rAlone << " dB");
        REQUIRE (rWithLoudL < rAlone - 4.0f);   // dual-mono would give rWithLoudL ~= rAlone
    }

    SECTION ("oversampled path (standalone plugin)")
    {
        const float rWithLoudL = busRightOutRmsDb (0.5f, 0.05f, /*internalOs*/ true);
        const float rAlone     = busRightOutRmsDb (0.0f, 0.05f, /*internalOs*/ true);
        INFO ("R with loud L = " << rWithLoudL << " dB,  R alone = " << rAlone << " dB");
        REQUIRE (rWithLoudL < rAlone - 4.0f);
    }
}

TEST_CASE ("Bus comp stereo-link amount is continuous, not binary", "[compressor][bus][stereo]")
{
    // The link knob must scale the amount a loud L pulls down a quiet R — more
    // link → more pull. A regression to binary (any amount > 0 = full link)
    // would make r50 == r100.
    // Same loud L throughout; only the link amount varies. 0% = dual-mono
    // (R ignores L), 50% = partial, 100% = full link.
    const float rDualMono = busRightOutRmsDb (0.5f, 0.05f, false, 0.0f);
    const float r50       = busRightOutRmsDb (0.5f, 0.05f, false, 50.0f);
    const float r100      = busRightOutRmsDb (0.5f, 0.05f, false, 100.0f);
    INFO ("R dual-mono=" << rDualMono << "  r50=" << r50 << "  r100=" << r100);
    REQUIRE (r100 < r50 - 1.0f);        // 100% links harder than 50%
    REQUIRE (r50  < rDualMono - 1.0f);  // 50% links more than dual-mono
}
