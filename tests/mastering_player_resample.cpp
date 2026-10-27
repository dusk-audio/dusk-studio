#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/MasteringPlayer.h"
#include "engine/audiofile/FileWriter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

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

TEST_CASE ("MasteringPlayer rejects frequencies above the destination Nyquist", "[mastering][resample][antialias]")
{
    constexpr double deviceRate = 44100.0;
    constexpr int blockSize = 257;
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("dusk-mastering-antialias-" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory().wasOk());
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };
    for (const auto rates : { std::array<double, 2> { 48000.0, 23500.0 },
                              std::array<double, 2> { 96000.0, 30000.0 } })
        for (double frequency : { 1000.0, rates[1] })
        {
            const double sourceRate = rates[0];
            const int sourceFrames = (int) sourceRate;
            CAPTURE (sourceRate, frequency);
            const auto wav = dir.getChildFile ("mix.wav");
            REQUIRE (wav.deleteFile());
            {
                auto writer = dusk::audio::FileWriter::create (
                    std::filesystem::u8path (wav.getFullPathName().toStdString()),
                    { sourceRate, 2, 32, dusk::audio::WriteSpec::Format::Wav });
                REQUIRE (writer != nullptr);
                juce::AudioBuffer<float> samples (2, sourceFrames);
                for (int frame = 0; frame < sourceFrames; ++frame)
                {
                    const float value = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi
                                                               * frequency * frame / sourceRate));
                    samples.setSample (0, frame, value);
                    samples.setSample (1, frame, -value);
                }
                REQUIRE (writer->write (samples.getArrayOfReadPointers(), 2, sourceFrames));
                REQUIRE (writer->flush());
            }
            MasteringPlayer player;
            player.prepare (blockSize, deviceRate);
            REQUIRE (player.loadFile (wav));
            player.play();
            std::array<float, blockSize> left {}, right {};
            double energy = 0.0;
            int measured = 0;
            for (int offset = 0; offset < (int) deviceRate; offset += blockSize)
            {
                const int frames = std::min (blockSize, (int) deviceRate - offset);
                REQUIRE (player.primeOfflineBlock (frames));
                const auto info = player.process (left.data(), right.data(), frames);
                REQUIRE (info.stable);
                REQUIRE (info.completeSourceRead);
                for (int frame = 0; frame < frames; ++frame)
                {
                    REQUIRE_THAT (left[(std::size_t) frame], Catch::Matchers::WithinAbs (-right[(std::size_t) frame], 2.0e-6f));
                    if (offset + frame < 1000 || offset + frame >= (int) deviceRate - 1000) continue;
                    energy += (double) left[(std::size_t) frame] * left[(std::size_t) frame];
                    ++measured;
                }
            }
            const double outputRms = std::sqrt (energy / measured);
            if (frequency < deviceRate * 0.5)
                CHECK_THAT (outputRms, Catch::Matchers::WithinAbs (0.5 / std::sqrt (2.0), 0.002));
            else
                CHECK (outputRms < 1.0e-4);
            player.finishOfflineRender();
        }
}

TEST_CASE ("MasteringPlayer centers resampling at file boundaries and preserves phase across blocks", "[mastering][resample][boundaries]")
{
    constexpr int sourceFrames = 47, maxBlock = 17;
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("dusk-mastering-boundaries-" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory().wasOk());
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };
    for (const auto rates : { std::array<double, 2> { 48000.0, 44100.0 },
                              std::array<double, 2> { 96000.0, 44100.0 },
                              std::array<double, 2> { 44100.0, 96000.0 } })
        for (int channels : { 1, 2 })
        {
            CAPTURE (rates[0], rates[1], channels);
            const auto wav = dir.getChildFile ("mix.wav");
            REQUIRE (wav.deleteFile());
            std::array<std::array<float, sourceFrames>, 2> source {};
            {
                auto writer = dusk::audio::FileWriter::create (
                    std::filesystem::u8path (wav.getFullPathName().toStdString()),
                    { rates[0], channels, 32, dusk::audio::WriteSpec::Format::Wav });
                REQUIRE (writer != nullptr);
                juce::AudioBuffer<float> samples (channels, sourceFrames);
                for (int channel = 0; channel < channels; ++channel)
                    for (int frame = 0; frame < sourceFrames; ++frame)
                    {
                        const float value = frame == 0 ? 0.5f : frame == sourceFrames - 1 ? -0.4f
                            : (float) (0.1 * std::sin (0.3 * frame + channel));
                        samples.setSample (channel, frame, value);
                    }
                REQUIRE (writer->write (samples.getArrayOfReadPointers(), channels, sourceFrames));
                REQUIRE (writer->flush());
            }
            {
                juce::WavAudioFormat format;
                std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (wav.createInputStream().release(), true));
                REQUIRE (reader != nullptr);
                float* dest[] { source[0].data(), source[1].data() };
                REQUIRE (reader->read (dest, 2, 0, sourceFrames));
                if (channels == 1) source[1] = source[0];
            }
            const double ratio = rates[0] / rates[1];
            const int total = (int) std::llround (sourceFrames / ratio);
            const auto expectedAt = [&] (int channel, double position)
            {
                constexpr double pi = 3.14159265358979323846;
                constexpr int radius = 100;
                const int center = (int) std::floor (position);
                const double fraction = position - center;
                const double cutoff = std::min (1.0, 1.0 / ratio);
                double value = 0.0, sum = 0.0;
                for (int tap = -radius; tap <= radius; ++tap)
                {
                    const double x = tap - fraction;
                    if (std::abs (x) >= radius) continue;
                    const double argument = pi * cutoff * x;
                    const double coefficient = cutoff * (std::abs (argument) < 1.0e-12 ? 1.0 : std::sin (argument) / argument)
                        * 0.5 * (1.0 + std::cos (pi * x / radius));
                    sum += coefficient;
                    if (center + tap >= 0 && center + tap < sourceFrames)
                        value += coefficient * source[(std::size_t) channel][(std::size_t) (center + tap)];
                }
                return (float) (value / sum);
            };
            MasteringPlayer player;
            player.prepare (maxBlock, rates[1]);
            REQUIRE (player.loadFile (wav));
            player.play();
            std::array<float, maxBlock> left {}, right {};
            int offset = 0, iteration = 0;
            while (offset < total)
            {
                const int sizes[] { 1, 7, 3, 17 };
                const int frames = std::min (sizes[iteration++ % 4], total - offset);
                REQUIRE (player.primeOfflineBlock (frames));
                const auto info = player.process (left.data(), right.data(), frames);
                REQUIRE (info.completeSourceRead);
                REQUIRE (info.stable);
                for (int sample = 0; sample < frames; ++sample)
                {
                    const double position = (offset + sample) * ratio;
                    CHECK_THAT (left[(std::size_t) sample], Catch::Matchers::WithinAbs (expectedAt (0, position), 2.0e-6f));
                    CHECK_THAT (right[(std::size_t) sample], Catch::Matchers::WithinAbs (expectedAt (1, position), 2.0e-6f));
                }
                offset += frames;
            }
            CHECK_FALSE (player.isPlaying());
            CHECK (player.getPlayhead() == sourceFrames);
            player.setPlayhead (5);
            player.play();
            REQUIRE (player.primeOfflineBlock (3));
            REQUIRE (player.process (left.data(), right.data(), 3).completeSourceRead);
            player.stop();
            REQUIRE_FALSE (player.process (left.data(), right.data(), 1).playing);
            CHECK_THAT (left[0], Catch::Matchers::WithinAbs (0.0f, 0.0f));
            player.play();
            REQUIRE (player.primeOfflineBlock (1));
            REQUIRE (player.process (left.data(), right.data(), 1).completeSourceRead);
            CHECK_THAT (left[0], Catch::Matchers::WithinAbs (expectedAt (0, 5.0 + 3.0 * ratio), 2.0e-6f));

            const auto samePosition = player.getPlayhead();
            player.setPlayhead (samePosition);
            REQUIRE (player.primeOfflineBlock (1));
            REQUIRE (player.process (left.data(), right.data(), 1).completeSourceRead);
            CHECK_THAT (left[0], Catch::Matchers::WithinAbs (expectedAt (0, (double) samePosition), 2.0e-6f));

            const auto preparedPosition = player.getPlayhead();
            player.prepare (maxBlock, rates[1]);
            REQUIRE (player.primeOfflineBlock (1));
            REQUIRE (player.process (left.data(), right.data(), 1).completeSourceRead);
            CHECK_THAT (left[0], Catch::Matchers::WithinAbs (expectedAt (0, (double) preparedPosition), 2.0e-6f));
            player.setPlayhead (0);
            REQUIRE (player.primeOfflineBlock (1));
            REQUIRE (player.process (left.data(), right.data(), 1).completeSourceRead);
            CHECK_THAT (left[0], Catch::Matchers::WithinAbs (expectedAt (0, 0.0), 2.0e-6f));
            player.finishOfflineRender();
        }
}

TEST_CASE ("MasteringPlayer keeps rounded half-frame durations independent of callback size", "[mastering][resample][duration]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("dusk-mastering-duration-" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory().wasOk());
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };
    for (const auto rateAndLength : { std::array<int, 2> { 48000, 80 },
                                      std::array<int, 2> { 96000, 160 } })
    {
        const auto wav = dir.getChildFile ("mix.wav");
        REQUIRE (wav.deleteFile());
        {
            auto writer = dusk::audio::FileWriter::create (
                std::filesystem::u8path (wav.getFullPathName().toStdString()),
                { (double) rateAndLength[0], 1, 32, dusk::audio::WriteSpec::Format::Wav });
            REQUIRE (writer != nullptr);
            std::vector<float> source ((std::size_t) rateAndLength[1], 0.25f);
            const float* channels[] { source.data() };
            REQUIRE (writer->write (channels, 1, rateAndLength[1]));
            REQUIRE (writer->flush());
        }
        for (const auto sizes : { std::array<int, 4> { 1, 1, 1, 1 },
                                  std::array<int, 4> { 7, 7, 7, 7 },
                                  std::array<int, 4> { 17, 17, 17, 17 },
                                  std::array<int, 4> { 1, 7, 3, 17 } })
        {
            CAPTURE (rateAndLength[0], sizes);
            MasteringPlayer player;
            player.prepare (17, 44100.0);
            REQUIRE (player.loadFile (wav));
            player.play();
            std::array<float, 17> left {}, right {};
            int offset = 0, block = 0;
            constexpr int expectedFrames = 74;
            while (offset < expectedFrames)
            {
                const int frames = std::min (sizes[(std::size_t) (block++ % 4)], expectedFrames - offset);
                REQUIRE (player.primeOfflineBlock (frames));
                const auto info = player.process (left.data(), right.data(), frames);
                CHECK (info.playing);
                CHECK (info.completeSourceRead);
                for (int sample = 0; sample < frames; ++sample)
                    CHECK (left[(std::size_t) sample] > 0.01f);
                offset += frames;
            }
            CHECK_FALSE (player.isPlaying());
            CHECK (player.getPlayhead() == rateAndLength[1]);
            player.finishOfflineRender();
        }
    }
}

TEST_CASE ("MasteringPlayer primes an accepted source span beyond the current cache lead", "[mastering][resample][prime]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("dusk-mastering-prime-" + juce::Uuid().toString());
    REQUIRE (dir.createDirectory().wasOk());
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };
    const auto wav = dir.getChildFile ("mix.wav");
    {
        auto writer = dusk::audio::FileWriter::create (
            std::filesystem::u8path (wav.getFullPathName().toStdString()),
            { 48000.0, 1, 32, dusk::audio::WriteSpec::Format::Wav });
        REQUIRE (writer != nullptr);
        std::vector<float> source (200000, 0.25f);
        const float* channels[] { source.data() };
        REQUIRE (writer->write (channels, 1, (std::int64_t) source.size()));
        REQUIRE (writer->flush());
    }
    MasteringPlayer player;
    player.prepare (50000, 44100.0);
    REQUIRE (player.loadFile (wav));
    REQUIRE (player.primeOfflineBlock (1));
    player.setPlayhead (43000);
    player.play();
    REQUIRE (player.primeOfflineBlock (50000));
    std::vector<float> left (50000), right (50000);
    const auto info = player.process (left.data(), right.data(), 50000);
    REQUIRE (info.completeSourceRead);
    for (const auto sample : { left.front(), left.back(), right.front(), right.back() })
        CHECK_THAT (sample, Catch::Matchers::WithinAbs (0.25f, 2.0e-6f));
    for (const double invalidRate : { -1.0, std::numeric_limits<double>::max(),
                                     std::numeric_limits<double>::infinity(),
                                     std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::denorm_min() })
    {
        player.prepare (17, invalidRate);
        CHECK_FALSE (player.primeOfflineBlock (1));
        player.prepare (17, 44100.0);
        REQUIRE (player.primeOfflineBlock (1));
        REQUIRE (player.process (left.data(), right.data(), 1).completeSourceRead);
    }
    player.prepare (std::numeric_limits<int>::max(), 44100.0);
    CHECK_FALSE (player.primeOfflineBlock (1));
    player.finishOfflineRender();
}
