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

    Session s;
    s.setSessionDirectory (dir);

    // Emulate a larger session already open: track 5 carries every kind of
    // ghostable content.
    {
        AudioRegion r;
        r.file            = s.getAudioDirectory().getChildFile ("ghost.wav");
        r.lengthInSamples = 1000;
        s.track (5).regions.push_back (r);

        s.track (5).pluginDescriptionXml = "<PLUGIN/>";
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
    REQUIRE (s.track (5).pluginDescriptionXml.isEmpty());
    REQUIRE (s.track (5).pluginStateBase64.isEmpty());
    REQUIRE (s.track (5).automationLanes[0].pointsConst().empty());

    dir.deleteRecursively();
}
