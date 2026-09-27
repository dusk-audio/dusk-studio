#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "engine/audiofile/FileReader.h"
#include "foundation/Fs.h"
#include "foundation/MidiBuffer.h"
#include "session/Session.h"
#include "session/UnreferencedAudio.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <thread>
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

std::int64_t framesOnDisk (const AudioRegion& region)
{
    auto reader = dusk::audio::FileReader::open (
        std::filesystem::u8path (region.file.getFullPathName().toStdString()));
    return reader != nullptr ? reader->info().numFrames : -1;
}

// Stands in for an audio-thread call that is still inside writeInputBlock
// when the stop begins, and leaves only after the stop has waited a given
// number of passes.
struct HeldCall
{
    RecordManager& recorder;
    int leaveAfterPasses = 0;
    int passes = 0;

    static void pass (void* context)
    {
        auto& self = *static_cast<HeldCall*> (context);
        if (++self.passes == self.leaveAfterPasses)
            self.recorder.holdAudioInFlightForTest (false);
    }
};
} // namespace

// The stop cannot tear the writers down while an audio-thread call is inside
// the recorder, and it cannot commit a take it has not torn down. It has to
// wait until the call leaves, however long that is, and then commit: a stop
// that gives up after a fixed number of passes throws the take away.
TEST_CASE ("A stop waits for an audio-thread call still inside the recorder and commits its take",
           "[recording][recordmanager]")
{
    static constexpr double kSampleRate = 48000.0;
    static constexpr int    kBlockSize  = 256;
    static constexpr int    kBlocks     = 4;

    const ScopedDir scoped { dusk::fs::createUniqueTempDirectory ("dusk-stop-waits-") };
    const auto dir = makeTempDir (scoped);

    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    const std::vector<float> block ((size_t) kBlockSize, 0.1f);
    REQUIRE (rm.startRecording (kSampleRate, 0));
    for (int i = 0; i < kBlocks; ++i)
        rm.writeInputBlock (0, block.data(), nullptr, kBlockSize);

    HeldCall held { rm, 5000 };
    rm.holdAudioInFlightForTest (true);
    rm.setAudioWaitObserverForTest (&held, &HeldCall::pass);
    rm.stopRecording (kBlocks * kBlockSize);
    rm.setAudioWaitObserverForTest (nullptr, nullptr);
    if (held.passes < held.leaveAfterPasses)
        rm.holdAudioInFlightForTest (false);

    CHECK (held.passes == held.leaveAfterPasses);
    CHECK_FALSE (rm.isActive());
    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 1);
    CHECK (regions.front().timelineStart == 0);
    CHECK (regions.front().lengthInSamples == kBlocks * kBlockSize);
    CHECK (framesOnDisk (regions.front()) == kBlocks * kBlockSize);
    CHECK (findUnreferencedAudio (session).files.empty());
    CHECK (rm.getLastCommitDiff().size() == 1);

    REQUIRE (rm.startRecording (kSampleRate, kBlocks * kBlockSize));
    rm.writeInputBlock (0, block.data(), nullptr, kBlockSize);
    rm.stopRecording ((kBlocks + 1) * kBlockSize);
    CHECK (session.track (0).regions.size() == 2);
}

// A real audio thread writes into every take while the message thread starts
// and stops them, so each stop lands while that thread may be mid-write. Every
// take has to commit, audio and MIDI, with the WAV holding exactly the frames
// its region claims. Run under TSan this is the check that the stop's
// teardown never overlaps a write.
TEST_CASE ("Every take commits while a live audio thread keeps writing into it",
           "[recording][recordmanager]")
{
    static constexpr double       kSampleRate      = 48000.0;
    static constexpr int          kBlockSize       = 256;
    static constexpr int          kBlocksPerTake   = 8;
    static constexpr int          kTakes           = 40;
    static constexpr std::int64_t kTakeSpacing     = 1 << 20;
    static constexpr int          kAudioTracks     = 2;
    static constexpr int          kMidiTrack       = 2;

    const ScopedDir scoped { dusk::fs::createUniqueTempDirectory ("dusk-live-stop-") };
    const auto dir = makeTempDir (scoped);

    Session session;
    session.setSessionDirectory (dir);
    for (int t = 0; t < kAudioTracks; ++t)
    {
        session.track (t).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
        session.setTrackArmed (t, true);
    }
    session.track (kMidiTrack).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (kMidiTrack, true);

    RecordManager rm (session);

    // A grant packs the take number with the blocks left for it, so the audio
    // thread claims a block and learns which take it belongs to in one atomic
    // step. Grants are published after startRecording returns, so a claimed
    // block begins after its take was armed; the stop waits for two of them to
    // finish and then lands while the rest may still be going in.
    static constexpr std::int64_t kBlocksField = 16;
    std::atomic<std::int64_t> grant { 0 };
    std::array<std::atomic<int>, kTakes + 1> blocksDone {};
    std::atomic<bool> quit { false };

    std::thread audioThread ([&]
    {
        const std::vector<float> block ((size_t) kBlockSize, 0.25f);
        const std::uint8_t noteOn[]  { 0x90, 60, 100 };
        const std::uint8_t noteOff[] { 0x80, 60, 0 };
        dusk::MidiBuffer midi;
        midi.addEvent (noteOn, 3, 0);
        midi.addEvent (noteOff, 3, kBlockSize / 2);
        std::int64_t fromRecord = 0;
        std::int64_t lastTake = 0;
        while (! quit.load (std::memory_order_acquire))
        {
            auto claimed = grant.load (std::memory_order_acquire);
            if (claimed % kBlocksField == 0
                || ! grant.compare_exchange_weak (claimed, claimed - 1, std::memory_order_acq_rel))
            {
                std::this_thread::yield();
                continue;
            }
            const auto take = claimed / kBlocksField;
            if (take != lastTake)
            {
                lastTake = take;
                fromRecord = 0;
            }
            for (int t = 0; t < kAudioTracks; ++t)
                rm.writeInputBlock (t, block.data(), nullptr, kBlockSize);
            rm.writeMidiBlock (kMidiTrack, midi, fromRecord);
            fromRecord += kBlockSize;
            blocksDone[(size_t) take].fetch_add (1, std::memory_order_release);
        }
    });

    for (int take = 1; take <= kTakes; ++take)
    {
        const std::int64_t start = (std::int64_t) take * kTakeSpacing;
        REQUIRE (rm.startRecording (kSampleRate, start));
        grant.store (take * kBlocksField + kBlocksPerTake, std::memory_order_release);
        while (blocksDone[(size_t) take].load (std::memory_order_acquire) < 2)
            std::this_thread::yield();
        rm.stopRecording (start + kBlocksPerTake * kBlockSize);
        CHECK (rm.getLastRecordErrors().empty());
        grant.store (0, std::memory_order_release);
    }
    quit.store (true, std::memory_order_release);
    audioThread.join();

    for (int t = 0; t < kAudioTracks; ++t)
    {
        INFO ("track " << t + 1);
        const auto& regions = session.track (t).regions;
        REQUIRE (regions.size() == (size_t) kTakes);
        for (const auto& region : regions)
        {
            CHECK (region.lengthInSamples >= 2 * kBlockSize);
            CHECK (region.lengthInSamples % kBlockSize == 0);
            CHECK (framesOnDisk (region) == region.lengthInSamples);
        }
    }
    const auto midiRegions = session.track (kMidiTrack).midiRegions.current();
    CHECK (midiRegions.size() == (size_t) kTakes);
    CHECK (findUnreferencedAudio (session).files.empty());
}
