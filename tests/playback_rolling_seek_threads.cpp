#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/PlaybackEngine.h"
#include "engine/Transport.h"
#include "engine/audiofile/FileWriter.h"
#include "session/Session.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

using namespace duskstudio;

namespace
{
constexpr int kBlock = 256;
constexpr double kRate = 48000.0;
constexpr std::int64_t kFileFrames = 960000;
constexpr int kTracks = 4;
constexpr std::int64_t kTrackOffset = 3000;

// Every frame differs from its neighbours and from zero, so a frame played back
// is either the right one or silence.
float frameValue (std::int64_t frame) { return 0.125f + (float) frame * 4.0e-7f; }

bool silent (float value) { return std::fpclassify (value) == FP_ZERO; }

juce::File writeRamp (const std::filesystem::path& dir)
{
    const auto path = dir / "audio" / "ramp.wav";
    std::filesystem::create_directories (path.parent_path());
    auto writer = dusk::audio::FileWriter::create (path, { kRate, 1, 32 });
    REQUIRE (writer != nullptr);
    std::vector<float> samples ((std::size_t) kFileFrames);
    for (std::int64_t f = 0; f < kFileFrames; ++f) samples[(std::size_t) f] = frameValue (f);
    const float* channels[] { samples.data() };
    REQUIRE (writer->write (channels, 1, kFileFrames));
    writer.reset();
    return juce::File (juce::String::fromUTF8 (path.u8string().c_str()));
}

// What the audio thread saw, read by the test only after it has joined.
struct Observed
{
    std::uint64_t blocks = 0, commits = 0, right = 0, silent = 0, wrong = 0;
    std::int64_t firstWrongAt = -1, lastCommitAt = -1;
    int firstWrongTrack = -1;
    float firstWrongValue = 0.0f, firstWrongExpected = 0.0f;
    // Blocks from the latest commit until every track played every frame right.
    std::int64_t blocksUntilWhole = -1;
};
} // namespace

TEST_CASE ("Rolling seeks against live fill workers play every frame right or silent",
           "[playback][reader-pool][seek][ipc]")
{
    // [ipc]: real fill workers and a paced audio thread, so it runs alone.
    test::TempDirectory dir { "dusk-rolling-seek-" };
    const auto file = writeRamp (dir.path());
    auto session = std::make_unique<Session>();
    session->setSessionDirectory (file.getParentDirectory().getParentDirectory());
    for (int t = 0; t < kTracks; ++t)
    {
        AudioRegion region;
        region.file = file;
        region.sourceOffset = t * kTrackOffset;
        region.lengthInSamples = kFileFrames - t * kTrackOffset;
        session->track (t).regions = { region };
    }
    const int streamsBefore = PlaybackEngine::liveStreamCountForTest();

    Transport transport;
    auto engine = std::make_unique<PlaybackEngine> (*session);
    engine->bindTransport (transport);
    engine->prepare (kBlock, kRate);
    transport.setPlayhead (0);
    transport.setState (Transport::State::Playing);
    engine->preparePlayback();
    REQUIRE (engine->readerStats().workers >= 2);

    Observed seen;
    std::atomic<bool> stopAudio { false };
    std::atomic<std::int64_t> lastCommitted { -1 };
    std::thread audio ([&]
    {
        std::vector<float> out ((std::size_t) kBlock);
        const auto period = std::chrono::microseconds ((int) (1.0e6 * kBlock / kRate));
        auto due = std::chrono::steady_clock::now();
        auto serial = transport.getLocateCount();
        while (! stopAudio.load (std::memory_order_acquire))
        {
            bool keepSeek = false;
            const auto at = transport.beginAudioBlock ([&] (std::int64_t& target) noexcept
            {
                keepSeek = true;
                return engine->locateReadyRt (target, kBlock, -1, -1);
            });
            engine->finishLocateBlockRt (at, keepSeek);
            engine->beginAudioBlock (at, -1, -1);
            if (transport.getLocateCount() != serial)
            {
                serial = transport.getLocateCount();
                ++seen.commits;
                seen.lastCommitAt = at;
                seen.blocksUntilWhole = -1;
                lastCommitted.store (at, std::memory_order_release);
            }
            bool whole = true;
            for (int t = 0; t < kTracks; ++t)
            {
                engine->readForTrack (t, at, out.data(), nullptr, kBlock);
                for (int i = 0; i < kBlock; ++i)
                {
                    const auto frame = at + i;
                    const auto value = out[(std::size_t) i];
                    const bool inRegion = frame < kFileFrames - t * kTrackOffset;
                    const float expected = inRegion ? frameValue (frame + t * kTrackOffset) : 0.0f;
                    if (inRegion && std::abs (value - expected) <= 1.0e-7f) ++seen.right;
                    else if (silent (value)) { ++seen.silent; whole = whole && ! inRegion; }
                    else
                    {
                        whole = false;
                        if (seen.wrong++ == 0)
                        {
                            seen.firstWrongAt = frame;
                            seen.firstWrongTrack = t;
                            seen.firstWrongValue = value;
                            seen.firstWrongExpected = expected;
                        }
                    }
                }
            }
            if (whole && seen.blocksUntilWhole < 0 && seen.lastCommitAt >= 0)
                seen.blocksUntilWhole = (at - seen.lastCommitAt) / kBlock;
            transport.finishAudioBlock (at + kBlock);
            ++seen.blocks;
            due += period;
            std::this_thread::sleep_until (due);
        }
    });

    // Retargets arrive faster than some seeks warm, so many supersede a seek still
    // waiting, and the rest land on audio that is playing from a committed seek.
    std::mt19937 random (922);
    std::uniform_int_distribution<std::int64_t> anywhere (0, kFileFrames - 4 * (std::int64_t) kRate);
    constexpr std::array<int, 3> gapsMs { 2, 15, 120 };
    std::int64_t lastTarget = 0;
    for (int seek = 0; seek < 48; ++seek)
    {
        lastTarget = anywhere (random);
        transport.requestLocate (lastTarget);
        const auto until = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds (gapsMs[(std::size_t) seek % gapsMs.size()]);
        while (std::chrono::steady_clock::now() < until)
        {
            engine->service();
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }
    // The last seek commits within its 300 ms limit; the deadline only bounds a
    // stalled run, which the checks below then report.
    const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now() < settle
           && lastCommitted.load (std::memory_order_acquire) != lastTarget)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    // Long enough for the last seek's readers to fill well past its warm span.
    std::this_thread::sleep_for (std::chrono::milliseconds (400));
    stopAudio.store (true, std::memory_order_release);
    audio.join();
    engine->stopPlayback();
    engine.reset();

    INFO ("blocks " << seen.blocks << ", commits " << seen.commits << ", right " << seen.right
          << ", silent " << seen.silent << ", wrong " << seen.wrong << " (first at frame "
          << seen.firstWrongAt << " on track " << seen.firstWrongTrack << ": "
          << seen.firstWrongValue << " for " << seen.firstWrongExpected << ")");
    CHECK (seen.wrong == 0);
    CHECK (seen.commits >= 6);
    CHECK (seen.right > 0);
    REQUIRE (transport.getPlayhead() == transport.getCommittedPlayhead());
    REQUIRE (seen.lastCommitAt == lastTarget);
    // The last seek plays whole within its warm span once it has committed.
    REQUIRE (seen.blocksUntilWhole >= 0);
    CHECK (seen.blocksUntilWhole * kBlock <= 8192);
    CHECK (PlaybackEngine::liveStreamCountForTest() == streamsBefore);
}
