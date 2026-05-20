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

    // One big MidiBuffer with kFifoCap + kOverflowBy events. Channel 1,
    // note 60, varying velocity so the buffer doesn't dedupe. Sample
    // positions are spaced 1 sample apart so JUCE keeps them.
    juce::MidiBuffer big;
    const int totalEvents = kFifoCap + kOverflowBy;
    for (int i = 0; i < totalEvents; ++i)
    {
        const juce::uint8 vel = juce::uint8 (1 + (i & 0x7E));
        big.addEvent (juce::MidiMessage::noteOn (1, (juce::uint8) 60, vel), i);
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

    juce::MidiBuffer small;
    for (int i = 0; i < 100; ++i)
        small.addEvent (juce::MidiMessage::noteOn (1, (juce::uint8) 60, (juce::uint8) 100), i);
    rm.writeMidiBlock (0, small, 0);

    rm.stopRecording (100);

    for (const auto& e : rm.getLastRecordErrors())
        REQUIRE_FALSE (e.kind == RecordManager::RecordErrorKind::MidiOverflow);

    dir.deleteRecursively();
}
