#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

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
        CHECK (root["version"].get<int>() == 9);

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
    CHECK (resaved["version"].get<int>() == 9);
    for (const auto& track : resaved["tracks"])
        for (const auto& region : track["regions"])
            CHECK_FALSE (region.contains ("previous_takes"));
    CHECK (resaved["tracks"][1]["midi_regions"][0]["previous_takes"].size() == 1);

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

TEST_CASE ("Loading a v8 session starts no take before the timeline and keeps it under its regions",
           "[session][serializer][migration][takes]")
{
    // One pass split in two, its right half moved near the start: where that
    // half puts the file, the pass would begin 3000 samples before zero.
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
    CHECK (saved[0]["timeline_start"].get<std::int64_t>() == 0);
    CHECK (saved[0]["source_offset"].get<std::int64_t>() == 3000);
    CHECK (saved[0]["length"].get<std::int64_t>() == 9000);

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
    const auto& moved = track.regions[1];
    CHECK (moved.takeId == take.id);
    CHECK (take.timelineStart + (moved.sourceOffset - take.sourceOffset) == moved.timelineStart);
    CHECK (take.timelineStart + take.lengthInSamples == 9000);
    dir.deleteRecursively();
}

TEST_CASE ("Loading gives zero and duplicate take ids fresh ones", "[session][serializer][takes]")
{
    const Json root {
        { "version", 9 },
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
        { "version", 9 },
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
    writeJson (target, Json { { "version", 9 },
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
