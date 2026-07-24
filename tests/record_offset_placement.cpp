#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <vector>

using namespace duskstudio;

// The manual recording latency offset is subtracted from a committed AUDIO
// take's timelineStart (clamped >= 0) so a round-trip-delayed input lands in
// time. It must NOT move the MIDI commit, which has no converter latency.

namespace
{
struct ScopedDir
{
    juce::File d;
    ~ScopedDir() { d.deleteRecursively(); }
};

juce::File makeTempDir (const juce::String& tag)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile (tag + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

void writeAudio (RecordManager& rm, int numBlocks, int blockSize)
{
    std::vector<float> block ((size_t) blockSize, 0.1f);
    for (int b = 0; b < numBlocks; ++b)
        rm.writeInputBlock (0, block.data(), nullptr, blockSize);
}
} // namespace

TEST_CASE ("Latency offset shifts committed audio start earlier",
           "[recording][recordmanager][offset]")
{
    constexpr double      kSampleRate = 48000.0;
    constexpr int         kBlockSize  = 256;
    constexpr int         kNumBlocks  = 8;
    constexpr juce::int64 kStart      = 10000;
    constexpr int         kOffset     = 500;
    constexpr juce::int64 kTotal      = kStart + kNumBlocks * kBlockSize;

    const auto dir = makeTempDir ("dusk-offset-audio-");
    const ScopedDir scoped { dir };

    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, kStart, kOffset));
    writeAudio (rm, kNumBlocks, kBlockSize);
    rm.stopRecording (kTotal);

    const auto& regs = session.track (0).regions;
    REQUIRE (regs.size() == 1);
    REQUIRE (regs[0].timelineStart == kStart - kOffset);
    REQUIRE (regs[0].lengthInSamples == kNumBlocks * kBlockSize);
}

TEST_CASE ("Latency offset larger than start clamps audio to zero and trims head",
           "[recording][recordmanager][offset]")
{
    constexpr double      kSampleRate = 48000.0;
    constexpr int         kBlockSize  = 256;
    constexpr int         kNumBlocks  = 4;
    constexpr juce::int64 kStart      = 100;
    constexpr int         kOffset     = 600;
    constexpr juce::int64 kFrames     = kNumBlocks * kBlockSize;  // 1024
    constexpr juce::int64 kTrim       = kOffset - kStart;         // 500
    constexpr juce::int64 kTotal      = kStart + kFrames;

    const auto dir = makeTempDir ("dusk-offset-clamp-");
    const ScopedDir scoped { dir };

    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, kStart, kOffset));
    writeAudio (rm, kNumBlocks, kBlockSize);
    rm.stopRecording (kTotal);

    const auto& regs = session.track (0).regions;
    REQUIRE (regs.size() == 1);
    REQUIRE (regs[0].timelineStart == 0);
    REQUIRE (regs[0].sourceOffset == kTrim);
    REQUIRE (regs[0].lengthInSamples == kFrames - kTrim);
}

TEST_CASE ("Latency offset exceeding take length commits no region",
           "[recording][recordmanager][offset]")
{
    constexpr double      kSampleRate = 48000.0;
    constexpr int         kBlockSize  = 256;
    constexpr int         kNumBlocks  = 2;
    constexpr juce::int64 kStart      = 100;
    constexpr int         kOffset     = 5000;  // trim 4900 > frames 512
    constexpr juce::int64 kTotal      = kStart + kNumBlocks * kBlockSize;

    const auto dir = makeTempDir ("dusk-offset-empty-");
    const ScopedDir scoped { dir };

    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, kStart, kOffset));
    writeAudio (rm, kNumBlocks, kBlockSize);
    rm.stopRecording (kTotal);

    REQUIRE (session.track (0).regions.empty());

    const auto& errs = rm.getLastRecordErrors();
    REQUIRE (errs.size() == 1);
    REQUIRE (errs[0].trackIndex == 0);
    REQUIRE (errs[0].kind == RecordManager::RecordErrorKind::OffsetConsumedTake);
    REQUIRE (errs[0].count == (std::uint64_t) (kNumBlocks * kBlockSize));
}

TEST_CASE ("Latency offset does not move MIDI region placement",
           "[recording][recordmanager][offset][midi]")
{
    constexpr double      kSampleRate = 48000.0;
    constexpr juce::int64 kStart      = 2000;
    constexpr int         kOffset     = 500;
    constexpr juce::int64 kTotal      = kStart + 48000;

    const auto dir = makeTempDir ("dusk-offset-midi-");
    const ScopedDir scoped { dir };

    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, kStart, kOffset));

    juce::MidiBuffer block;
    block.addEvent (juce::MidiMessage::noteOn  (1, 64, (juce::uint8) 100), 0);
    block.addEvent (juce::MidiMessage::noteOff (1, 64), 200);
    rm.writeMidiBlock (0, block, 0);

    rm.stopRecording (kTotal);

    const auto after = session.track (0).midiRegions.current();
    REQUIRE (after.size() == 1);
    REQUIRE (after[0].timelineStart == kStart);
}
