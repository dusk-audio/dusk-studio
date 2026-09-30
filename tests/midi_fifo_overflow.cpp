#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-midi-overflow-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// Regression guard for A1.1: PerTrackMidi has a 65536-event FIFO. When
// it fills, writeMidiBlock bumps an atomic overflow counter; stopRecording
// latches that into RecordManager::getLastRecordErrors so the UI can
// surface a "MIDI events dropped" alert. Without this counter the
// failure mode is silent data loss on busy controller streams.

TEST_CASE ("RecordManager surfaces MIDI FIFO overflow at stopRecording",
           "[recording][recordmanager][midi]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kFifoCap    = 65536;
    constexpr int    kOverflowBy = 1024;  // push 1k past the cap

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));

    // One big dusk MIDI buffer with kFifoCap + kOverflowBy events. Channel 1,
    // note 60, varying velocity so the buffer doesn't dedupe. Sample
    // positions are spaced 1 sample apart so every event is retained.
    dusk::MidiBuffer big;
    const int totalEvents = kFifoCap + kOverflowBy;
    for (int i = 0; i < totalEvents; ++i)
    {
        const std::uint8_t vel = std::uint8_t (1 + (i & 0x7E));
        const std::uint8_t bytes[] { 0x90, 60, vel };
        big.addEvent (bytes, 3, i);
    }

    // CONTRACT BYPASS: writeMidiBlock is documented audio-thread-only
    // (uses active.load(acquire) for cross-thread sync). Calling from
    // the test thread is safe here because no AudioEngine is attached
    // — the active flag's acquire/release pair serializes against
    // nothing. If RecordManager ever adds a real audio-thread assert,
    // expose a writeMidiBlockForTesting shim instead of weakening it.
    rm.writeMidiBlock (0, big, /*blockStartFromRecord*/ 0);

    rm.stopRecording (totalEvents);

    const auto& errs = rm.getLastRecordErrors();
    bool foundOverflow = false;
    juce::uint64 droppedCount = 0;
    for (const auto& e : errs)
    {
        if (e.kind == RecordManager::RecordErrorKind::MidiOverflow
            && e.trackIndex == 0)
        {
            foundOverflow = true;
            droppedCount  = e.count;
            break;
        }
    }
    REQUIRE (foundOverflow);
    // We pushed kOverflowBy past cap; counter should reflect at least
    // that many drops. Exact figure depends on AbstractFifo's free-space
    // accounting between calls — we assert the floor, not equality.
    REQUIRE (droppedCount >= (juce::uint64) kOverflowBy);

    dir.deleteRecursively();
}

TEST_CASE ("RecordManager clean MIDI take leaves overflow list empty",
           "[recording][recordmanager][midi]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (48000.0, 0));

    dusk::MidiBuffer small;
    for (int i = 0; i < 100; ++i)
    {
        const std::uint8_t bytes[] { 0x90, 60, 100 };
        small.addEvent (bytes, 3, i);
    }
    rm.writeMidiBlock (0, small, 0);

    rm.stopRecording (100);

    for (const auto& e : rm.getLastRecordErrors())
        REQUIRE_FALSE (e.kind == RecordManager::RecordErrorKind::MidiOverflow);

    dir.deleteRecursively();
}

TEST_CASE ("The recording errors alert advises on free space only when a write failed",
           "[recording][recordmanager][errors]")
{
    using duskstudio::RecordManager;
    using Kind = RecordManager::RecordErrorKind;

    SECTION ("errors that are not about the disk get no disk advice")
    {
        const auto text = RecordManager::describeRecordErrors ({ { 0, Kind::LoopPassLimit, 3 },
                                                                 { 4, Kind::MidiOverflow, 12 },
                                                                 { 6, Kind::OffsetConsumedTake, 80 } });
        CHECK (text.find ("\n    Track 1 - loop passes not recorded (past 1,024 in one take) (3)\n") != std::string::npos);
        CHECK (text.find ("\n    Track 5 - MIDI events dropped (capture buffer full) (12)\n") != std::string::npos);
        const std::string last = "\n    Track 7 - take discarded (recording offset exceeds its length) (80)";
        REQUIRE (text.size() > last.size());
        CHECK (text.compare (text.size() - last.size(), last.size(), last) == 0);
        CHECK (text.find ("free space") == std::string::npos);
        CHECK (text.find ("I/O details") == std::string::npos);
    }

    SECTION ("a write failure among them brings the disk advice")
    {
        const auto text = RecordManager::describeRecordErrors ({ { 0, Kind::LoopPassLimit, 3 },
                                                                 { 1, Kind::WavWrite, 4096 } });
        CHECK (text.find ("\n    Track 2 - WAV write failed (disk full / I/O error) (4096)\n") != std::string::npos);
        const std::string advice = "\n\nCheck the session's audio folder for free space and the "
                                   "session log for I/O details before continuing.";
        REQUIRE (text.size() > advice.size());
        CHECK (text.compare (text.size() - advice.size(), advice.size(), advice) == 0);
    }
}
