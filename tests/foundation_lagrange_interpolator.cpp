#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include "foundation/LagrangeInterpolator.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
std::vector<float> makeSource (int numSamples)
{
    std::vector<float> source ((size_t) numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        const double t = (double) i;
        source[(size_t) i] = (float) (0.7 * std::sin (0.031 * t)
                                      + 0.2 * std::sin (0.47 * t + 1.1)
                                      + 0.05 * std::sin (1.9 * t));
    }
    return source;
}
} // namespace

TEST_CASE ("dusk::audio::LagrangeInterpolator tracks the JUCE resampler across blocks",
           "[foundation][audio]")
{
    constexpr int kBlock = 128;
    constexpr int kBlocks = 12;

    for (const double ratio : { 0.5, 0.75, 1.0009765625, 44100.0 / 48000.0, 48000.0 / 44100.0,
                                96000.0 / 48000.0, 2.5 })
    {
        const int needed = (int) std::ceil ((double) (kBlock * kBlocks) * ratio) + 16;
        const auto source = makeSource (needed);

        dusk::audio::LagrangeInterpolator dut;
        juce::LagrangeInterpolator        reference;

        std::vector<float> dutOut ((size_t) kBlock);
        std::vector<float> refOut ((size_t) kBlock);
        int dutRead = 0, refRead = 0;

        for (int block = 0; block < kBlocks; ++block)
        {
            const int dutUsed = dut.process (ratio, source.data() + dutRead,
                                             dutOut.data(), kBlock);
            const int refUsed = reference.process (ratio, source.data() + refRead,
                                                  refOut.data(), kBlock);

            REQUIRE (dutUsed == refUsed);
            dutRead += dutUsed;
            refRead += refUsed;

            for (int i = 0; i < kBlock; ++i)
                REQUIRE_THAT (dutOut[(size_t) i], WithinAbs (refOut[(size_t) i], 2.0e-6));
        }
    }
}

TEST_CASE ("dusk::audio::LagrangeInterpolator reset clears history like the JUCE resampler",
           "[foundation][audio]")
{
    constexpr int kBlock = 64;
    const auto source = makeSource (512);

    dusk::audio::LagrangeInterpolator dut;
    juce::LagrangeInterpolator        reference;

    std::vector<float> dutOut ((size_t) kBlock);
    std::vector<float> refOut ((size_t) kBlock);

    dut.process (0.8, source.data(), dutOut.data(), kBlock);
    reference.process (0.8, source.data(), refOut.data(), kBlock);

    dut.reset();
    reference.reset();

    const int dutUsed = dut.process (0.8, source.data() + 256, dutOut.data(), kBlock);
    const int refUsed = reference.process (0.8, source.data() + 256, refOut.data(), kBlock);

    REQUIRE (dutUsed == refUsed);
    for (int i = 0; i < kBlock; ++i)
        REQUIRE_THAT (dutOut[(size_t) i], WithinAbs (refOut[(size_t) i], 2.0e-6));
}

TEST_CASE ("dusk::audio::LagrangeInterpolator is silent for silent input", "[foundation][audio]")
{
    constexpr int kBlock = 96;
    const std::vector<float> silence (512, 0.0f);
    std::vector<float> out ((size_t) kBlock, 1.0f);

    dusk::audio::LagrangeInterpolator dut;
    int read = 0;
    for (int block = 0; block < 4; ++block)
    {
        read += dut.process (1.3, silence.data() + read, out.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            REQUIRE_THAT (out[(size_t) i], WithinAbs (0.0f, 0.0f));
    }
}

TEST_CASE ("dusk::audio::LagrangeInterpolator passes a unity ratio through with 2-sample latency",
           "[foundation][audio]")
{
    constexpr int kCount = 64;
    const auto source = makeSource (kCount);
    std::vector<float> out ((size_t) kCount);

    dusk::audio::LagrangeInterpolator dut;
    const int used = dut.process (1.0, source.data(), out.data(), kCount);
    REQUIRE (used == kCount);

    for (int i = 2; i < kCount; ++i)
        REQUIRE_THAT (out[(size_t) i], WithinAbs (source[(size_t) (i - 2)], 1.0e-6));
}
