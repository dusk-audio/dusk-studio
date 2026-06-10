#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-version-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

void writeRaw (const juce::File& target, const juce::String& contents)
{
    target.deleteFile();
    target.create();
    target.replaceWithText (contents);
}
} // namespace

// Format-version contract:
//   * Manual save writes "version": kFormatVersion (currently 1).
//   * Load rejects sessions whose version is HIGHER than the build's
//     kFormatVersion — newer Dusk Studio can read older sessions (via the
//     migrateSession switch) but older Dusk Studio must refuse newer ones
//     rather than silent-drop new fields the build doesn't understand.
//   * Missing "version" key (pre-versioning saves) is treated as v1 so
//     those files run through every migration step once one exists.

TEST_CASE ("SessionSerializer rejects future-version sessions",
           "[session][serializer][version]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    writeRaw (target, R"({"version":9999,"tempo":120.0})");

    Session s;
    REQUIRE_FALSE (SessionSerializer::load (s, target));

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer accepts session with missing version (legacy)",
           "[session][serializer][version]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    // No "version" property — emulates a pre-versioning save (Dusk Studio
    // wrote files without the key before the format-version contract
    // landed). Must succeed and parse the rest of the document.
    writeRaw (target, R"({"tempo":98.5,"tracks":[{"name":"Legacy"}]})");

    Session s;
    REQUIRE (SessionSerializer::load (s, target));

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer round-trip preserves version field",
           "[session][serializer][version]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    REQUIRE (SessionSerializer::save (a, target));

    // Manual JSON parse — the version field is implementation-internal
    // (no public getter on Session) so we read straight from disk.
    auto root = juce::JSON::parse (target);
    REQUIRE (root.isObject());
    REQUIRE (root.hasProperty ("version"));
    REQUIRE ((int) root["version"] >= 1);

    dir.deleteRecursively();
}
