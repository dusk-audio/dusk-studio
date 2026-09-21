#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <memory>

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
//   * Manual save writes "version": kFormatVersion (currently 7).
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
    REQUIRE ((int) root["version"] == 7);

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer stamps version 7 on a session carrying built-ins",
           "[session][serializer][version][builtin]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    // The highest format the last released reader accepts. v0.13.2 has no
    // built-in model, ignores builtin_id / builtin_state, and rewrites
    // session.json from its own model on save, so a session holding built-ins
    // must carry a version that build refuses. Dropping the format back to
    // this number trips the check below.
    constexpr int kLastReleasedReaderMaxVersion = 6;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    auto savedPtr = std::make_unique<Session>();
    savedPtr->track (2).builtinUnitId      = "dusk.builtin.sunset";
    savedPtr->track (2).builtinStateBase64 = "dHJhY2stcGF0Y2g=";
    savedPtr->auxLane (1).builtinUnitId[0]      = "dusk.builtin.utility";
    savedPtr->auxLane (1).builtinStateBase64[0] = "YXV4LXBhdGNo";
    REQUIRE (SessionSerializer::save (*savedPtr, target));

    const auto root = nlohmann::json::parse (
        target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE (root.is_object());
    REQUIRE (root["version"].get<int>() == 7);
    REQUIRE (root["version"].get<int>() > kLastReleasedReaderMaxVersion);

    auto loadedPtr = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*loadedPtr, target));
    CHECK (loadedPtr->track (2).builtinUnitId == "dusk.builtin.sunset");
    CHECK (loadedPtr->track (2).builtinStateBase64 == "dHJhY2stcGF0Y2g=");
    CHECK (loadedPtr->auxLane (1).builtinUnitId[0] == "dusk.builtin.utility");
    CHECK (loadedPtr->auxLane (1).builtinStateBase64[0] == "YXV4LXBhdGNo");

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer refuses a v8 session before touching the live model",
           "[session][serializer][version]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    // One version above this build's format, carrying values the loader would
    // otherwise write into the live Session.
    writeRaw (target,
              R"({"version":8,"tempo":76.0,)"
              R"("tracks":[{"name":"From the future","builtin_id":"dusk.builtin.utility"}]})");

    auto livePtr = std::make_unique<Session>();
    Session& live = *livePtr;
    live.tempoBpm.store (131.0f, std::memory_order_relaxed);
    live.track (0).name = "Still mine";
    live.track (0).builtinUnitId = "dusk.builtin.sunset";
    live.missingAudioFilesAfterLoad.push_back ("keep-me.wav");

    REQUIRE_FALSE (SessionSerializer::load (live, target));

    CHECK_THAT (live.tempoBpm.load (std::memory_order_relaxed),
                Catch::Matchers::WithinAbs (131.0, 1e-6));
    CHECK (live.track (0).name == "Still mine");
    CHECK (live.track (0).builtinUnitId == "dusk.builtin.sunset");
    CHECK (live.missingAudioFilesAfterLoad.size() == 1);

    dir.deleteRecursively();
}
