#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"
#include "session/TakeComp.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio
{
bool migrateSession (nlohmann::json& root, int from);
} // namespace duskstudio

using namespace duskstudio;
using Json = nlohmann::json;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-takes-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

void writeJson (const juce::File& target, const Json& root)
{
    target.deleteFile();
    target.replaceWithText (juce::String (root.dump()));
}

Json readJson (const juce::File& source)
{
    return Json::parse (source.loadFileAsString().toStdString(), nullptr, false);
}

Json withoutKey (Json object, const char* key)
{
    object.erase (key);
    return object;
}

// Recorded in format 8: one region carrying two displaced passes, one of them
// the same file on another loop pass, a region split in two whose right half
// was moved earlier, an imported region, a MIDI track with its own history, and
// a second audio track.
Json v8Session()
{
    return Json {
        { "version", 8 },
        { "tracks", Json::array ({
            { { "name", "Vox" },
              { "regions", Json::array ({
                  { { "file", "audio/pass1.wav" }, { "timeline_start", 48000 }, { "length", 24000 },
                    { "source_offset", 12000 }, { "num_channels", 2 }, { "gain_db", -2.0 },
                    { "fade_in", 480 }, { "label", "Verse" },
                    { "take_provenance", { { "captured_at_ms", 1000 }, { "loop_pass", 1 } } },
                    { "previous_takes", Json::array ({
                        { { "file", "audio/pass1.wav" }, { "source_offset", 0 }, { "length", 36000 },
                          { "take_provenance", { { "captured_at_ms", 900 }, { "loop_pass", 2 } } } },
                        { { "file", "audio/pass0.wav" }, { "source_offset", 500 }, { "length", 10000 } } }) } },
                  { { "file", "audio/split.wav" }, { "timeline_start", 100000 }, { "length", 5000 },
                    { "source_offset", 0 },
                    { "take_provenance", { { "captured_at_ms", 2000 }, { "partial", true } } } },
                  { { "file", "audio/split.wav" }, { "timeline_start", 104000 }, { "length", 7000 },
                    { "source_offset", 5000 },
                    { "take_provenance", { { "captured_at_ms", 2000 }, { "partial", true } } } },
                  { { "file", "audio/imported.wav" }, { "timeline_start", 0 }, { "length", 1000 },
                    { "source_offset", 0 } } }) } },
            { { "name", "Keys" },
              { "regions", Json::array() },
              { "midi_regions", Json::array ({
                  { { "timeline_start", 0 }, { "length_samples", 24000 }, { "length_ticks", 960 },
                    { "notes", Json::array ({ { { "ch", 1 }, { "note", 60 }, { "vel", 100 },
                                                { "start", 0 }, { "len", 240 } } }) },
                    { "take_provenance", { { "captured_at_ms", 3000 } } },
                    { "previous_takes", Json::array ({
                        { { "length_ticks", 480 },
                          { "notes", Json::array ({ { { "ch", 1 }, { "note", 64 }, { "vel", 90 },
                                                      { "start", 0 }, { "len", 120 } } }) },
                          { "take_provenance", { { "captured_at_ms", 2500 }, { "loop_pass", 3 } } } } }) } } }) } },
            { { "name", "Gtr" },
              { "regions", Json::array ({
                  { { "file", "audio/gtr.wav" }, { "timeline_start", 9600 }, { "length", 4800 },
                    { "source_offset", 0 },
                    { "take_provenance", { { "captured_at_ms", 4000 } } } } }) } } }) }
    };
}

struct ExpectedTake
{
    TakeId id;
    const char* name;
    const char* file;
    std::int64_t timelineStart, length, sourceOffset;
    int numChannels;
    TakeProvenance provenance;
};

void checkTake (const AudioTake& take, const ExpectedTake& expected)
{
    INFO (expected.name << " on " << expected.file);
    CHECK (take.id == expected.id);
    CHECK (take.name == expected.name);
    CHECK (take.file.getFileName() == juce::String (expected.file).fromLastOccurrenceOf ("/", false, false));
    CHECK (take.timelineStart == expected.timelineStart);
    CHECK (take.lengthInSamples == expected.length);
    CHECK (take.sourceOffset == expected.sourceOffset);
    CHECK (take.numChannels == expected.numChannels);
    CHECK (take.provenance.capturedAtMs == expected.provenance.capturedAtMs);
    CHECK (take.provenance.loopPassOrdinal == expected.provenance.loopPassOrdinal);
    CHECK (take.provenance.partialPass == expected.provenance.partialPass);
}
} // namespace

TEST_CASE ("SessionSerializer round-trips track takes and the regions that name them",
           "[session][serializer][takes]")
{
    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    auto saved = std::make_unique<Session>();
    saved->setSessionDirectory (dir);
    auto& track = saved->track (5);

    AudioTake first;
    first.id = saved->allocateTakeId();
    first.name = "Take 1";
    first.file = dir.getChildFile ("audio/pass_a.wav");
    first.timelineStart = 48000;
    first.lengthInSamples = 96000;
    first.sourceOffset = 0;
    first.provenance = { 1111, 0, false };

    AudioTake second;
    second.id = saved->allocateTakeId();
    second.name = "Scratch vocal";
    second.file = dir.getChildFile ("audio/pass_b.wav");
    second.timelineStart = 12000;
    second.lengthInSamples = 50000;
    second.sourceOffset = 2400;
    second.numChannels = 2;
    second.provenance = { 2222, 3, true };

    AudioTake third;
    third.id = saved->allocateTakeId();
    third.name = "Take 3";
    third.file = dir.getChildFile ("audio/pass_c.wav");
    third.timelineStart = 0;
    third.lengthInSamples = 7000;
    third.sourceOffset = 300;

    track.takes = { first, second, third };

    AudioRegion fromSecond;
    fromSecond.file = second.file;
    fromSecond.timelineStart = 20000;
    fromSecond.lengthInSamples = 10000;
    fromSecond.sourceOffset = 10400;
    fromSecond.numChannels = 2;
    fromSecond.takeId = second.id;
    AudioRegion alsoFromSecond = fromSecond;
    alsoFromSecond.timelineStart = 40000;
    alsoFromSecond.sourceOffset = 30400;
    AudioRegion fromThird;
    fromThird.file = third.file;
    fromThird.timelineStart = 0;
    fromThird.lengthInSamples = 7000;
    fromThird.sourceOffset = 300;
    fromThird.takeId = third.id;
    AudioRegion imported;
    imported.file = dir.getChildFile ("audio/loop.wav");
    imported.lengthInSamples = 4800;
    track.regions = { fromThird, fromSecond, alsoFromSecond, imported };

    REQUIRE (SessionSerializer::save (*saved, target));

    const auto root = readJson (target);
    REQUIRE (root.is_object());
    const auto& savedTrack = root["tracks"][5];
    REQUIRE (savedTrack["takes"].size() == 3);
    CHECK_FALSE (savedTrack["takes"][0].contains ("num_channels"));
    CHECK (savedTrack["takes"][1]["num_channels"].get<int>() == 2);
    CHECK_FALSE (savedTrack["takes"][2].contains ("take_provenance"));
    CHECK (savedTrack["takes"][1]["file"].get<std::string>() == "audio/pass_b.wav");
    CHECK_FALSE (savedTrack["regions"][3].contains ("take_id"));
    CHECK_FALSE (root["tracks"][4].contains ("takes"));

    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));

    const auto& takes = loaded->track (5).takes;
    REQUIRE (takes.size() == 3);
    for (size_t i = 0; i < takes.size(); ++i)
    {
        const auto& want = track.takes[i];
        const auto& got = takes[i];
        INFO ("take " << i);
        CHECK (got.id == want.id);
        CHECK (got.name == want.name);
        CHECK (got.file == want.file);
        CHECK (got.timelineStart == want.timelineStart);
        CHECK (got.lengthInSamples == want.lengthInSamples);
        CHECK (got.sourceOffset == want.sourceOffset);
        CHECK (got.numChannels == want.numChannels);
        CHECK (got.provenance.capturedAtMs == want.provenance.capturedAtMs);
        CHECK (got.provenance.loopPassOrdinal == want.provenance.loopPassOrdinal);
        CHECK (got.provenance.partialPass == want.provenance.partialPass);
    }

    const auto& regions = loaded->track (5).regions;
    REQUIRE (regions.size() == 4);
    CHECK (regions[0].takeId == third.id);
    CHECK (regions[1].takeId == second.id);
    CHECK (regions[2].takeId == second.id);
    CHECK (regions[3].takeId == 0);
    CHECK (loaded->allocateTakeId() == third.id + 1);

    dir.deleteRecursively();
}

TEST_CASE ("Loading a v8 session turns audio take history into track takes",
           "[session][serializer][migration][takes]")
{
    const auto original = v8Session();

    SECTION ("the migrated tree")
    {
        auto root = original;
        REQUIRE (migrateSession (root, 8));
        CHECK (root["version"].get<int>() == 10);

        // Playback parity: a region gains take_id and loses previous_takes,
        // and nothing else about it moves.
        for (size_t t = 0; t < original["tracks"].size(); ++t)
        {
            const auto& before = original["tracks"][t]["regions"];
            const auto& after = root["tracks"][t]["regions"];
            REQUIRE (after.size() == before.size());
            for (size_t r = 0; r < before.size(); ++r)
            {
                INFO ("track " << t << " region " << r);
                CHECK_FALSE (after[r].contains ("previous_takes"));
                CHECK (withoutKey (after[r], "take_id") == withoutKey (before[r], "previous_takes"));
            }
        }
        CHECK (root["tracks"][1]["midi_regions"] == original["tracks"][1]["midi_regions"]);
        CHECK_FALSE (root["tracks"][1].contains ("takes"));
    }

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, original);

    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));

    // Oldest first: the pass with no capture time sat at the bottom of the
    // history, and the other two follow their capture times.
    const auto& vox = loaded->track (0);
    REQUIRE (vox.takes.size() == 4);
    checkTake (vox.takes[0], { 1, "Take 1", "audio/pass0.wav", 48000, 10000, 500,   2, {} });
    checkTake (vox.takes[1], { 2, "Take 2", "audio/pass1.wav", 48000, 36000, 0,     2, { 900, 2, false } });
    checkTake (vox.takes[2], { 3, "Take 3", "audio/pass1.wav", 48000, 24000, 12000, 2, { 1000, 1, false } });
    // The split pair is one pass: the union of both halves, placed where the
    // earlier of the two puts the file.
    checkTake (vox.takes[3], { 4, "Take 4", "audio/split.wav", 99000, 12000, 0,     1, { 2000, 0, true } });

    REQUIRE (vox.regions.size() == 4);
    CHECK (vox.regions[0].takeId == 3);
    CHECK (vox.regions[1].takeId == 4);
    CHECK (vox.regions[2].takeId == 4);
    CHECK (vox.regions[3].takeId == 0);
    CHECK (vox.regions[0].timelineStart == 48000);
    CHECK (vox.regions[0].lengthInSamples == 24000);
    CHECK (vox.regions[0].sourceOffset == 12000);
    CHECK (vox.regions[0].numChannels == 2);
    CHECK (vox.regions[0].fadeInSamples == 480);
    CHECK (vox.regions[0].label == "Verse");
    CHECK (vox.regions[2].timelineStart == 104000);
    CHECK (vox.regions[2].sourceOffset == 5000);

    const auto& gtr = loaded->track (2);
    REQUIRE (gtr.takes.size() == 1);
    checkTake (gtr.takes[0], { 5, "Take 1", "audio/gtr.wav", 9600, 4800, 0, 1, { 4000, 0, false } });
    REQUIRE (gtr.regions.size() == 1);
    CHECK (gtr.regions[0].takeId == 5);

    const auto& keys = loaded->track (1).midiRegions.current();
    REQUIRE (keys.size() == 1);
    REQUIRE (keys[0].previousTakes.size() == 1);
    CHECK (keys[0].previousTakes[0].lengthInTicks == 480);
    CHECK (keys[0].previousTakes[0].provenance.loopPassOrdinal == 3);
    CHECK (loaded->track (1).takes.empty());

    CHECK (loaded->allocateTakeId() == 6);

    REQUIRE (SessionSerializer::save (*loaded, target));
    const auto resaved = readJson (target);
    CHECK (resaved["version"].get<int>() == 10);
    for (const auto& track : resaved["tracks"])
        for (const auto& region : track["regions"])
            CHECK_FALSE (region.contains ("previous_takes"));
    CHECK (resaved["tracks"][1]["midi_regions"][0]["previous_takes"].size() == 1);

    dir.deleteRecursively();
}

TEST_CASE ("Loading a v9 session turns audio take history into track takes",
           "[session][serializer][migration][takes]")
{
    // Format 9 stamped the LV2 state tags a moved track keeps; its regions still
    // carried their take history the way format 8 did.
    auto original = v8Session();
    original["version"] = 9;
    original["tracks"][0]["lv2_state_tag"] = "track18";

    auto root = original;
    REQUIRE (migrateSession (root, 9));
    CHECK (root["version"].get<int>() == 10);
    CHECK (root["tracks"][0]["lv2_state_tag"] == "track18");
    for (const auto& region : root["tracks"][0]["regions"])
        CHECK_FALSE (region.contains ("previous_takes"));

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, original);

    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));

    const auto& vox = loaded->track (0);
    REQUIRE (vox.takes.size() == 4);
    checkTake (vox.takes[0], { 1, "Take 1", "audio/pass0.wav", 48000, 10000, 500,   2, {} });
    checkTake (vox.takes[2], { 3, "Take 3", "audio/pass1.wav", 48000, 24000, 12000, 2, { 1000, 1, false } });
    REQUIRE (vox.regions.size() == 4);
    CHECK (vox.regions[0].takeId == 3);
    REQUIRE (loaded->track (2).takes.size() == 1);
    CHECK (loaded->track (2).regions[0].takeId == 5);

    dir.deleteRecursively();
}

TEST_CASE ("Loading a v8 session numbers each track's takes oldest first",
           "[session][serializer][migration][takes]")
{
    const auto pass = [] (const char* file, std::int64_t offset, std::int64_t capturedAtMs, int loopPass)
    {
        Json take { { "file", file }, { "source_offset", offset }, { "length", 500 } };
        if (capturedAtMs != 0)
            take["take_provenance"] = { { "captured_at_ms", capturedAtMs }, { "loop_pass", loopPass } };
        return take;
    };
    const auto region = [] (Json live, Json history)
    {
        live["timeline_start"] = 4800;
        live["previous_takes"] = std::move (history);
        return Json { { "regions", Json::array ({ std::move (live) }) } };
    };

    // The v8 history lists the newest displaced take first. The passes of one
    // loop share the capture time.
    const Json root {
        { "version", 8 },
        { "tracks", Json::array ({
            region (pass ("audio/loop.wav", 1000, 5000, 3),
                    Json::array ({ pass ("audio/loop.wav", 500, 5000, 2), pass ("audio/loop.wav", 0, 5000, 1) })),
            region (pass ("audio/after.wav", 0, 0, 0),
                    Json::array ({ pass ("audio/loop2.wav", 500, 6000, 2), pass ("audio/loop2.wav", 0, 6000, 1) })),
            region (pass ("audio/loop3.wav", 500, 7000, 2),
                    Json::array ({ pass ("audio/loop3.wav", 0, 7000, 1), pass ("audio/before.wav", 0, 0, 0) })) }) }
    };

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, root);
    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));

    const auto& loop = loaded->track (0);
    REQUIRE (loop.takes.size() == 3);
    checkTake (loop.takes[0], { 1, "Take 1", "audio/loop.wav", 4800, 500, 0,    1, { 5000, 1, false } });
    checkTake (loop.takes[1], { 2, "Take 2", "audio/loop.wav", 4800, 500, 500,  1, { 5000, 2, false } });
    checkTake (loop.takes[2], { 3, "Take 3", "audio/loop.wav", 4800, 500, 1000, 1, { 5000, 3, false } });
    REQUIRE (loop.regions.size() == 1);
    CHECK (loop.regions[0].takeId == 3);

    // A plain take recorded over a loop.
    const auto& after = loaded->track (1);
    REQUIRE (after.takes.size() == 3);
    checkTake (after.takes[0], { 4, "Take 1", "audio/loop2.wav", 4800, 500, 0,   1, { 6000, 1, false } });
    checkTake (after.takes[1], { 5, "Take 2", "audio/loop2.wav", 4800, 500, 500, 1, { 6000, 2, false } });
    checkTake (after.takes[2], { 6, "Take 3", "audio/after.wav", 4800, 500, 0,   1, {} });
    REQUIRE (after.regions.size() == 1);
    CHECK (after.regions[0].takeId == 6);

    // A loop recorded over a plain take.
    const auto& before = loaded->track (2);
    REQUIRE (before.takes.size() == 3);
    checkTake (before.takes[0], { 7, "Take 1", "audio/before.wav", 4800, 500, 0,   1, {} });
    checkTake (before.takes[1], { 8, "Take 2", "audio/loop3.wav",  4800, 500, 0,   1, { 7000, 1, false } });
    checkTake (before.takes[2], { 9, "Take 3", "audio/loop3.wav",  4800, 500, 500, 1, { 7000, 2, false } });
    REQUIRE (before.regions.size() == 1);
    CHECK (before.regions[0].takeId == 9);

    CHECK (loaded->allocateTakeId() == 10);
    dir.deleteRecursively();
}

TEST_CASE ("Loading a v8 session starts no take before the timeline and keeps it whole",
           "[session][serializer][migration][takes]")
{
    // One pass split in two, its right half moved near the start: where that
    // half puts the file, the pass would begin 3000 samples before zero, so it
    // sits where the left half puts it instead.
    const Json root {
        { "version", 8 },
        { "tracks", Json::array ({
            { { "regions", Json::array ({
                  { { "file", "audio/split.wav" }, { "timeline_start", 100000 }, { "length", 5000 },
                    { "source_offset", 0 }, { "take_provenance", { { "captured_at_ms", 2000 } } } },
                  { { "file", "audio/split.wav" }, { "timeline_start", 2000 }, { "length", 7000 },
                    { "source_offset", 5000 }, { "take_provenance", { { "captured_at_ms", 2000 } } } } }) } } }) }
    };

    auto migrated = root;
    REQUIRE (migrateSession (migrated, 8));
    const auto& saved = migrated["tracks"][0]["takes"];
    REQUIRE (saved.size() == 1);
    CHECK (saved[0]["timeline_start"].get<std::int64_t>() == 100000);
    CHECK (saved[0]["source_offset"].get<std::int64_t>() == 0);
    CHECK (saved[0]["length"].get<std::int64_t>() == 12000);

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, root);
    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));

    const auto& track = loaded->track (0);
    REQUIRE (track.takes.size() == 1);
    REQUIRE (track.regions.size() == 2);
    const auto& take = track.takes[0];
    const auto& left = track.regions[0];
    CHECK (left.takeId == take.id);
    CHECK (take.timelineStart + (left.sourceOffset - take.sourceOffset) == left.timelineStart);

    // The moved half still names the take, counted at the part of it its audio
    // comes from.
    const auto& moved = track.regions[1];
    CHECK (moved.takeId == take.id);
    CHECK (moved.timelineStart == 2000);
    CHECK (takeCoverage (track, take.id) == std::vector<std::pair<std::int64_t, std::int64_t>> { { 100000, 112000 } });

    auto& carved = loaded->track (0);
    detail::carveRegions (*loaded, carved, 101000, 102000);
    const auto holdsCutAudio = std::any_of (carved.takes.begin(), carved.takes.end(), [] (const AudioTake& t)
    {
        return t.file.getFileName() == "split.wav" && t.sourceOffset <= 1000
            && t.sourceOffset + t.lengthInSamples >= 2000;
    });
    CHECK (holdsCutAudio);
    for (const auto& region : carved.regions)
    {
        INFO ("region at " << region.timelineStart);
        CHECK (findTake (carved, region.takeId) != nullptr);
    }
    dir.deleteRecursively();
}

TEST_CASE ("Loading a v8 session keeps take audio only a region's history holds",
           "[session][serializer][migration][takes]")
{
    // Recorded in 0.14: C at 48000, then A over it, A split at 96000, A's right
    // half moved to 10000, then B exactly over A's left half. A's left half now
    // lives only in B's history, and where the moved half puts A's file it
    // would begin 38000 samples before zero.
    const auto piece = [] (const char* file, std::int64_t offset, std::int64_t length, std::int64_t capturedAtMs)
    {
        return Json { { "file", file }, { "source_offset", offset }, { "length", length },
                      { "take_provenance", { { "captured_at_ms", capturedAtMs } } } };
    };
    auto overLeft = piece ("audio/b.wav", 0, 48000, 3000);
    overLeft["timeline_start"] = 48000;
    overLeft["previous_takes"] = Json::array ({ piece ("audio/a.wav", 0, 48000, 2000),
                                                piece ("audio/c.wav", 0, 48000, 1000) });
    auto movedRight = piece ("audio/a.wav", 48000, 96000, 2000);
    movedRight["timeline_start"] = 10000;
    movedRight["previous_takes"] = Json::array ({ piece ("audio/c.wav", 48000, 96000, 1000) });
    const Json root {
        { "version", 8 },
        { "tracks", Json::array ({ { { "regions", Json::array ({ overLeft, movedRight }) } } }) }
    };

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, root);
    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));

    const auto& track = loaded->track (0);
    const auto takeOf = [&track] (const char* name) -> const AudioTake*
    {
        for (const auto& take : track.takes)
            if (take.file.getFileName() == name) return &take;
        return nullptr;
    };
    for (const auto& take : track.takes)
        CHECK (take.timelineStart >= 0);

    const struct { const char* file; std::int64_t end; } reached[] {
        { "a.wav", 144000 }, { "b.wav", 48000 }, { "c.wav", 144000 } };
    for (const auto& span : reached)
    {
        INFO (span.file);
        const auto* take = takeOf (span.file);
        REQUIRE (take != nullptr);
        CHECK (take->sourceOffset == 0);
        CHECK (take->sourceOffset + take->lengthInSamples == span.end);
    }

    // Choosing A over B's span plays what cycling B back played in 0.14.
    const auto* a = takeOf ("a.wav");
    REQUIRE (a != nullptr);
    const auto underB = regionFromTake (*a, 48000, 96000);
    REQUIRE (underB.has_value());
    CHECK (underB->sourceOffset == 0);
    CHECK (underB->lengthInSamples == 48000);

    REQUIRE (track.regions.size() == 2);
    CHECK (track.regions[0].takeId == takeOf ("b.wav")->id);
    CHECK (track.regions[1].takeId == a->id);
    CHECK (track.regions[1].timelineStart == 10000);
    CHECK (track.regions[1].sourceOffset == 48000);
    dir.deleteRecursively();
}

TEST_CASE ("Loading gives zero and duplicate take ids fresh ones", "[session][serializer][takes]")
{
    const Json root {
        { "version", 10 },
        { "tracks", Json::array ({
            { { "takes", Json::array ({
                  { { "id", 7 }, { "file", "audio/a.wav" }, { "length", 100 } },
                  { { "id", 0 }, { "file", "audio/b.wav" }, { "length", 100 } },
                  { { "id", 7 }, { "file", "audio/c.wav" }, { "length", 100 } } }) },
              { "regions", Json::array ({
                  { { "file", "audio/a.wav" }, { "length", 100 }, { "take_id", 7 } },
                  { { "file", "audio/b.wav" }, { "length", 100 } } }) } },
            { { "takes", Json::array ({
                  { { "id", 7 }, { "file", "audio/d.wav" }, { "length", 100 } },
                  { { "id", 2 }, { "file", "audio/e.wav" }, { "length", 100 } } }) },
              { "regions", Json::array ({
                  { { "file", "audio/d.wav" }, { "length", 100 }, { "take_id", 7 } },
                  { { "file", "audio/e.wav" }, { "length", 100 }, { "take_id", 2 } } }) } } }) }
    };

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, root);

    for (int pass = 0; pass < 2; ++pass)
    {
        INFO ("load " << pass);
        auto loaded = std::make_unique<Session>();
        loaded->setSessionDirectory (dir);
        REQUIRE (SessionSerializer::load (*loaded, target));

        const auto& first = loaded->track (0);
        REQUIRE (first.takes.size() == 3);
        CHECK (first.takes[0].id == 7);
        CHECK (first.takes[1].id == 8);
        CHECK (first.takes[2].id == 9);
        CHECK (first.takes[2].file.getFileName() == "c.wav");
        CHECK (first.regions[0].takeId == 7);
        CHECK (first.regions[1].takeId == 0);

        // The same id on another track is another take: that track's regions
        // follow it to its fresh id.
        const auto& second = loaded->track (1);
        REQUIRE (second.takes.size() == 2);
        CHECK (second.takes[0].id == 10);
        CHECK (second.takes[1].id == 2);
        CHECK (second.regions[0].takeId == 10);
        CHECK (second.regions[1].takeId == 2);

        CHECK (loaded->allocateTakeId() == 11);
    }

    dir.deleteRecursively();
}

TEST_CASE ("A region naming a take its track does not hold names none after load",
           "[session][serializer][takes]")
{
    const Json root {
        { "version", 10 },
        { "tracks", Json::array ({
            { { "takes", Json::array ({ { { "id", 4 }, { "file", "audio/a.wav" }, { "length", 100 } } }) },
              { "regions", Json::array ({
                  { { "file", "audio/a.wav" }, { "length", 100 }, { "take_id", 4 } },
                  { { "file", "audio/a.wav" }, { "length", 100 }, { "take_id", 5 } },
                  { { "file", "audio/a.wav" }, { "length", 100 }, { "take_id", -3 } } }) } },
            { { "regions", Json::array ({
                  { { "file", "audio/a.wav" }, { "length", 100 }, { "take_id", 4 } } }) } } }) }
    };

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, root);

    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));
    const auto& regions = loaded->track (0).regions;
    REQUIRE (regions.size() == 3);
    CHECK (regions[0].takeId == 4);
    CHECK (regions[1].takeId == 0);
    CHECK (regions[2].takeId == 0);
    REQUIRE (loaded->track (1).regions.size() == 1);
    CHECK (loaded->track (1).regions[0].takeId == 0);

    dir.deleteRecursively();
}

TEST_CASE ("Loading a session drops the takes the previous one held",
           "[session][serializer][takes]")
{
    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    writeJson (target, Json { { "version", 10 },
                              { "tracks", Json::array ({ { { "takes", Json::array ({
                                  { { "id", 41 }, { "file", "audio/a.wav" }, { "length", 100 } } }) } } }) } });

    auto session = std::make_unique<Session>();
    session->setSessionDirectory (dir);
    for (int i = 0; i < 3; ++i)
        session->track (3).takes.push_back ({ session->allocateTakeId(), "Take", {}, 0, 100, 0, 1, {} });
    REQUIRE (SessionSerializer::load (*session, target));
    CHECK (session->track (3).takes.empty());
    REQUIRE (session->track (0).takes.size() == 1);
    CHECK (session->allocateTakeId() == 42);

    dir.deleteRecursively();
}

TEST_CASE ("Reversing a reversed region maps it back onto the span it reversed",
           "[session][takes][reverse]")
{
    const auto source = juce::File ("/audio/source.wav");
    const auto render = juce::File ("/takes/source-reversed.wav");

    // A render of source samples [1000, 1600), played whole, with a short fade in
    // on the render's head (the source's tail).
    AudioRegion reversed;
    reversed.file = render;
    reversed.timelineStart = 48000;
    reversed.sourceOffset = 0;
    reversed.lengthInSamples = 600;
    reversed.fadeInSamples = 40;
    reversed.fadeInShape = FadeShape::EqualPower;
    reversed.reversedFrom = AudioRegion::ReverseSource { render, source, 1000, 600, 7 };

    SECTION ("the whole render")
    {
        const auto forward = forwardOfReversed (reversed);
        REQUIRE (forward.has_value());
        CHECK (forward->file == source);
        CHECK (forward->sourceOffset == 1000);
        CHECK (forward->lengthInSamples == 600);
        CHECK (forward->timelineStart == 48000);
        CHECK (forward->takeId == 7);
        CHECK_FALSE (forward->reversedFrom.has_value());
        CHECK (forward->fadeInSamples == 0);
        CHECK (forward->fadeOutSamples == 40);
        CHECK (forward->fadeOutShape == FadeShape::EqualPower);
    }
    SECTION ("a trimmed or split part of it")
    {
        // Render samples [100, 250) are source samples [1350, 1500).
        auto part = reversed;
        part.sourceOffset = 100;
        part.lengthInSamples = 150;
        const auto forward = forwardOfReversed (part);
        REQUIRE (forward.has_value());
        CHECK (forward->sourceOffset == 1350);
        CHECK (forward->lengthInSamples == 150);
    }
    SECTION ("a region no longer playing the render, or reaching past it")
    {
        auto other = reversed;
        other.file = juce::File ("/takes/normalized.wav");
        CHECK_FALSE (forwardOfReversed (other).has_value());
        auto past = reversed;
        past.sourceOffset = 500;
        past.lengthInSamples = 200;
        CHECK_FALSE (forwardOfReversed (past).has_value());
        auto plain = reversed;
        plain.reversedFrom.reset();
        CHECK_FALSE (forwardOfReversed (plain).has_value());
    }
}

TEST_CASE ("SessionSerializer round-trips what a reversed region reversed",
           "[session][serializer][takes][reverse]")
{
    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    const auto source = dir.getChildFile ("audio").getChildFile ("source.wav");
    const auto render = dir.getChildFile ("takes").getChildFile ("source-reversed.wav");

    auto saved = std::make_unique<Session>();
    saved->setSessionDirectory (dir);
    AudioTake take;
    take.id = saved->allocateTakeId();
    take.file = dir.getChildFile ("audio").getChildFile ("take.wav");
    take.lengthInSamples = 4800;
    saved->track (2).takes.push_back (take);
    AudioRegion reversed;
    reversed.file = render;
    reversed.lengthInSamples = 600;
    reversed.reversedFrom = AudioRegion::ReverseSource { render, source, 1000, 600, take.id };
    saved->track (2).regions.push_back (reversed);
    REQUIRE (SessionSerializer::save (*saved, target));

    // An earlier track holds a take under the same id, so this one gets a fresh
    // id on load, and the reverse follows it there.
    auto root = readJson (target);
    root["tracks"][2]["takes"][0]["id"] = 30;
    root["tracks"][2]["regions"][0]["reversed_from"]["take_id"] = 30;
    root["tracks"][1]["takes"] = Json::array ({ { { "id", 30 }, { "file", "audio/other.wav" }, { "length", 10 } } });
    writeJson (target, root);

    auto loaded = std::make_unique<Session>();
    loaded->setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (*loaded, target));
    REQUIRE (loaded->track (2).regions.size() == 1);
    const auto& from = loaded->track (2).regions[0].reversedFrom;
    REQUIRE (from.has_value());
    CHECK (from->render == render);
    CHECK (from->file == source);
    CHECK (from->sourceOffset == 1000);
    CHECK (from->lengthInSamples == 600);
    REQUIRE (loaded->track (2).takes.size() == 1);
    CHECK (loaded->track (2).takes[0].id != 30);
    CHECK (from->takeId == loaded->track (2).takes[0].id);
    // The render the region plays is missing; the source it would play reversed
    // again is not reported, as nothing plays it now.
    const auto& missing = loaded->missingAudioFilesAfterLoad;
    CHECK (std::any_of (missing.begin(), missing.end(),
                        [] (const juce::String& path) { return path.endsWith ("source-reversed.wav"); }));
    CHECK_FALSE (std::any_of (missing.begin(), missing.end(),
                              [] (const juce::String& path) { return path.endsWith ("source.wav"); }));

    dir.deleteRecursively();
}

TEST_CASE ("The takes covering a span are listed newest first, whole cover only",
           "[session][takes]")
{
    Track track;
    track.takes.push_back ({ 1, "Take 1", {}, 0, 96000, 0, 1, {} });
    track.takes.push_back ({ 2, "Take 2", {}, 0, 96000, 0, 1, {} });
    track.takes.push_back ({ 3, "Punch", {}, 24000, 48000, 0, 1, {} });

    CHECK (takesCovering (track, 0, 96000) == std::vector<TakeId> { 2, 1 });
    CHECK (takesCovering (track, 30000, 60000) == std::vector<TakeId> { 3, 2, 1 });
    CHECK (takesCovering (track, 20000, 30000) == std::vector<TakeId> { 2, 1 });
    CHECK (takesCovering (track, 24000, 72000) == std::vector<TakeId> { 3, 2, 1 });
    CHECK (takesCovering (track, 5000, 5000).empty());
}

TEST_CASE ("Stepping a comp section's take moves one lane at a time and stops at the ends",
           "[session][takes]")
{
    const std::vector<TakeId> lanes { 3, 2, 1 };
    CHECK (steppedTake (lanes, 3, 1) == 2);
    CHECK (steppedTake (lanes, 2, 1) == 1);
    CHECK (steppedTake (lanes, 1, 1) == 0);
    CHECK (steppedTake (lanes, 2, -1) == 3);
    CHECK (steppedTake (lanes, 3, -1) == 0);
    // A section playing no take on the list steps onto the nearest end.
    CHECK (steppedTake (lanes, 0, 1) == 3);
    CHECK (steppedTake (lanes, 9, -1) == 1);
    CHECK (steppedTake ({}, 3, 1) == 0);
}

namespace
{
// Take 1 plays up to a seam at 24000, crossfading over 64 samples into take 2, which
// plays on to 96000.
void fillSeamTrack (Track& track)
{
    track.takes.push_back ({ 1, "Take 1", {}, 0, 96000, 0, 1, {} });
    track.takes.push_back ({ 2, "Take 2", {}, 0, 96000, 0, 1, {} });
    AudioRegion left;
    left.takeId = 1;
    left.lengthInSamples = 24000 + kPunchFadeSamples;
    left.fadeOutSamples = kPunchFadeSamples;
    AudioRegion right;
    right.takeId = 2;
    right.timelineStart = right.sourceOffset = 24000;
    right.lengthInSamples = 72000;
    right.fadeInSamples = kPunchFadeSamples;
    track.regions = { left, right };
}
} // namespace

TEST_CASE ("A comp seam is found where one take's region crossfades into the next",
           "[session][takes][seam]")
{
    Track track;
    fillSeamTrack (track);
    const auto seam = compSeamNear (track, 24030, 10);
    REQUIRE (seam.has_value());
    CHECK (seam->left == 0);
    CHECK (seam->right == 1);
    CHECK (compSeamNear (track, 23990, 10).has_value());
    CHECK_FALSE (compSeamNear (track, 30000, 100).has_value());

    SECTION ("regions that name no take meet at no seam")
    {
        track.regions[0].takeId = 0;
        CHECK_FALSE (compSeamNear (track, 24030, 10).has_value());
    }
    SECTION ("regions apart by more than a seam fade meet at no seam")
    {
        track.regions[0].lengthInSamples = 24000 + 2 * kPunchFadeSamples;
        CHECK_FALSE (compSeamNear (track, 24030, 10).has_value());
    }
}

TEST_CASE ("Moving a comp seam moves both edges and stops where either region runs out",
           "[session][takes][seam]")
{
    Track track;
    fillSeamTrack (track);
    const CompSeam seam { 0, 1 };

    shiftSeam (track, seam, 1000);
    CHECK (track.regions[0].lengthInSamples == 25000 + kPunchFadeSamples);
    CHECK (track.regions[1].timelineStart == 25000);
    CHECK (track.regions[1].sourceOffset == 25000);
    CHECK (track.regions[1].lengthInSamples == 71000);
    CHECK (track.regions[0].fadeOutSamples == kPunchFadeSamples);
    CHECK (track.regions[1].fadeInSamples == kPunchFadeSamples);

    // Later, the right region keeps its fade and a sample; earlier, the left one does.
    CHECK (clampSeamShift (track, seam, 1'000'000) == 71000 - kPunchFadeSamples - 1);
    CHECK (clampSeamShift (track, seam, -1'000'000) == kPunchFadeSamples + 1 - (25000 + kPunchFadeSamples));

    SECTION ("a take that starts later holds the seam back to its start")
    {
        track.takes[1].timelineStart = 20000;
        track.takes[1].sourceOffset = 0;
        track.regions[1].sourceOffset = 5000;
        CHECK (clampSeamShift (track, seam, -20000) == -5000);
    }
    SECTION ("a take that ends sooner holds the seam to its end")
    {
        track.takes[0].lengthInSamples = 26000;
        CHECK (clampSeamShift (track, seam, 5000) == 26000 - (25000 + kPunchFadeSamples));
    }
}

TEST_CASE ("A comp seam without fades still keeps its overlap inside both regions",
           "[session][takes][seam]")
{
    Track track;
    fillSeamTrack (track);
    track.regions[0].fadeOutSamples = 0;
    track.regions[1].fadeInSamples = 0;
    const CompSeam seam { 0, 1 };
    const auto overlap = kPunchFadeSamples;

    // Earlier, the left region keeps the overlap and a sample, so the right one
    // still starts after it.
    const auto earliest = clampSeamShift (track, seam, -1'000'000);
    CHECK (track.regions[0].lengthInSamples + earliest == overlap + 1);
    shiftSeam (track, seam, -1'000'000);
    CHECK (track.regions[1].timelineStart > track.regions[0].timelineStart);
    CHECK (track.regions[1].timelineStart >= 0);

    // Later, the right region keeps the overlap and a sample.
    const auto latest = clampSeamShift (track, seam, 1'000'000);
    CHECK (track.regions[1].lengthInSamples - latest == overlap + 1);
}

TEST_CASE ("The comp section under a point is the region playing there, or the gap around it",
           "[session][takes][comp]")
{
    Track track;
    fillSeamTrack (track);
    // Take 1 plays [0, 24064), take 2 [24000, 96000): the crossfade belongs to take 2.
    CHECK (compSectionAt (track, 100) == std::pair<std::int64_t, std::int64_t> { 0, 24000 + kPunchFadeSamples });
    CHECK (compSectionAt (track, 24030) == std::pair<std::int64_t, std::int64_t> { 24000, 96000 });
    CHECK (compSectionAt (track, 50000) == std::pair<std::int64_t, std::int64_t> { 24000, 96000 });

    // A hole between regions, and the open space past the last one.
    track.regions[1].timelineStart = track.regions[1].sourceOffset = 40000;
    track.regions[1].lengthInSamples = 20000;
    CHECK (compSectionAt (track, 30000) == std::pair<std::int64_t, std::int64_t> { 24000 + kPunchFadeSamples, 40000 });
    const auto after = compSectionAt (track, 70000);
    CHECK (after.first == 60000);
    CHECK (after.second == std::numeric_limits<std::int64_t>::max());
}
