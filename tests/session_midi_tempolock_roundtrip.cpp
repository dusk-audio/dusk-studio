#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using Catch::Matchers::WithinAbs;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-midi-tempolock-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// MidiRegion gained tempoLock + recordedAtBPM in the Phase-4 BPM-retime
// work. The fields are spec'd in DuskStudio.md §5b and only matter once
// the user changes BPM after recording. A round-trip miss here means a
// post-load BPM change retimes the wrong direction (or not at all),
// silently destroying the take's musical timing.
TEST_CASE ("SessionSerializer round-trips MidiRegion tempoLock + recordedAtBPM",
           "[session][serializer][midi]")
{
    using duskstudio::Session;
    using duskstudio::MidiRegion;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.tempoBpm.store (96.0f, std::memory_order_relaxed);

    // Two regions on the same track to verify per-region (not per-track)
    // independence of the new fields.
    {
        auto& v = a.track (0).midiRegions.currentMutable();
        v.clear();

        MidiRegion locked;
        locked.timelineStart   = 48000;
        locked.lengthInSamples = 96000;
        locked.lengthInTicks   = 960;
        locked.tempoLock       = true;
        locked.recordedAtBPM   = 96.0;
        v.push_back (std::move (locked));

        MidiRegion floating;
        floating.timelineStart   = 240000;
        floating.lengthInSamples = 48000;
        floating.lengthInTicks   = 480;
        floating.tempoLock       = false;     // explicitly unlocked
        floating.recordedAtBPM   = 132.5;     // mismatch with session bpm by design
        v.push_back (std::move (floating));
    }

    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));

    const auto& reloaded = b.track (0).midiRegions.current();
    REQUIRE (reloaded.size() == 2);

    // Region 0: tempoLock default (true) + matching recordedAtBPM.
    REQUIRE (reloaded[0].tempoLock == true);
    REQUIRE_THAT (reloaded[0].recordedAtBPM, WithinAbs (96.0, 1e-6));

    // Region 1: explicitly unlocked + recordedAtBPM that differs from session.
    REQUIRE (reloaded[1].tempoLock == false);
    REQUIRE_THAT (reloaded[1].recordedAtBPM, WithinAbs (132.5, 1e-6));
}

// Legacy session (saved before tempoLock landed) loads with the spec
// defaults: locked = true, recordedAtBPM = the session's saved tempo.
// Verifies the migration path described in restoreTrack's MidiRegion
// parser - so a Patreon user opening a v0.0.1 session after this change
// doesn't see their MIDI regions secretly anchored to 120 BPM when the
// session was actually 137.
TEST_CASE ("SessionSerializer legacy MidiRegion (no tempo_lock key) anchors to session BPM",
           "[session][serializer][midi]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    // Hand-rolled legacy session JSON. Only the minimum fields the parser
    // requires; everything else relies on struct defaults. The midi_regions
    // array intentionally omits both tempo_lock and recorded_at_bpm so the
    // load path exercises the absent-field defaults.
    const juce::String legacy =
        R"({
            "version": 1,
            "transport": { "tempo_bpm": 84.5 },
            "tracks": [
                {
                    "name": "Lead Synth",
                    "midi_regions": [
                        {
                            "timeline_start": 0,
                            "length_samples": 60000,
                            "length_ticks": 480
                        }
                    ]
                }
            ]
        })";

    REQUIRE (target.replaceWithText (legacy));

    Session s;
    REQUIRE (SessionSerializer::load (s, target));

    const auto& v = s.track (0).midiRegions.current();
    REQUIRE (v.size() == 1);
    REQUIRE (v[0].tempoLock == true);
    REQUIRE_THAT (v[0].recordedAtBPM, WithinAbs (84.5, 1e-6));
}
