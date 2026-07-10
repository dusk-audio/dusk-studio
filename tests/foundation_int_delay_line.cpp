#include <catch2/catch_test_macros.hpp>

#include "foundation/IntDelayLine.h"

#include <juce_dsp/juce_dsp.h>

#include <random>
#include <vector>

// Ground-truth A/B: dusk::audio::IntDelayLine must produce bit-identical output
// to juce::dsp::DelayLine<float, None> for every delay in the used range.
namespace
{
using JuceDelay = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>;

void primeJuce (JuceDelay& j, int maxDelay, int blockSize)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = 48000.0;
    spec.maximumBlockSize = (std::uint32_t) blockSize;
    spec.numChannels      = 1;
    j.prepare (spec);
    j.setMaximumDelayInSamples (maxDelay);
    j.reset();
}
} // namespace

TEST_CASE ("IntDelayLine matches juce::dsp::DelayLine<None>", "[foundation][dsp]")
{
    constexpr int maxDelay = 16384;

    std::mt19937 rng (0xD05C);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> input (4096);
    for (auto& s : input) s = dist (rng);

    for (int delay : { 0, 1, 2, 3, 100, 4095, 4096, 16384 })
    {
        dusk::audio::IntDelayLine d;
        d.setMaximumDelayInSamples (maxDelay);
        d.setDelay (delay);

        JuceDelay j;
        primeJuce (j, maxDelay, (int) input.size());
        j.setDelay ((float) delay);

        for (float x : input)
        {
            d.pushSample (x);
            j.pushSample (0, x);
            REQUIRE (d.popSample() == j.popSample (0));
        }
    }
}

TEST_CASE ("IntDelayLine reset clears history like juce", "[foundation][dsp]")
{
    constexpr int maxDelay = 256;
    dusk::audio::IntDelayLine d;
    JuceDelay j;
    d.setMaximumDelayInSamples (maxDelay);
    primeJuce (j, maxDelay, 64);
    d.setDelay (100);
    j.setDelay (100.0f);

    for (int i = 0; i < 50; ++i) { d.pushSample (1.0f); j.pushSample (0, 1.0f); d.popSample(); j.popSample (0); }
    d.reset();
    j.reset();

    for (int i = 0; i < maxDelay; ++i)
    {
        d.pushSample (0.0f);
        j.pushSample (0, 0.0f);
        REQUIRE (d.popSample() == j.popSample (0));
    }
}
