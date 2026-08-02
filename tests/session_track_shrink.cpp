#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using namespace duskstudio;

// Loading a session must REPLACE the model, not merge into it. A session.json
// with fewer tracks than this build (hand-edited, or written by a tool like the
// DP importer) used to leave the surplus track slots holding the previously-open
// session's content — ghost regions / MIDI / automation / plugin that still
// played back. load() now drives every slot through the restore (an absent slot
// gets blanked), so this pins that.
TEST_CASE ("loading a session with fewer tracks blanks the surplus slots",
           "[session][serializer][paths]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-shrink-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    // RAII cleanup so a failing REQUIRE below never leaves the temp dir behind.
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    Session s;
    s.setSessionDirectory (dir);

    // Emulate a larger session already open: track 5 carries every kind of
    // ghostable content.
    {
        AudioRegion r;
        r.file            = s.getAudioDirectory().getChildFile ("ghost.wav");
        r.lengthInSamples = 1000;
        s.track (5).regions.push_back (r);

        s.track (5).pluginLegacyDescriptionXml = "<PLUGIN/>";
        s.track (5).pluginStateBase64    = "ABCD";

        s.track (5).midiRegions.publish (
            std::make_unique<std::vector<MidiRegion>> (1));

        s.track (5).automationLanes[0].publishPoints (
            std::vector<AutomationPoint> (1));
    }

    // A session.json that only describes two tracks.
    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"tracks":[{"name":"A"},{"name":"B"}]})");

    REQUIRE (SessionSerializer::load (s, target));

    // Slot 5 is not in the JSON, so it must come back blank — no ghosts.
    REQUIRE (s.track (5).regions.empty());
    REQUIRE (s.track (5).midiRegions.current().empty());
    REQUIRE_FALSE (s.track (5).pluginDescriptor.has_value());
    REQUIRE (s.track (5).pluginLegacyDescriptionXml.isEmpty());
    REQUIRE (s.track (5).pluginStateBase64.isEmpty());
    REQUIRE (s.track (5).automationLanes[0].pointsConst().empty());
}

// The mixer half of the same contract. Every setter in restoreTrack is
// conditional on its key being present, so driving a surplus slot from an empty
// object cleared the regions but left the previous session's name, colour,
// fader, sends and hardware routing on the strip.
TEST_CASE ("loading a session with fewer tracks blanks the surplus mixer state",
           "[session][serializer][paths]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-shrink-mixer-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    Session s;
    s.setSessionDirectory (dir);

    const auto defaultName   = s.track (5).name;
    const auto defaultColour = s.track (5).colour;

    auto& ghost = s.track (5);
    ghost.name = "Ghost";
    ghost.colour = juce::Colours::red;
    ghost.strip.faderDb.store (-18.0f);
    ghost.strip.pan.store (0.75f);
    ghost.strip.mute.store (true);
    ghost.strip.busAssign[2].store (true);
    ghost.strip.auxSendDb[1].store (-12.0f);
    ghost.strip.auxSendPreFader[1].store (true);
    ghost.strip.hpfFreq.store (120.0f);
    ghost.strip.lpfFreq.store (8000.0f);
    ghost.hardwareInsert.enabled.store (true);
    ghost.hardwareInsert.outputGainDb.store (6.0f);
    ghost.hardwareInsert.dryWet.store (0.5f);

    const auto target = dir.getChildFile ("session.json");
    REQUIRE (target.replaceWithText (R"({"version":3,"tracks":[{"name":"A"},{"name":"B"}]})"));

    REQUIRE (SessionSerializer::load (s, target));

    const auto& t = s.track (5);
    REQUIRE (t.name == defaultName);
    REQUIRE (t.colour == defaultColour);
    REQUIRE (t.strip.faderDb.load() == 0.0f);
    REQUIRE (t.strip.pan.load() == 0.0f);
    REQUIRE_FALSE (t.strip.mute.load());
    REQUIRE_FALSE (t.strip.busAssign[2].load());
    REQUIRE (t.strip.auxSendDb[1].load() == ChannelStripParams::kAuxSendOffDb);
    REQUIRE_FALSE (t.strip.auxSendPreFader[1].load());
    REQUIRE (t.strip.hpfFreq.load() == ChannelStripParams::kHpfOffHz);
    REQUIRE (t.strip.lpfFreq.load() == ChannelStripParams::kLpfOffHz);
    REQUIRE_FALSE (t.hardwareInsert.enabled.load());
    REQUIRE (t.hardwareInsert.outputGainDb.load() == 0.0f);
    REQUIRE (t.hardwareInsert.dryWet.load() == 1.0f);
}
