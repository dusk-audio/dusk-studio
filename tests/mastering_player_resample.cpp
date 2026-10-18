#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/MasteringPlayer.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace duskstudio;

// Rewinding a tail-resident reader and rendering faster than its worker must
// preserve sample content across windows, including through resampling.
TEST_CASE ("MasteringPlayer resamples a source whose rate differs from the device",
           "[mastering][resample]")
{
    constexpr double kSourceRate = 44100.0;
    double deviceRate = 48000.0;
    SECTION ("Matching source and device rates") { deviceRate = kSourceRate; }
    SECTION ("Resampling at 48 kHz") {}
    SECTION ("Resampling at 96 kHz") { deviceRate = 96000.0; }
    constexpr int    kSourceLen  = 176400;
    constexpr int    kBlock      = 512;

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-mastering-resample-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    const auto wav = dir.getChildFile ("mix.wav");
    {
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt.createWriterFor (wav.createOutputStream().release(),
                                  kSourceRate, 2, 24, {}, 0));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> buf (2, kSourceLen);
        for (int n = 0; n < kSourceLen; ++n)
        {
            const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                              * 441.0f * (float) n / (float) kSourceRate);
            buf.setSample (0, n, v);
            buf.setSample (1, n, v);
        }
        REQUIRE (writer->writeFromAudioSampleBuffer (buf, 0, kSourceLen));
    }

    MasteringPlayer player;
    player.prepare (kBlock, deviceRate);
    REQUIRE (player.loadFile (wav));
    REQUIRE (player.getSourceSampleRate() == kSourceRate);

    player.setPlayhead (kSourceLen - 2 * kBlock);
    REQUIRE (player.primeOfflineBlock (kBlock));
    player.setPlayhead (0);
    player.play();
    std::vector<float> outL ((size_t) kBlock), outR ((size_t) kBlock);
    juce::int64 deviceSamples = 0;
    const juce::int64 hardCap = 10 * (juce::int64) deviceRate;
    const auto expected = (juce::int64) std::llround ((double) kSourceLen * deviceRate / kSourceRate);
    std::int64_t expectedStart = 0;
    std::uint64_t revision = 0;
    while (player.isPlaying() && deviceSamples < hardCap)
    {
        REQUIRE (player.primeOfflineBlock (kBlock));
        const auto block = player.process (outL.data(), outR.data(), kBlock);
        REQUIRE (block.stable);
        REQUIRE (block.playing);
        REQUIRE (block.completeSourceRead);
        REQUIRE (block.sourceStart == expectedStart);
        REQUIRE (block.sourceEnd == player.getPlayhead());
        REQUIRE (block.sourceEnd >= block.sourceStart);
        if (deviceSamples == 0) revision = block.revision;
        REQUIRE (block.revision == revision);
        expectedStart = block.sourceEnd;
        if (deviceSamples + kBlock < expected - kBlock)
        {
            for (const auto* samples : { outL.data(), outR.data() })
            {
                double energy = 0.0;
                for (int n = 0; n < kBlock; ++n) energy += samples[n] * samples[n];
                REQUIRE_THAT (energy / kBlock, Catch::Matchers::WithinAbs (0.125, 0.02));
            }
        }
        deviceSamples += kBlock;
    }

    REQUIRE (deviceSamples >= expected - 2 * kBlock);
    REQUIRE (deviceSamples <= expected + 2 * kBlock);

    // Playhead is source-domain and lands at (or immediately before) EOF.
    REQUIRE (player.getPlayhead() >= kSourceLen - 8);
    REQUIRE (player.getPlayhead() <= kSourceLen);
    player.finishOfflineRender();
}
