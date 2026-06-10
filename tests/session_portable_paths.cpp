#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir (const char* tag)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile (juce::String ("dusk-studio-paths-") + tag + "-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

TEST_CASE ("region files inside the session dir round-trip as relative paths",
           "[session][serializer][paths]")
{
    using namespace duskstudio;

    const auto dir = makeTempSessionDir ("rel");
    Session a;
    a.setSessionDirectory (dir);

    const auto wav = a.getAudioDirectory().getChildFile ("track01_take.wav");
    wav.replaceWithText ("not a real wav");

    AudioRegion r;
    r.file            = wav;
    r.lengthInSamples = 1000;
    a.track (0).regions.push_back (r);

    const auto target = dir.getChildFile ("session.json");
    REQUIRE (SessionSerializer::save (a, target));

    // The stored path must not leak the machine-specific prefix, and the
    // separator must be the canonical '/' on every OS (Windows'
    // getRelativePathFrom hands back backslashes).
    const auto json = target.loadFileAsString();
    REQUIRE (json.contains ("audio/track01_take.wav"));
    REQUIRE_FALSE (json.contains ("audio\\track01_take.wav"));
    REQUIRE_FALSE (json.contains (dir.getFullPathName() + "/audio"));

    Session b;
    b.setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.track (0).regions.size() == 1);
    REQUIRE (b.track (0).regions[0].file == wav);
    REQUIRE (b.missingAudioFilesAfterLoad.empty());

    dir.deleteRecursively();
}

TEST_CASE ("stale absolute region path re-roots to the session's audio dir",
           "[session][serializer][paths]")
{
    using namespace duskstudio;

    const auto dir = makeTempSessionDir ("reroot");
    Session s;
    s.setSessionDirectory (dir);

    const auto wav = s.getAudioDirectory().getChildFile ("track02_take.wav");
    wav.replaceWithText ("not a real wav");

    // Emulates a session folder copied from another machine: the stored
    // absolute path doesn't exist here, but the WAV travelled with the
    // folder.
    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":1,"tracks":[{"name":"T","regions":[)"
        R"({"file":"/home/elsewhere/sessions/old/audio/track02_take.wav",)"
        R"("timeline_start":0,"length":1000,"source_offset":0}]}]})");

    REQUIRE (SessionSerializer::load (s, target));
    REQUIRE (s.track (0).regions.size() == 1);
    REQUIRE (s.track (0).regions[0].file == wav);
    REQUIRE (s.missingAudioFilesAfterLoad.empty());

    dir.deleteRecursively();
}

TEST_CASE ("backslash-relative path from a Windows save resolves on load",
           "[session][serializer][paths]")
{
    using namespace duskstudio;

    const auto dir = makeTempSessionDir ("backslash");
    Session s;
    s.setSessionDirectory (dir);

    const auto wav = s.getAudioDirectory().getChildFile ("track04_take.wav");
    wav.replaceWithText ("not a real wav");

    // Emulates a session saved by a Windows build before separators were
    // canonicalised: the relative path uses '\\'.
    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":1,"tracks":[{"name":"T","regions":[)"
        R"({"file":"audio\\track04_take.wav",)"
        R"("timeline_start":0,"length":1000,"source_offset":0}]}]})");

    REQUIRE (SessionSerializer::load (s, target));
    REQUIRE (s.track (0).regions.size() == 1);
    REQUIRE (s.track (0).regions[0].file == wav);
    REQUIRE (s.missingAudioFilesAfterLoad.empty());

    dir.deleteRecursively();
}

TEST_CASE ("unresolvable region path is reported as missing",
           "[session][serializer][paths]")
{
    using namespace duskstudio;

    const auto dir = makeTempSessionDir ("missing");
    Session s;
    s.setSessionDirectory (dir);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":1,"tracks":[{"name":"T","regions":[)"
        R"({"file":"/gone/forever/track03_take.wav",)"
        R"("timeline_start":0,"length":1000,"source_offset":0}]}]})");

    REQUIRE (SessionSerializer::load (s, target));
    REQUIRE (s.missingAudioFilesAfterLoad.size() == 1);
    REQUIRE (s.missingAudioFilesAfterLoad[0] == "/gone/forever/track03_take.wav");

    dir.deleteRecursively();
}
