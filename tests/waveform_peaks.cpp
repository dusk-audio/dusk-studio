#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/WaveformPeaks.h"
#include "engine/audiofile/FileWriter.h"
#include "TestTempDirectory.h"

#include <sndfile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <thread>
#include <type_traits>

using namespace dusk::audio;
using Catch::Matchers::WithinAbs;

namespace
{
void writeWave (const std::filesystem::path& path, const std::vector<std::vector<float>>& samples)
{
    WriteSpec spec;
    spec.numChannels = (int) samples.size();
    spec.sampleRate = 44100.0;
    spec.bitsPerSample = 32;
    auto writer = FileWriter::create (path, spec);
    REQUIRE (writer != nullptr);
    std::vector<const float*> channels;
    for (const auto& channel : samples) channels.push_back (channel.data());
    REQUIRE (writer->write (channels.data(), spec.numChannels, (int64_t) samples.front().size()));
}

void checkPeak (const std::optional<WaveformPeaks::Peak>& peak, float minimum, float maximum)
{
    REQUIRE (peak.has_value());
    REQUIRE_THAT (peak->minimum, WithinAbs (minimum, 1.0e-6));
    REQUIRE_THAT (peak->maximum, WithinAbs (maximum, 1.0e-6));
}

WaveformSource::Snapshot awaitSnapshot (const WaveformSource& source)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    auto snapshot = source.snapshot();
    while (snapshot.state == WaveformSource::State::Loading
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        snapshot = source.snapshot();
    }
    REQUIRE (snapshot.state != WaveformSource::State::Loading);
    return snapshot;
}
}

TEST_CASE ("Waveform peaks preserve channels, polarity, gain and the final partial bin", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform");
    const auto file = dir.path() / "stereo.wav";
    constexpr int frames = 65537;
    std::vector<std::vector<float>> samples (2, std::vector<float> (frames, 0.25f));
    std::fill (samples[1].begin(), samples[1].end(), -0.5f);
    samples[0][511] = 1.5f;
    samples[0][512] = -0.75f;
    samples[1][1030] = -1.25f;
    samples[1][frames - 1] = -0.875f;
    writeWave (file, samples);
    const auto peaks = WaveformPeaks::generate (file);
    STATIC_REQUIRE (std::is_const_v<std::remove_reference_t<decltype (*peaks)>>);
    REQUIRE (peaks != nullptr);
    REQUIRE (peaks->info().numChannels == 2);
    REQUIRE (peaks->info().numFrames == frames);
    REQUIRE (peaks->framesPerPeak() == 2);
    REQUIRE_THAT (peaks->info().sampleRate, WithinAbs (44100.0, 1.0e-9));
    REQUIRE_THAT (peaks->durationSeconds(), WithinAbs ((double) frames / 44100.0, 1.0e-12));
    checkPeak (peaks->query (0, 0, 512), 0.25f, 1.5f);
    checkPeak (peaks->query (0, 512, 1024), -0.75f, 0.25f);
    checkPeak (peaks->query (0, 1024, 1031), 0.25f, 0.25f);
    checkPeak (peaks->query (1, 0, 1024), -0.5f, -0.5f);
    checkPeak (peaks->query (1, 1024, 1031), -1.25f, -0.5f);
    checkPeak (peaks->query (1, frames - 1, frames), -0.875f, -0.875f);
    std::filesystem::remove (file);
    checkPeak (peaks->query (1, 0, 1031), -1.25f, -0.5f);
}

TEST_CASE ("Waveform range queries match conservative sample extrema at every pyramid level", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-ranges");
    const auto file = dir.path() / "ranges.wav";
    constexpr int frames = 512 * 35 + 17;
    std::vector<std::vector<float>> samples (3, std::vector<float> (frames));
    for (int channel = 0; channel < 3; ++channel)
        for (int frame = 0; frame < frames; ++frame)
            samples[(size_t) channel][(size_t) frame] = std::sin ((float) (frame * (channel + 1)) * 0.0031f);
    writeWave (file, samples);
    const auto peaks = WaveformPeaks::generate (file);
    REQUIRE (peaks != nullptr);
    for (int channel = 0; channel < 3; ++channel)
        for (int first = 0; first < frames; first += 509)
            for (int end = first + 1; end <= frames; end += 997)
            {
                const int resolution = peaks->framesPerPeak();
                const int expandedFirst = first / resolution * resolution;
                const int expandedEnd = std::min (frames, ((end - 1) / resolution + 1) * resolution);
                const auto& data = samples[(size_t) channel];
                const auto range = std::minmax_element (data.begin() + expandedFirst, data.begin() + expandedEnd);
                checkPeak (peaks->query (channel, first, end), *range.first, *range.second);
            }
    const auto firstBin = std::minmax_element (samples[0].begin(), samples[0].begin() + peaks->framesPerPeak());
    checkPeak (peaks->query (0, -100, 1), *firstBin.first, *firstBin.second);
    REQUIRE_FALSE (peaks->query (-1, 0, 1));
    REQUIRE_FALSE (peaks->query (3, 0, 1));
    REQUIRE_FALSE (peaks->query (0, 1, 1));
    REQUIRE_FALSE (peaks->query (0, 100, 99));
    REQUIRE_FALSE (peaks->query (0, -100, -1));
    REQUIRE_FALSE (peaks->query (0, frames, std::numeric_limits<int64_t>::max()));
    const auto whole = peaks->query (0, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
    const auto expected = std::minmax_element (samples[0].begin(), samples[0].end());
    checkPeak (whole, *expected.first, *expected.second);
}

TEST_CASE ("Short waveform overviews retain transients without coarse-bin smearing", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail");
    const auto file = dir.path() / "detail.wav";
    constexpr int frames = 44100 * 18 / 10;
    constexpr int transient = 30001;
    std::vector<float> samples (frames, 0.0f);
    samples[transient] = 1.0f;
    writeWave (file, { samples });
    const auto peaks = WaveformPeaks::generate (file);
    REQUIRE (peaks != nullptr);
    REQUIRE (peaks->framesPerPeak() == 2);
    checkPeak (peaks->query (0, transient, transient + 1), 0.0f, 1.0f);
    checkPeak (peaks->query (0, transient - 3, transient - 1), 0.0f, 0.0f);
    checkPeak (peaks->query (0, transient + 1, transient + 3), 0.0f, 0.0f);
    constexpr int width = 1200;
    int occupiedPixels = 0;
    for (int pixel = 0; pixel < width; ++pixel)
    {
        const int first = pixel * frames / width;
        const int end = ((pixel + 1) * frames + width - 1) / width;
        const auto peak = peaks->query (0, first, end);
        REQUIRE (peak.has_value());
        if (peak->maximum > 0.0f) ++occupiedPixels;
        if (first <= transient && end > transient)
            REQUIRE_THAT (peak->maximum, WithinAbs (1.0f, 1.0e-6));
    }
    REQUIRE (occupiedPixels == 1);
}

TEST_CASE ("Normal-duration waveform pixels conservatively cover source extrema", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-overview");
    const auto file = dir.path() / "overview.wav";
    constexpr int frames = 44100 * 60;
    std::vector<float> samples (frames, 0.0f);
    for (int frame = 121; frame < frames; frame += 10007)
        samples[(size_t) frame] = (frame % 2 == 0) ? 0.75f : -0.5f;
    writeWave (file, { samples });
    const auto peaks = WaveformPeaks::generate (file);
    REQUIRE (peaks != nullptr);
    REQUIRE (peaks->framesPerPeak() > 1);
    REQUIRE (peaks->framesPerPeak() <= WaveformPeaks::kMaxFramesPerPeak);
    constexpr int width = 1200;
    for (int pixel = 0; pixel < width; ++pixel)
    {
        const auto first = (int64_t) pixel * frames / width;
        const auto end = ((int64_t) (pixel + 1) * frames + width - 1) / width;
        const auto expected = std::minmax_element (samples.begin() + first, samples.begin() + end);
        const auto peak = peaks->query (0, first, end);
        REQUIRE (peak.has_value());
        REQUIRE (peak->minimum <= *expected.first);
        REQUIRE (peak->maximum >= *expected.second);
    }
}

TEST_CASE ("Waveform generation treats non-finite samples as silence", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-nonfinite");
    const auto file = dir.path() / "nonfinite.wav";
    writeWave (file, { { std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity(), 0.5f } });
    const auto peaks = WaveformPeaks::generate (file);
    REQUIRE (peaks != nullptr);
    checkPeak (peaks->query (0, 0, 4), 0.0f, 0.5f);
}

TEST_CASE ("Waveform generation handles missing, corrupt and empty sources", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-invalid");
    const auto file = dir.path() / "source.wav";
    REQUIRE_FALSE (WaveformPeaks::generate (file));
    { std::ofstream output (file); output << "not an audio file"; }
    REQUIRE_FALSE (WaveformPeaks::generate (file));
    WriteSpec spec;
    { auto writer = FileWriter::create (file, spec); REQUIRE (writer != nullptr); }
    const auto empty = WaveformPeaks::generate (file);
    REQUIRE (empty != nullptr);
    REQUIRE (empty->info().numFrames == 0);
    REQUIRE_THAT (empty->durationSeconds(), WithinAbs (0.0, 1.0e-12));
    REQUIRE_FALSE (empty->query (0, 0, 1));
}

TEST_CASE ("Waveform cancellation never publishes partial peaks and permits a fresh build", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-cancel");
    const auto file = dir.path() / "cancel.wav";
    writeWave (file, { std::vector<float> (40000, 0.625f) });
    int checks = 0;
    const auto complete = WaveformPeaks::generate (file, [&] { ++checks; return false; });
    REQUIRE (complete != nullptr);
    REQUIRE (checks > 8);
    for (int cancelAt = 1; cancelAt <= checks; ++cancelAt)
    {
        int current = 0;
        REQUIRE_FALSE (WaveformPeaks::generate (file, [&] { return ++current == cancelAt; }));
        REQUIRE (current == cancelAt);
    }
    checkPeak (complete->query (0, 0, 40000), 0.625f, 0.625f);
    const auto fresh = WaveformPeaks::generate (file);
    REQUIRE (fresh != nullptr);
    checkPeak (fresh->query (0, 0, 40000), 0.625f, 0.625f);
}

// NTFS needs explicit sparse-file setup; seeking alone can allocate gigabytes.
#ifndef _WIN32
TEST_CASE ("Waveform long-file cancellation uses bounded reads and 64-bit frame counts", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-long");
    const auto file = dir.path() / "sparse.wav";
    SF_INFO info {};
    info.channels = 1;
    info.samplerate = 48000;
    info.format = SF_FORMAT_RF64 | SF_FORMAT_PCM_16;
    auto* writer = sf_open (file.c_str(), SFM_WRITE, &info);
    REQUIRE (writer != nullptr);
    constexpr int64_t lastFrame = (int64_t) std::numeric_limits<int32_t>::max() + 1024;
    const auto position = sf_seek (writer, lastFrame, SEEK_SET);
    const float tail = 0.5f;
    const auto written = sf_writef_float (writer, &tail, 1);
    const auto closed = sf_close (writer);
    REQUIRE (position == lastFrame);
    REQUIRE (written == 1);
    REQUIRE (closed == 0);
    const auto reader = FileReader::open (file);
    REQUIRE (reader != nullptr);
    REQUIRE (reader->info().numFrames == lastFrame + 1);
    int checks = 0;
    REQUIRE_FALSE (WaveformPeaks::generate (file, [&] { return ++checks == 4; }));
    REQUIRE (checks == 4);
}
#endif

TEST_CASE ("Waveform source clear invalidates queued and active work", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-clear");
    const auto file = dir.path() / "clear.wav";
    writeWave (file, { std::vector<float> (40000, 0.25f) });
    WaveformSource source;
    REQUIRE (source.snapshot().state == WaveformSource::State::Empty);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        source.setFile (file);
        source.setFile ({});
        const auto snapshot = source.snapshot();
        REQUIRE (snapshot.state == WaveformSource::State::Empty);
        REQUIRE_FALSE (snapshot.peaks);
    }
}

TEST_CASE ("Waveform source publishes only the latest file and reloads changed contents", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-publish");
    const auto firstFile = dir.path() / "first.wav";
    const auto secondFile = dir.path() / "second.wav";
    writeWave (firstFile, { std::vector<float> (40000, 0.25f) });
    writeWave (secondFile, { std::vector<float> (1000, -0.5f) });
    WaveformSource source;
    source.setFile (firstFile);
    const auto original = awaitSnapshot (source);
    REQUIRE (original.state == WaveformSource::State::Ready);
    REQUIRE (original.peaks != nullptr);
    checkPeak (original.peaks->query (0, 0, 40000), 0.25f, 0.25f);

    source.setFile (firstFile);
    source.setFile ({});
    source.setFile (secondFile);
    const auto replacement = awaitSnapshot (source);
    REQUIRE (replacement.state == WaveformSource::State::Ready);
    REQUIRE (replacement.peaks != nullptr);
    REQUIRE (replacement.peaks->info().numFrames == 1000);
    checkPeak (replacement.peaks->query (0, 0, 1000), -0.5f, -0.5f);
    checkPeak (original.peaks->query (0, 0, 40000), 0.25f, 0.25f);

    writeWave (secondFile, { std::vector<float> (1000, 0.75f) });
    source.setFile (secondFile);
    const auto reloaded = awaitSnapshot (source);
    REQUIRE (reloaded.state == WaveformSource::State::Ready);
    REQUIRE (reloaded.peaks != nullptr);
    checkPeak (reloaded.peaks->query (0, 0, 1000), 0.75f, 0.75f);
    checkPeak (replacement.peaks->query (0, 0, 1000), -0.5f, -0.5f);
}

TEST_CASE ("Waveform source publishes failure for unreadable input and then clears", "[waveform]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-failed");
    WaveformSource source;
    source.setFile (dir.path() / "missing.wav");
    const auto failed = awaitSnapshot (source);
    REQUIRE (failed.state == WaveformSource::State::Failed);
    REQUIRE_FALSE (failed.peaks);
    source.setFile ({});
    const auto empty = source.snapshot();
    REQUIRE (empty.state == WaveformSource::State::Empty);
    REQUIRE_FALSE (empty.peaks);
}
