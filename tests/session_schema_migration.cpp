// H1 schema migration test. Exercises SessionSerializer::migrateSession
// directly (forward-declared below — non-static in the .cpp + lives in
// namespace duskstudio so tests can reach it without touching the
// header) AND end-to-end via SessionSerializer::load on a v1-tagged
// session JSON. Confirms:
//   1. migrateSession returns false + does not advance when asked to
//      migrate from an unknown lower version (safety branch).
//   2. migrateSession advances v1 → kFormatVersion (currently v2) on a
//      well-formed root object, and the "version" property on `root` is
//      bumped to match.
//   3. End-to-end: a v1-tagged session.json on disk loads cleanly,
//      Session deserialises, the round-trip save writes the current
//      kFormatVersion back out.
//
// These tests are the regression net for future kFormatVersion bumps:
// every migrator case added must keep test (2) green for its specific
// from-version and test (3) green for round-trip.

#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace duskstudio
{
// Forward-declare the in-cpp non-static migrator. The cpp drops `static`
// from this symbol specifically so tests can reach it without a header
// change (SessionSerializer.h stays untouched for source-compat).
bool migrateSession (juce::var& root, int from);
} // namespace duskstudio

namespace
{
juce::File makeTempMigrationDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-migration-"
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

TEST_CASE ("migrateSession safety branch: refuses unknown lower version",
           "[session][serializer][migration]")
{
    // Mock pre-versioning (v0) — no migrator case registered. The loop's
    // default branch must return false rather than spin forever.
    juce::DynamicObject::Ptr obj (new juce::DynamicObject());
    obj->setProperty ("tempo", 120.0);
    juce::var root (obj.get());

    const bool ok = duskstudio::migrateSession (root, 0);
    REQUIRE_FALSE (ok);

    // Root must be untouched on the safety-branch path — caller relies
    // on this to know the document is unsalvageable.
    REQUIRE (root.isObject());
    REQUIRE_FALSE (root.hasProperty ("version"));
}

TEST_CASE ("migrateSession advances a mock v1 root to the current schema",
           "[session][serializer][migration]")
{
    // Mock v1 root with a stable field the migrator must preserve.
    juce::DynamicObject::Ptr obj (new juce::DynamicObject());
    obj->setProperty ("version", 1);
    obj->setProperty ("tempo", 98.5);
    juce::var root (obj.get());

    const bool ok = duskstudio::migrateSession (root, 1);
    REQUIRE (ok);

    // version field must now match the current build's kFormatVersion.
    // We don't reach kFormatVersion symbolically from the test (it's
    // in an anonymous namespace inside the .cpp), so we check the
    // post-migrate value is at least 2 + the original payload survived.
    REQUIRE (root.isObject());
    REQUIRE (root.hasProperty ("version"));
    REQUIRE ((int) root["version"] >= 2);
    REQUIRE (root.hasProperty ("tempo"));
    REQUIRE ((double) root["tempo"] == 98.5);
}

TEST_CASE ("SessionSerializer loads a v1-tagged session file end-to-end",
           "[session][serializer][migration]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempMigrationDir();
    const auto target = dir.getChildFile ("session.json");

    // Hand-written v1 session — minimum surface to exercise the load +
    // migrate path. The loader sees fileVersion < kFormatVersion and
    // routes through migrateSession before Session deserialisation.
    writeRaw (target, R"({"version":1,"tempo":124.0,"tracks":[{"name":"v1-track"}]})");

    Session s;
    REQUIRE (SessionSerializer::load (s, target));

    // Save back + verify the file is now tagged with the current
    // kFormatVersion. Without re-reading SessionSerializer's
    // kFormatVersion constant directly, we settle for ">= 2" (the
    // version this migrator case targets).
    REQUIRE (SessionSerializer::save (s, target));
    auto root = juce::JSON::parse (target);
    REQUIRE (root.isObject());
    REQUIRE (root.hasProperty ("version"));
    REQUIRE ((int) root["version"] >= 2);

    dir.deleteRecursively();
}
