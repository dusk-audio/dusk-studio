#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include "foundation/WindowedSincInterpolator.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

// The generated kernel is bit-identical to the table JUCE ships wherever the
// two agree on sin/cos to the last double bit - on glibc every assertion below
// holds at a tolerance of exactly zero. The 1e-6 the tests actually use is
// headroom for a platform libm that rounds a handful of table entries the
// other way, not slack for algorithmic drift.

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

TEST_CASE ("dusk::audio::WindowedSincInterpolator tracks the JUCE resampler across blocks",
           "[foundation][audio]")
{
    constexpr int kBlock = 128;
    constexpr int kBlocks = 12;

    // The ratios a file import actually asks for (source rate / session rate),
    // plus two that never land on a repeating fraction.
    for (const double ratio : { 44100.0 / 48000.0, 48000.0 / 44100.0, 96000.0 / 48000.0,
                                48000.0 / 96000.0, 88200.0 / 48000.0, 1.0,
                                1.0009765625, 1.2345678 })
    {
        const int needed = (int) std::ceil ((double) (kBlock * kBlocks) * ratio) + 16;
        const auto source = makeSource (needed);

        dusk::audio::WindowedSincInterpolator dut;
        juce::WindowedSincInterpolator        reference;

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
                REQUIRE_THAT (dutOut[(size_t) i], WithinAbs (refOut[(size_t) i], 1.0e-6));
        }
    }
}

TEST_CASE ("dusk::audio::WindowedSincInterpolator reset clears history like the JUCE resampler",
           "[foundation][audio]")
{
    constexpr int kBlock = 256;
    const auto source = makeSource (2048);

    dusk::audio::WindowedSincInterpolator dut;
    juce::WindowedSincInterpolator        reference;

    std::vector<float> dutOut ((size_t) kBlock);
    std::vector<float> refOut ((size_t) kBlock);

    dut.process (0.8, source.data(), dutOut.data(), kBlock);
    reference.process (0.8, source.data(), refOut.data(), kBlock);

    dut.reset();
    reference.reset();

    const int dutUsed = dut.process (0.8, source.data() + 1024, dutOut.data(), kBlock);
    const int refUsed = reference.process (0.8, source.data() + 1024, refOut.data(), kBlock);

    REQUIRE (dutUsed == refUsed);
    for (int i = 0; i < kBlock; ++i)
        REQUIRE_THAT (dutOut[(size_t) i], WithinAbs (refOut[(size_t) i], 1.0e-6));
}

TEST_CASE ("dusk::audio::WindowedSincInterpolator is silent for silent input",
           "[foundation][audio]")
{
    constexpr int kBlock = 96;
    const std::vector<float> silence (1024, 0.0f);
    std::vector<float> out ((size_t) kBlock, 1.0f);

    dusk::audio::WindowedSincInterpolator dut;
    int read = 0;
    for (int block = 0; block < 4; ++block)
    {
        read += dut.process (1.3, silence.data() + read, out.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            REQUIRE_THAT (out[(size_t) i], WithinAbs (0.0f, 0.0f));
    }
}

TEST_CASE ("dusk::audio::WindowedSincInterpolator delays by its base latency",
           "[foundation][audio]")
{
    REQUIRE_THAT (dusk::audio::WindowedSincInterpolator::getBaseLatency(),
                  WithinAbs (juce::WindowedSincInterpolator::getBaseLatency(), 0.0));

    constexpr int kCount = 512;
    const auto latency = (int) dusk::audio::WindowedSincInterpolator::getBaseLatency();

    std::vector<float> impulse ((size_t) kCount, 0.0f);
    impulse[0] = 1.0f;
    std::vector<float> out ((size_t) kCount);

    dusk::audio::WindowedSincInterpolator dut;
    REQUIRE (dut.process (1.0, impulse.data(), out.data(), kCount) == kCount);

    REQUIRE_THAT (out[(size_t) latency], WithinAbs (1.0f, 1.0e-6));
    for (int i = 0; i < kCount; ++i)
        if (i != latency)
            REQUIRE (std::abs (out[(size_t) i]) < 0.02f);
}
