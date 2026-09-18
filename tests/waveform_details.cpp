#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/WaveformPeaks.h"
#include "engine/audiofile/FileWriter.h"
#include "TestTempDirectory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

using namespace dusk::audio;
using Catch::Matchers::WithinAbs;

namespace
{
void writeDetailFile (const std::filesystem::path& file, const std::vector<std::vector<float>>& samples)
{
    WriteSpec spec;
    spec.sampleRate = 48000.0;
    spec.numChannels = (int) samples.size();
    spec.bitsPerSample = 32;
    auto writer = FileWriter::create (file, spec);
    REQUIRE (writer != nullptr);
    std::vector<const float*> channels;
    for (const auto& samplesForChannel : samples) channels.push_back (samplesForChannel.data());
    REQUIRE (writer->write (channels.data(), spec.numChannels, (int64_t) samples.front().size()));
}

void checkDetail (const std::optional<WaveformPeaks::Peak>& peak, float minimum, float maximum)
{
    REQUIRE (peak.has_value());
    REQUIRE_THAT (peak->minimum, WithinAbs (minimum, 1.0e-6));
    REQUIRE_THAT (peak->maximum, WithinAbs (maximum, 1.0e-6));
}

WaveformSource::Snapshot waitForDetail (const WaveformSource& source, bool overview = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    auto snapshot = source.snapshot();
    while ((overview ? snapshot.state : snapshot.detailState) == WaveformSource::State::Loading
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
        snapshot = source.snapshot();
    }
    REQUIRE ((overview ? snapshot.state : snapshot.detailState) != WaveformSource::State::Loading);
    return snapshot;
}
}

TEST_CASE ("Waveform details preserve individual samples across read boundaries", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-samples");
    const auto file = dir.path() / "samples.wav";
    std::vector<std::vector<float>> samples (2, std::vector<float> (9000));
    for (int frame = 0; frame < 9000; ++frame)
    {
        samples[0][(size_t) frame] = (float) (frame % 17) * 0.1f;
        samples[1][(size_t) frame] = -0.75f;
    }
    samples[0][8191] = std::numeric_limits<float>::quiet_NaN();
    samples[0][8192] = 1.75f;
    writeDetailFile (file, samples);
    auto reader = FileReader::open (file);
    REQUIRE (reader != nullptr);
    const auto detail = WaveformDetails::generate (*reader, { { 0, 9000, 9000, 0, 9000 }, { 31, 1, 10, 0, 10 } });
    REQUIRE (detail != nullptr);
    REQUIRE_THAT (detail->info().sampleRate, WithinAbs (48000.0, 1.0e-9));
    for (int frame = 0; frame < 9000; ++frame)
    {
        const float value = frame == 8191 ? 0.0f : samples[0][(size_t) frame];
        checkDetail (detail->column (0, 0, frame), value, value);
        checkDetail (detail->column (0, 1, frame), -0.75f, -0.75f);
    }
    for (int column = 0; column < 10; ++column)
        checkDetail (detail->column (1, 0, column), samples[0][31], samples[0][31]);
    REQUIRE_FALSE (detail->column (2, 0, 0));
    REQUIRE_FALSE (detail->column (0, -1, 0));
    REQUIRE_FALSE (detail->column (0, 2, 0));
    REQUIRE_FALSE (detail->column (0, 0, -1));
    REQUIRE_FALSE (detail->column (0, 0, 9000));

#ifndef _WIN32
    SECTION ("An open source stays consistent after atomic path replacement")
    {
        const auto replacement = dir.path() / "replacement.wav";
        writeDetailFile (replacement, { std::vector<float> (500, -0.5f) });
        std::filesystem::rename (replacement, file);
        const auto overview = WaveformPeaks::generate (*reader);
        REQUIRE (overview != nullptr);
        REQUIRE (overview->info().numFrames == 9000);
        REQUIRE (overview->info().numChannels == 2);
        checkDetail (overview->query (1, 0, 9000), -0.75f, -0.75f);
        const auto retained = WaveformDetails::generate (*reader, { { 8192, 1, 1, 0, 1 } });
        REQUIRE (retained != nullptr);
        checkDetail (retained->column (0, 0, 0), 1.75f, 1.75f);
        const auto reopened = WaveformPeaks::generate (file);
        REQUIRE (reopened != nullptr);
        REQUIRE (reopened->info().numFrames == 500);
        REQUIRE (reopened->info().numChannels == 1);
        checkDetail (reopened->query (0, 0, 500), -0.5f, -0.5f);
    }
#endif
}

TEST_CASE ("Waveform detail windows retain full mappings and clipped disjoint extrema", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-windows");
    const auto file = dir.path() / "windows.wav";
    std::vector<float> samples (12000);
    for (size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = std::sin ((float) frame * 0.0321f);
    writeDetailFile (file, { samples });
    auto reader = FileReader::open (file);
    REQUIRE (reader != nullptr);
    const std::vector<WaveformDetails::Window> windows {
        { 13, 9327, 1200, 171, 834 }, { 11985, 200, 99, 0, 99 }, { 500, 1000, 77, 0, 77 },
        { std::numeric_limits<int64_t>::max() - 10, 100, 100, 0, 100 },
        { 0, (int64_t) std::numeric_limits<int>::max() * 512 - 1,
          std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - 3, 3 } };
    const auto detail = WaveformDetails::generate (*reader, windows);
    REQUIRE (detail != nullptr);
    REQUIRE (detail->windows() == windows);
    for (size_t index = 0; index < windows.size(); ++index)
    {
        const auto& window = windows[index];
        for (int column = 0; column < window.numColumns; ++column)
        {
            const long double first = (long double) window.sourceStart
                + std::floor ((long double) window.sourceLength * (window.firstColumn + column) / window.fullWidth);
            const long double end = (long double) window.sourceStart
                + std::ceil ((long double) window.sourceLength * (window.firstColumn + column + 1) / window.fullWidth);
            const auto a = (size_t) std::min (first, (long double) samples.size());
            const auto b = (size_t) std::min (end, (long double) samples.size());
            const auto range = window.columnRange (column, (int64_t) samples.size());
            REQUIRE (range.has_value());
            REQUIRE (range->first == (int64_t) a);
            REQUIRE (range->second == (int64_t) b);
            if (a == b) checkDetail (detail->column (index, 0, column), 0.0f, 0.0f);
            else
            {
                const auto expected = std::minmax_element (samples.begin() + (ptrdiff_t) a,
                                                           samples.begin() + (ptrdiff_t) b);
                checkDetail (detail->column (index, 0, column), *expected.first, *expected.second);
            }
        }
    }
}

TEST_CASE ("Waveform detail admission bounds invalid geometry and complete result memory", "[waveform][detail]")
{
    using Window = WaveformDetails::Window;
    const std::vector<Window> invalid {
        { -1, 1, 1, 0, 1 }, { 0, 0, 1, 0, 1 }, { 0, 1, 0, 0, 1 }, { 0, 1, 1, -1, 1 },
        { 0, 1, 1, 0, 0 }, { 0, 1, 1, 0, 2 }, { 0, 513, 1, 0, 1 },
        { 0, std::numeric_limits<int64_t>::max(), std::numeric_limits<int>::max(), 0, 1 },
        { 0, 1, std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - 1, 2 },
        { 0, 65537, 65537, 0, 65537 } };
    for (const auto& window : invalid) REQUIRE_FALSE (WaveformDetails::validRequest ({ window }, 1));
    REQUIRE_FALSE (WaveformDetails::validRequest ({ { 0, 1, 1, 0, 1 } }, 0));
    REQUIRE_FALSE (WaveformDetails::validRequest ({ { 0, 1, 1, 0, 1 } }, 257));
    REQUIRE (WaveformDetails::validRequest ({ { 0, 65536, 65536, 0, 65536 } }, 1));
    REQUIRE_FALSE (WaveformDetails::validRequest ({ { 0, 65536, 65536, 0, 65536 } }, 256));
    std::vector<Window> many (WaveformDetails::kMaxColumns, { 0, 1, 1, 0, 1 });
    REQUIRE (WaveformDetails::validRequest (many, 1));
    REQUIRE_FALSE (WaveformDetails::validRequest (many, 32));
    many.push_back ({ 0, 1, 1, 0, 1 });
    REQUIRE_FALSE (WaveformDetails::validRequest (many, 1));

    const Window ordinary { 0, 10, 10, 0, 10 };
    REQUIRE_FALSE (ordinary.columnRange (-1, 10));
    REQUIRE_FALSE (ordinary.columnRange (10, 10));
    REQUIRE_FALSE (ordinary.columnRange (0, -1));
    for (size_t index : { 0u, 1u, 2u, 3u, 4u, 5u, 8u })
        REQUIRE_FALSE (invalid[index].columnRange (0, 100));
    const Window extreme { 0, std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - 1, 1 };
    const auto finalRange = extreme.columnRange (0, std::numeric_limits<int64_t>::max());
    REQUIRE (finalRange.has_value());
    REQUIRE (finalRange->first == std::numeric_limits<int64_t>::max() - INT64_C (4294967299));
    REQUIRE (finalRange->second == std::numeric_limits<int64_t>::max());
}

TEST_CASE ("Waveform detail cancellation never returns partial batches", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-cancel");
    const auto file = dir.path() / "cancel.wav";
    writeDetailFile (file, { std::vector<float> (50000, 0.625f) });
    auto reader = FileReader::open (file);
    REQUIRE (reader != nullptr);
    const std::vector<WaveformDetails::Window> windows { { 0, 40000, 100, 0, 100 }, { 100, 10000, 1000, 300, 500 } };
    int checks = 0;
    const auto complete = WaveformDetails::generate (*reader, windows, [&] { ++checks; return false; });
    REQUIRE (complete != nullptr);
    REQUIRE (checks > 6);
    for (int cancelAt = 1; cancelAt <= checks; ++cancelAt)
    {
        int current = 0;
        REQUIRE_FALSE (WaveformDetails::generate (*reader, windows, [&] { return ++current == cancelAt; }));
        REQUIRE (current == cancelAt);
    }
    checkDetail (complete->column (0, 0, 50), 0.625f, 0.625f);
    const auto fresh = WaveformDetails::generate (*reader, windows);
    REQUIRE (fresh != nullptr);
    checkDetail (fresh->column (1, 0, 499), 0.625f, 0.625f);
}

TEST_CASE ("Waveform viewport publication preserves completed overview and immutable detail", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-publish");
    const auto file = dir.path() / "publish.wav";
    std::vector<float> samples (100000, 0.25f);
    samples[123] = -0.75f;
    writeDetailFile (file, { samples });
    WaveformSource source;
    source.setFile (file);
    const auto original = waitForDetail (source, true);
    REQUIRE (original.state == WaveformSource::State::Ready);
    REQUIRE (original.peaks != nullptr);
    REQUIRE (original.info.has_value());
    REQUIRE_THAT (original.info->sampleRate, WithinAbs (48000.0, 1.0e-9));
    const std::vector<WaveformDetails::Window> first { { 120, 10, 10, 0, 10 } };
    REQUIRE (source.setDetailWindows (first));
    REQUIRE (source.snapshot().peaks == original.peaks);
    const auto loaded = waitForDetail (source);
    REQUIRE (loaded.detailState == WaveformSource::State::Ready);
    REQUIRE (loaded.details != nullptr);
    checkDetail (loaded.details->column (0, 0, 3), -0.75f, -0.75f);
    REQUIRE (source.setDetailWindows (first));
    REQUIRE (source.snapshot().details == loaded.details);
    const std::vector<WaveformDetails::Window> second { { 500, 10, 10, 0, 10 } };
    REQUIRE (source.setDetailWindows (second));
    REQUIRE (source.snapshot().peaks == original.peaks);
    const auto changed = waitForDetail (source);
    REQUIRE (changed.details != nullptr);
    REQUIRE (changed.details->windows() == second);
    REQUIRE (changed.state == WaveformSource::State::Ready);
    checkDetail (changed.details->column (0, 0, 3), 0.25f, 0.25f);
    checkDetail (loaded.details->column (0, 0, 3), -0.75f, -0.75f);
}

TEST_CASE ("Invalid waveform viewport clears prior Ready detail and empty clears failure", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-invalid");
    const auto file = dir.path() / "valid.wav";
    writeDetailFile (file, { std::vector<float> (1000, 0.25f) });
    WaveformSource source;
    source.setFile (file);
    REQUIRE (source.setDetailWindows ({ { 0, 1000, 1000, 0, 1000 } }));
    const auto original = waitForDetail (source);
    REQUIRE (original.details != nullptr);
    REQUIRE_FALSE (source.setDetailWindows ({ { 0, 1000, 1, 0, 1 } }));
    const auto failed = source.snapshot();
    REQUIRE (failed.detailState == WaveformSource::State::Failed);
    REQUIRE_FALSE (failed.details);
    REQUIRE (source.setDetailWindows ({}));
    REQUIRE (source.snapshot().detailState == WaveformSource::State::Empty);
    REQUIRE_FALSE (source.snapshot().details);
    checkDetail (original.details->column (0, 0, 0), 0.25f, 0.25f);
}

TEST_CASE ("Waveform worker validates early detail requests against actual source channels", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-channel-budget");
    const auto file = dir.path() / "many-channels.wav";
    writeDetailFile (file, std::vector<std::vector<float>> (256, std::vector<float> (1, 0.25f)));
    const std::vector<WaveformDetails::Window> windows { { 0, 1, 65536, 0, 65536 } };
    REQUIRE (WaveformDetails::validRequest (windows, 1));
    REQUIRE_FALSE (WaveformDetails::validRequest (windows, 256));
    WaveformSource source;
    source.setFile (file);
    // Metadata may arrive before admission; both schedules must refuse Ready.
    source.setDetailWindows (windows);
    const auto failed = waitForDetail (source);
    REQUIRE (failed.detailState == WaveformSource::State::Failed);
    REQUIRE_FALSE (failed.details);
    const auto loaded = waitForDetail (source, true);
    REQUIRE (loaded.state == WaveformSource::State::Ready);
    REQUIRE (loaded.info.has_value());
    REQUIRE (loaded.info->numChannels == 256);
    REQUIRE_FALSE (source.setDetailWindows (windows));
    REQUIRE (source.setDetailWindows ({}));
    REQUIRE (source.setDetailWindows ({ { 0, 1, 1, 0, 1 } }));
    const auto valid = waitForDetail (source);
    REQUIRE (valid.details != nullptr);
    checkDetail (valid.details->column (0, 255, 0), 0.25f, 0.25f);
}

TEST_CASE ("Waveform worker rejects superseded viewports and source replacements", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-replace");
    const auto first = dir.path() / "first.wav";
    const auto second = dir.path() / "second.wav";
    writeDetailFile (first, { std::vector<float> (50000, 0.25f) });
    writeDetailFile (second, { std::vector<float> (1000, -0.5f) });
    WaveformSource source;
    const std::vector<WaveformDetails::Window> latest { { 17, 10, 10, 0, 10 } };
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        source.setFile (first);
        REQUIRE (source.setDetailWindows ({ { 0, 50000, 100, 0, 100 } }));
        REQUIRE (source.setDetailWindows (latest));
        const auto viewport = waitForDetail (source);
        REQUIRE (viewport.details != nullptr);
        REQUIRE (viewport.details->windows() == latest);
        checkDetail (viewport.details->column (0, 0, 0), 0.25f, 0.25f);
        REQUIRE (source.setDetailWindows ({ { 0, 50000, 100, 0, 100 } }));
        source.setFile (second);
        REQUIRE (source.setDetailWindows (latest));
        const auto replacement = waitForDetail (source);
        REQUIRE (replacement.details != nullptr);
        REQUIRE (replacement.info.has_value());
        REQUIRE (replacement.info->numFrames == 1000);
        checkDetail (replacement.details->column (0, 0, 0), -0.5f, -0.5f);
        checkDetail (viewport.details->column (0, 0, 0), 0.25f, 0.25f);
        source.setFile ({});
        REQUIRE (source.snapshot().detailState == WaveformSource::State::Empty);
        REQUIRE_FALSE (source.snapshot().details);
        REQUIRE_FALSE (source.snapshot().info);
    }
}

TEST_CASE ("Waveform detail worker reports unreadable sources without stale metadata", "[waveform][detail]")
{
    duskstudio::test::TempDirectory dir ("dusk-waveform-detail-missing");
    WaveformSource source;
    source.setFile (dir.path() / "missing.wav");
    REQUIRE (source.setDetailWindows ({ { 0, 10, 10, 0, 10 } }));
    const auto failed = waitForDetail (source);
    REQUIRE (failed.state == WaveformSource::State::Failed);
    REQUIRE (failed.detailState == WaveformSource::State::Failed);
    REQUIRE_FALSE (failed.info);
    REQUIRE_FALSE (failed.details);
    source.setFile ({});
    REQUIRE_FALSE (source.setDetailWindows ({ { 0, 1, 1, 0, 1 } }));
    REQUIRE (source.setDetailWindows ({}));
    REQUIRE (source.snapshot().detailState == WaveformSource::State::Empty);
}
