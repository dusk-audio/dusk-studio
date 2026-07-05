#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>

using namespace duskstudio;

// A MIDI overdub whose range fully contains exactly one existing region
// absorbs it into previousTakes and pushes the new region — the region COUNT
// is unchanged. The commit diff used to compare sizes only, so this take
// produced no TrackCommitDiff and could not be undone.
TEST_CASE ("MIDI overdub replacing one region still emits a commit diff",
           "[recording][recordmanager][midi]")
{
    constexpr double      kSampleRate = 48000.0;
    constexpr juce::int64 kTotal      = 48000;

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-midi-overdub-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    // Existing take, fully inside the coming overdub's [0, kTotal) range.
    {
        MidiRegion existing;
        existing.timelineStart   = 1000;
        existing.lengthInSamples = 8000;
        existing.lengthInTicks   = 960;
        MidiNote n;
        n.noteNumber    = 60;
        n.velocity      = 100;
        n.lengthInTicks = 480;
        existing.notes.push_back (n);
        session.track (0).midiRegions.publish (
            std::make_unique<std::vector<MidiRegion>> (1, existing));
    }

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));

    juce::MidiBuffer block;
    block.addEvent (juce::MidiMessage::noteOn  (1, 64, (juce::uint8) 100), 0);
    block.addEvent (juce::MidiMessage::noteOff (1, 64), 200);
    rm.writeMidiBlock (0, block, 0);

    rm.stopRecording (kTotal);

    // The overdub absorbed the old region: count is still 1, history moved.
    const auto after = session.track (0).midiRegions.current();
    REQUIRE (after.size() == 1);
    REQUIRE (after[0].previousTakes.size() == 1);

    const auto& diff = rm.getLastCommitDiff();
    REQUIRE (diff.size() == 1);
    REQUIRE (diff[0].trackIndex == 0);
    REQUIRE (diff[0].midiBefore.size() == 1);
    REQUIRE (diff[0].midiAfter.size() == 1);
}
