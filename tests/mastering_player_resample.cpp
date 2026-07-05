#include <catch2/catch_test_macros.hpp>

#include "engine/MasteringPlayer.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace duskstudio;

// A 44.1 kHz source on a 48 kHz device must play for len * 48/44.1 device
// samples — the player resamples instead of clocking the file out 1:1 (which
// played it ~8.8 % fast and sharp). The playhead stays in source samples.
TEST_CASE ("MasteringPlayer resamples a source whose rate differs from the device",
           "[mastering][resample]")
{
    constexpr double kSourceRate = 44100.0;
    constexpr double kDeviceRate = 48000.0;
    constexpr int    kSourceLen  = 22050;   // 0.5 s at source rate
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
    player.prepare (kBlock, kDeviceRate);
    REQUIRE (player.loadFile (wav));
    REQUIRE (player.getSourceSampleRate() == kSourceRate);

    player.play();
    std::vector<float> outL ((size_t) kBlock), outR ((size_t) kBlock);
    juce::int64 deviceSamples = 0;
    const juce::int64 hardCap = 10 * (juce::int64) kDeviceRate;
    while (player.isPlaying() && deviceSamples < hardCap)
    {
        player.process (outL.data(), outR.data(), kBlock);
        deviceSamples += kBlock;
    }

    // Expected device-domain duration: 22050 * 48000/44100 = 24000 samples,
    // ± one block of stop-detection quantisation either side.
    const auto expected = (juce::int64) std::llround ((double) kSourceLen
                                                       * kDeviceRate / kSourceRate);
    REQUIRE (deviceSamples >= expected - 2 * kBlock);
    REQUIRE (deviceSamples <= expected + 2 * kBlock);

    // Playhead is source-domain and lands at (or immediately before) EOF.
    REQUIRE (player.getPlayhead() >= kSourceLen - 8);
    REQUIRE (player.getPlayhead() <= kSourceLen);
}
