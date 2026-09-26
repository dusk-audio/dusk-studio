#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "foundation/Fs.h"
#include "session/Session.h"
#include "session/UnreferencedAudio.h"

#include <juce_core/juce_core.h>

#include <filesystem>
#include <system_error>
#include <vector>

using namespace duskstudio;

namespace
{
struct ScopedDir
{
    std::filesystem::path d;
    ~ScopedDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all (d, ignored);
    }
};

juce::File makeTempDir (const ScopedDir& scoped)
{
    REQUIRE_FALSE (scoped.d.empty());
    return juce::File (juce::String::fromUTF8 (scoped.d.u8string().c_str()));
}

void makeMonoTracks (Session& session)
{
    for (int t = 0; t < Session::kNumTracks; ++t)
        session.track (t).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
}
} // namespace

// When the audio thread will not leave writeInputBlock, stopRecording gives up
// its teardown and leaves the take's writers registered with the drain pool,
// whose disk thread keeps draining them through raw pointers. The next
// startRecording has to take each one out of the pool before freeing it; freed
// while registered, the disk thread drains freed memory, and the stale entry
// also holds one of the pool's kNumTracks places, so a take on every track
// loses its last one. The bailed take was announced as dropped, so its file
// goes too and no WAV is left without a region.
TEST_CASE ("A take after a bailed stop reclaims the bailed writers first",
           "[recording][recordmanager]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 256;

    const ScopedDir scoped { dusk::fs::createUniqueTempDirectory ("dusk-bailed-stop-") };
    const auto dir = makeTempDir (scoped);

    Session session;
    session.setSessionDirectory (dir);
    makeMonoTracks (session);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    const std::vector<float> block ((size_t) kBlockSize, 0.1f);
    REQUIRE (rm.startRecording (kSampleRate, 0));
    rm.writeInputBlock (0, block.data(), nullptr, kBlockSize);

    rm.holdAudioInFlightForTest (true);
    rm.stopRecording (kBlockSize);
    CHECK (session.track (0).regions.empty());
    CHECK (rm.hasOpenTake());
    CHECK_FALSE (rm.startRecording (kSampleRate, kBlockSize));
    rm.holdAudioInFlightForTest (false);

    for (int t = 0; t < Session::kNumTracks; ++t)
        session.setTrackArmed (t, true);
    for (int t = 0; t < Session::kNumTracks; ++t)
        REQUIRE (session.track (t).recordArmed.load (std::memory_order_relaxed));

    REQUIRE (rm.startRecording (kSampleRate, kBlockSize));
    CHECK (rm.getLastSetupFailures().empty());

    for (int t = 0; t < Session::kNumTracks; ++t)
        rm.writeInputBlock (t, block.data(), nullptr, kBlockSize);
    rm.stopRecording (2 * kBlockSize);

    CHECK_FALSE (rm.hasOpenTake());
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        INFO ("track " << t + 1);
        const auto& regions = session.track (t).regions;
        REQUIRE (regions.size() == 1);
        CHECK (regions.front().timelineStart == kBlockSize);
        CHECK (regions.front().lengthInSamples == kBlockSize);
        CHECK (regions.front().file.existsAsFile());
    }
    CHECK (findUnreferencedAudio (session).files.empty());
}

// Clean out asks whether a take is open before it lists what it would delete,
// and a bailed take counts as open until something discards it. Nothing is
// recording, so once the audio thread has left the take has to go without a
// new take having to start first; before that nothing may touch it.
TEST_CASE ("A bailed take is reclaimed without starting another once the audio thread leaves",
           "[recording][recordmanager]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 256;

    const ScopedDir scoped { dusk::fs::createUniqueTempDirectory ("dusk-bailed-stop-") };
    const auto dir = makeTempDir (scoped);

    Session session;
    session.setSessionDirectory (dir);
    makeMonoTracks (session);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    const std::vector<float> block ((size_t) kBlockSize, 0.1f);
    REQUIRE (rm.startRecording (kSampleRate, 0));
    CHECK_FALSE (rm.reclaimBailedTake());
    CHECK (rm.hasOpenTake());
    rm.writeInputBlock (0, block.data(), nullptr, kBlockSize);

    rm.holdAudioInFlightForTest (true);
    rm.stopRecording (kBlockSize);
    const auto bailed = findUnreferencedAudio (session).files;
    REQUIRE (bailed.size() == 1);
    CHECK_FALSE (rm.reclaimBailedTake());
    CHECK (rm.hasOpenTake());
    CHECK (std::filesystem::exists (bailed.front()));

    rm.holdAudioInFlightForTest (false);
    CHECK (rm.reclaimBailedTake());
    CHECK_FALSE (rm.hasOpenTake());
    CHECK_FALSE (std::filesystem::exists (bailed.front()));
    CHECK (findUnreferencedAudio (session).files.empty());
    CHECK (session.track (0).regions.empty());

    CHECK (rm.reclaimBailedTake());
    REQUIRE (rm.startRecording (kSampleRate, kBlockSize));
    rm.writeInputBlock (0, block.data(), nullptr, kBlockSize);
    rm.stopRecording (2 * kBlockSize);
    REQUIRE (session.track (0).regions.size() == 1);
    CHECK (session.track (0).regions.front().file.existsAsFile());
}
