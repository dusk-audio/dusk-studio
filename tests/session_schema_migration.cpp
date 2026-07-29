// H1 schema migration test. Exercises SessionSerializer::migrateSession
// directly (forward-declared below — non-static in the .cpp + lives in
// namespace duskstudio so tests can reach it without touching the
// header) AND end-to-end via SessionSerializer::load on a v1-tagged
// session JSON. Confirms:
//   1. migrateSession returns false + does not advance when asked to
//      migrate from an unknown lower version (safety branch).
//   2. migrateSession advances v1 → the current kFormatVersion on a
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
#include <nlohmann/json.hpp>
#include <memory>

namespace duskstudio
{
// Forward-declare the in-cpp non-static migrator. The cpp drops `static`
// from this symbol specifically so tests can reach it without a header
// change (SessionSerializer.h stays untouched for source-compat).
bool migrateSession (nlohmann::json& root, int from);
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
    nlohmann::json root = { { "tempo", 120.0 } };

    const bool ok = duskstudio::migrateSession (root, 0);
    REQUIRE_FALSE (ok);

    // Root must be untouched on the safety-branch path — caller relies
    // on this to know the document is unsalvageable.
    REQUIRE (root.is_object());
    REQUIRE_FALSE (root.contains ("version"));
}

TEST_CASE ("migrateSession advances a mock v1 root to the current schema",
           "[session][serializer][migration]")
{
    // Mock v1 root with a stable field the migrator must preserve.
    nlohmann::json root = { { "version", 1 }, { "tempo", 98.5 } };

    const bool ok = duskstudio::migrateSession (root, 1);
    REQUIRE (ok);

    // version field must now match the current build's kFormatVersion.
    // We don't reach kFormatVersion symbolically from the test (it's
    // in an anonymous namespace inside the .cpp), so we check the
    // H4 owns version 4; the original payload must survive every step.
    REQUIRE (root.is_object());
    REQUIRE (root.contains ("version"));
    REQUIRE (root["version"].get<int>() == 4);
    REQUIRE (root.contains ("tempo"));
    REQUIRE (root["tempo"].get<double>() == 98.5);
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
    // kFormatVersion.
    REQUIRE (SessionSerializer::save (s, target));
    auto root = nlohmann::json::parse (target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE (root.is_object());
    REQUIRE (root.contains ("version"));
    REQUIRE (root["version"].get<int>() == 4);

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer migrates a v3 legacy plugin reference to a v4 save",
           "[session][serializer][migration][plugin-descriptor]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempMigrationDir();
    const auto target = dir.getChildFile ("session.json");
    const std::string legacyXml =
        R"(<PLUGIN name="Legacy Synth" descriptiveName="Legacy Synth" format="VST3" file="/plugins/Legacy.vst3" uid="1234" manufacturer="Dusk" version="1.0" isInstrument="1"/>)";
    nlohmann::json root {
        { "version", 3 },
        { "tracks", nlohmann::json::array ({
            { { "name", "Legacy" },
              { "plugin_desc_xml", legacyXml },
              { "plugin_state", "bGVnYWN5LXN0YXRl" } }
        }) }
    };
    writeRaw (target, root.dump());

    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, target));
    CHECK_FALSE (session->track (0).pluginDescriptor.has_value());
    CHECK (session->track (0).pluginLegacyDescriptionXml.toStdString() == legacyXml);
    CHECK (session->track (0).pluginStateBase64 == "bGVnYWN5LXN0YXRl");

    REQUIRE (SessionSerializer::save (*session, target));
    const auto saved = nlohmann::json::parse (
        target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE (saved.is_object());
    CHECK (saved["version"].get<int>() == 4);
    CHECK (saved["tracks"][0]["plugin_desc_xml"].get<std::string>() == legacyXml);
    CHECK (saved["tracks"][0]["plugin_state"].get<std::string>()
           == "bGVnYWN5LXN0YXRl");

    dir.deleteRecursively();
}
