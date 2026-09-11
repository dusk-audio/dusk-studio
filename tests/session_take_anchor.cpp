#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

using namespace duskstudio;

namespace
{
juce::File named (const char* name)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (name);
}

// A retake at 3000 that swallowed take 1, recorded at 5000.
AudioRegion retakeOverLaterTake()
{
    AudioRegion region;
    region.file = named ("take2.wav");
    region.timelineStart = 3000;
    region.lengthInSamples = 9000;
    region.previousTakes = { { named ("take1.wav"), 0, 5000, {}, 2000 } };
    return region;
}

struct ScopedSessionDir
{
    juce::File dir;
    ~ScopedSessionDir() { dir.deleteRecursively(); }
};

// Claimed by creating it: ctest runs cases as parallel processes.
ScopedSessionDir makeSessionDir (const char* tag)
{
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto candidate = base / (std::string (tag)
                                       + std::to_string (juce::Random::getSystemRandom().nextInt()));
        std::error_code ec;
        if (std::filesystem::create_directory (candidate, ec))
            return { juce::File (juce::String (candidate.string())) };
    }
    FAIL ("could not create a session directory");
    return {};
}

nlohmann::json firstRegionTakes (const juce::File& sessionJson)
{
    const auto root = nlohmann::json::parse (sessionJson.loadFileAsString().toStdString());
    return root["tracks"][0]["regions"][0]["previous_takes"];
}
} // namespace

TEST_CASE ("Swapping in an anchored take moves the region to it and back",
           "[session][takes][anchor]")
{
    auto region = retakeOverLaterTake();

    swapAudioTakePayload (region, region.previousTakes[0]);
    CHECK (region.file.getFileName() == "take1.wav");
    CHECK (region.timelineStart == 5000);
    CHECK (region.lengthInSamples == 5000);
    REQUIRE (region.previousTakes.size() == 1);
    CHECK (region.previousTakes[0].file.getFileName() == "take2.wav");
    CHECK (region.previousTakes[0].timelineOffset == -2000);

    swapAudioTakePayload (region, region.previousTakes[0]);
    CHECK (region.file.getFileName() == "take2.wav");
    CHECK (region.timelineStart == 3000);
    CHECK (region.lengthInSamples == 9000);
    CHECK (region.previousTakes[0].timelineOffset == 2000);
}

TEST_CASE ("Cycling through anchored takes keeps every take at its place",
           "[session][takes][anchor]")
{
    auto region = retakeOverLaterTake();
    region.previousTakes.push_back ({ named ("take0.wav"), 100, 1000, {}, 4000 });

    // The badge cycle: the front take comes up, the displaced one goes last.
    const auto cycle = [&region]
    {
        auto next = std::move (region.previousTakes.front());
        region.previousTakes.erase (region.previousTakes.begin());
        swapAudioTakePayload (region, next);
        region.previousTakes.push_back (std::move (next));
    };

    cycle();
    CHECK (region.file.getFileName() == "take1.wav");
    CHECK (region.timelineStart == 5000);
    cycle();
    CHECK (region.file.getFileName() == "take0.wav");
    CHECK (region.timelineStart == 7000);
    CHECK (region.sourceOffset == 100);
    cycle();
    CHECK (region.file.getFileName() == "take2.wav");
    CHECK (region.timelineStart == 3000);
    CHECK (region.lengthInSamples == 9000);
}

TEST_CASE ("Moving only the top take's start leaves earlier takes in place",
           "[session][takes][anchor]")
{
    auto region = retakeOverLaterTake();
    moveRegionStartKeepingTakes (region, 3500);
    CHECK (region.timelineStart == 3500);
    CHECK (region.timelineStart + region.previousTakes[0].timelineOffset == 5000);
}

TEST_CASE ("A take moved left of the timeline origin comes back trimmed at zero",
           "[session][takes][anchor]")
{
    AudioRegion region;
    region.file = named ("current.wav");
    region.timelineStart = 1000;
    region.lengthInSamples = 4000;
    region.previousTakes = { { named ("early.wav"), 100, 5000, {}, -3000 } };

    swapAudioTakePayload (region, region.previousTakes[0]);
    CHECK (region.file.getFileName() == "early.wav");
    CHECK (region.timelineStart == 0);
    CHECK (region.sourceOffset == 100 + 2000);
    CHECK (region.lengthInSamples == 3000);
    CHECK (region.previousTakes[0].file.getFileName() == "current.wav");
    CHECK (region.previousTakes[0].timelineOffset == 1000);
}

TEST_CASE ("Delete on a MIDI take brings the older one back where it was",
           "[session][takes][anchor][midi]")
{
    MidiRegion region;
    region.timelineStart = 48000;
    region.lengthInTicks = 3840;
    region.notes = { { 1, 64, 100, 0, 240 } };

    MidiTakeRef older;
    older.lengthInTicks = 960;
    older.notes = { { 1, 60, 90, 0, 480 } };
    older.timelineOffset = 24000;
    region.previousTakes = { older };

    REQUIRE (popMidiTake (region));
    CHECK (region.timelineStart == 72000);
    CHECK (region.lengthInTicks == 960);
    REQUIRE (region.notes.size() == 1);
    CHECK (region.notes[0].noteNumber == 60);
}

TEST_CASE ("A tempo change keeps a locked MIDI region's earlier takes on their beat",
           "[session][takes][anchor][midi][tempo]")
{
    constexpr double sr = 48000.0;
    Session s;
    s.tempoBpm.store (120.0f, std::memory_order_relaxed);
    s.track (0).midiRegions.mutate ([] (std::vector<MidiRegion>& v)
    {
        MidiRegion locked;
        locked.timelineStart = 48000;
        locked.lengthInTicks = 480;
        locked.lengthInSamples = 24000;
        locked.tempoLock = true;
        MidiTakeRef older;
        older.lengthInTicks = 480;
        older.timelineOffset = 24000;
        locked.previousTakes = { older };
        v.push_back (std::move (locked));
    });

    applyTempoChange (s, 60.0f, sr);

    const auto& region = s.track (0).midiRegions.current().front();
    CHECK (region.timelineStart == 96000);
    CHECK (region.previousTakes.front().timelineOffset == 48000);
}

TEST_CASE ("Take anchors survive a save and reload", "[session][takes][anchor][serializer]")
{
    const auto temp = makeSessionDir ("dusk-take-anchor-");
    const auto target = temp.dir.getChildFile ("session.json");

    Session a;
    a.setSessionDirectory (temp.dir);
    auto swapped = retakeOverLaterTake();
    swapAudioTakePayload (swapped, swapped.previousTakes[0]);
    a.track (0).regions = { swapped };
    a.track (1).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    a.track (1).midiRegions.mutate ([] (std::vector<MidiRegion>& v)
    {
        MidiRegion midi;
        midi.timelineStart = 1000;
        midi.lengthInTicks = 480;
        midi.lengthInSamples = 24000;
        MidiTakeRef older;
        older.lengthInTicks = 240;
        older.timelineOffset = 700;
        midi.previousTakes = { older };
        v.push_back (std::move (midi));
    });
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    b.setSessionDirectory (temp.dir);
    REQUIRE (SessionSerializer::load (b, target));

    const auto& audio = b.track (0).regions.front();
    CHECK (audio.timelineStart == 5000);
    REQUIRE (audio.previousTakes.size() == 1);
    CHECK (audio.previousTakes[0].timelineOffset == -2000);
    const auto& midi = b.track (1).midiRegions.current().front();
    REQUIRE (midi.previousTakes.size() == 1);
    CHECK (midi.previousTakes[0].timelineOffset == 700);
}

TEST_CASE ("A take saved without an anchor loads at its region's start",
           "[session][takes][anchor][serializer]")
{
    const auto temp = makeSessionDir ("dusk-take-unanchored-");
    const auto target = temp.dir.getChildFile ("session.json");

    Session a;
    a.setSessionDirectory (temp.dir);
    AudioRegion region;
    region.file = temp.dir.getChildFile ("audio").getChildFile ("take2.wav");
    region.timelineStart = 3000;
    region.lengthInSamples = 9000;
    region.previousTakes = { { temp.dir.getChildFile ("audio").getChildFile ("take1.wav"),
                               0, 5000, {} } };
    a.track (0).regions = { region };
    REQUIRE (SessionSerializer::save (a, target));

    // An entry anchored at its region's start is written exactly as before
    // the anchor existed, so older sessions and older builds agree.
    const auto takes = firstRegionTakes (target);
    REQUIRE (takes.size() == 1);
    CHECK_FALSE (takes[0].contains ("timeline_offset"));

    // Loaded over a session that held an anchored take: the absent key must
    // not inherit it.
    Session b;
    b.setSessionDirectory (temp.dir);
    b.track (0).regions = { retakeOverLaterTake() };
    REQUIRE (SessionSerializer::load (b, target));
    const auto& loaded = b.track (0).regions.front();
    REQUIRE (loaded.previousTakes.size() == 1);
    CHECK (loaded.previousTakes[0].timelineOffset == 0);

    auto popped = loaded;
    REQUIRE (popAudioTake (popped));
    CHECK (popped.timelineStart == 3000);
}

TEST_CASE ("A hand-edited anchor cannot put a take before the timeline origin",
           "[session][takes][anchor][serializer]")
{
    const auto temp = makeSessionDir ("dusk-take-anchor-clamp-");
    const auto target = temp.dir.getChildFile ("session.json");

    Session a;
    a.setSessionDirectory (temp.dir);
    a.track (0).regions = { retakeOverLaterTake() };
    REQUIRE (SessionSerializer::save (a, target));

    auto root = nlohmann::json::parse (target.loadFileAsString().toStdString());
    root["tracks"][0]["regions"][0]["previous_takes"][0]["timeline_offset"] = -999999;
    REQUIRE (target.replaceWithText (juce::String (root.dump())));

    Session b;
    b.setSessionDirectory (temp.dir);
    REQUIRE (SessionSerializer::load (b, target));
    const auto& loaded = b.track (0).regions.front();
    CHECK (loaded.timelineStart + loaded.previousTakes[0].timelineOffset == 0);
}
