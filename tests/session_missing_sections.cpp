#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

// A truncated / hand-edited session.json can lack whole section keys
// ("tracks", "buses", "aux_lanes"). Loading such a file over a populated
// session used to skip those sections entirely, leaving the previous
// session's regions, plugins and mixer state alive under the new session's
// name. load() now substitutes serialized defaults for any missing section.
TEST_CASE ("loading a session without section keys resets those sections",
           "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-missing-sections-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    Session s;
    s.setSessionDirectory (dir);

    // Populate every ghostable surface.
    {
        AudioRegion r;
        r.file            = s.getAudioDirectory().getChildFile ("ghost.wav");
        r.lengthInSamples = 1000;
        s.track (3).regions.push_back (r);
        s.track (3).pluginDescriptionXml = "<PLUGIN/>";
        s.track (3).pluginStateBase64    = "ABCD";

        s.bus (1).strip.faderDb.store (-12.0f);
        s.bus (1).strip.compEnabled.store (true);

        s.auxLane (0).pluginDescriptionXml[0] = "<PLUGIN/>";
        s.auxLane (0).pluginStateBase64[0]    = "ABCD";
        s.auxLane (0).nativeClapPath[0]       = "/tmp/ghost.clap";
        s.auxLane (0).params.returnLevelDb.store (-6.0f);
    }

    const auto target = dir.getChildFile ("session.json");

    SECTION ("all section keys absent")
    {
        target.replaceWithText (R"({"version":3})");
    }
    SECTION ("section keys present but not arrays")
    {
        target.replaceWithText (R"({"version":3,"tracks":42,"buses":"x","aux_lanes":{}})");
    }
    SECTION ("section arrays shorter than the model")
    {
        target.replaceWithText (R"({"version":3,"tracks":[],"buses":[],"aux_lanes":[]})");
    }

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE (s.track (3).regions.empty());
    REQUIRE (s.track (3).pluginDescriptionXml.isEmpty());
    REQUIRE (s.track (3).pluginStateBase64.isEmpty());

    REQUIRE_THAT (s.bus (1).strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_FALSE (s.bus (1).strip.compEnabled.load());

    REQUIRE (s.auxLane (0).pluginDescriptionXml[0].isEmpty());
    REQUIRE (s.auxLane (0).pluginStateBase64[0].isEmpty());
    REQUIRE (s.auxLane (0).nativeClapPath[0].isEmpty());
    REQUIRE_THAT (s.auxLane (0).params.returnLevelDb.load(), WithinAbs (0.0f, 1e-6f));
}
