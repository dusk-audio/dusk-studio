#include <catch2/catch_test_macros.hpp>

#include "engine/RecordManager.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace duskstudio;

namespace
{
constexpr double       kSampleRate = 44100.0;
constexpr std::int64_t kFade       = 64;

struct ScopedDir
{
    juce::File dir;
    ~ScopedDir() { dir.deleteRecursively(); }
};

// ctest runs cases as parallel processes, so the directory is claimed by
// creating it: a false return means another case already holds that name.
ScopedDir makeSessionDir (const char* tag)
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

void prepareTrack (Session& session, const juce::File& dir)
{
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);
}

// Records one mono take of `frames` samples starting at `start` and returns
// the file it was written to.
juce::File recordTake (Session& session, std::int64_t start, int frames)
{
    RecordManager manager (session);
    REQUIRE (manager.startRecording (kSampleRate, start, 0));
    std::vector<float> block ((size_t) frames, 0.25f);
    manager.writeInputBlock (0, block.data(), nullptr, frames);
    manager.stopRecording (start + frames);

    const auto& regions = session.track (0).regions;
    const auto it = std::find_if (regions.begin(), regions.end(),
                                  [start] (const AudioRegion& r) { return r.timelineStart == start; });
    REQUIRE (it != regions.end());
    return it->file;
}

const AudioRegion& regionFor (const Session& session, const juce::File& file)
{
    const auto& regions = session.track (0).regions;
    const auto it = std::find_if (regions.begin(), regions.end(),
                                  [&file] (const AudioRegion& r) { return r.file == file; });
    REQUIRE (it != regions.end());
    return *it;
}

void requireTake (const TakeRef& take, const juce::File& file,
                  std::int64_t sourceOffset, std::int64_t length,
                  std::int64_t timelineOffset = 0)
{
    CHECK (take.file == file);
    CHECK (take.sourceOffset == sourceOffset);
    CHECK (take.lengthInSamples == length);
    CHECK (take.timelineOffset == timelineOffset);
}
} // namespace

// Three takes on one track, each recorded over the last, with the third
// starting just inside the second (the demo-path walk, scaled down).
TEST_CASE ("An overdub keeps what it covers of every region in its take history",
           "[recording][recordmanager][takes]")
{
    const auto temp = makeSessionDir ("dusk-overdub-history-");
    Session session;
    prepareTrack (session, temp.dir);

    const auto take1 = recordTake (session, 0, 3200);
    const auto take2 = recordTake (session, 0, 2350);

    // Take 2 over the head of take 1: take 1's remainder stays on the timeline
    // and the covered part is take 2's history.
    {
        const auto& second = regionFor (session, take2);
        REQUIRE (second.previousTakes.size() == 1);
        requireTake (second.previousTakes[0], take1, 0, 2350);
        const auto& remainder = regionFor (session, take1);
        CHECK (remainder.timelineStart == 2350 - kFade);
        CHECK (remainder.sourceOffset == 2350 - kFade);
    }

    const auto take3 = recordTake (session, 65, 4290);

    const auto& regions = session.track (0).regions;
    REQUIRE (regions.size() == 2);

    const auto& second = regionFor (session, take2);
    CHECK (second.timelineStart == 0);
    CHECK (second.lengthInSamples == 65 + kFade);
    CHECK (second.fadeOutSamples == kFade);

    const auto& third = regionFor (session, take3);
    CHECK (third.timelineStart == 65);
    CHECK (third.fadeInSamples == kFade);
    REQUIRE (third.previousTakes.size() == 2);

    SECTION ("take 2's covered part comes first, anchored at take 3's start")
    {
        requireTake (third.previousTakes[0], take2, 65, 2350 - 65);
    }

    SECTION ("take 1 is one take, as far as its file reaches")
    {
        requireTake (third.previousTakes[1], take1, 65, 3200 - 65);
    }

    SECTION ("deleting take 3 brings take 2 back where it was")
    {
        auto popped = third;
        REQUIRE (popAudioTake (popped));
        CHECK (popped.file == take2);
        CHECK (popped.timelineStart == 65);
        CHECK (popped.timelineStart + popped.lengthInSamples == 2350);
        // Same file position at the same song position as take 2's remainder,
        // so the two crossfade over the 64 samples they share as one take.
        CHECK (popped.sourceOffset - popped.timelineStart
               == second.sourceOffset - second.timelineStart);
        CHECK (popped.fadeInSamples == second.fadeOutSamples);

        REQUIRE (popAudioTake (popped));
        CHECK (popped.file == take1);
        CHECK (popped.timelineStart + popped.lengthInSamples == 3200);
        CHECK_FALSE (popAudioTake (popped));
    }

    SECTION ("the history survives a save and reload")
    {
        const auto target = temp.dir.getChildFile ("session.json");
        REQUIRE (SessionSerializer::save (session, target));
        Session reloaded;
        reloaded.setSessionDirectory (temp.dir);
        REQUIRE (SessionSerializer::load (reloaded, target));

        const auto& reloadedThird = regionFor (reloaded, take3);
        REQUIRE (reloadedThird.previousTakes.size() == 2);
        requireTake (reloadedThird.previousTakes[0], take2, 65, 2350 - 65);
        requireTake (reloadedThird.previousTakes[1], take1, 65, 3200 - 65);
    }
}

TEST_CASE ("A retake that starts earlier brings the older take back where it was",
           "[recording][recordmanager][takes]")
{
    const auto temp = makeSessionDir ("dusk-overdub-earlier-");
    Session session;
    prepareTrack (session, temp.dir);

    const auto take1 = recordTake (session, 5000, 5000);
    const auto take2 = recordTake (session, 3000, 9000);

    const auto& retake = regionFor (session, take2);
    REQUIRE (session.track (0).regions.size() == 1);
    REQUIRE (retake.previousTakes.size() == 1);
    requireTake (retake.previousTakes[0], take1, 0, 5000, 2000);

    auto popped = retake;
    REQUIRE (popAudioTake (popped));
    CHECK (popped.file == take1);
    CHECK (popped.timelineStart == 5000);
    CHECK (popped.lengthInSamples == 5000);
    CHECK (popped.sourceOffset == 0);
}

TEST_CASE ("A take whose head was trimmed off comes back without it",
           "[recording][recordmanager][takes]")
{
    const auto temp = makeSessionDir ("dusk-overdub-trimmed-");
    Session session;
    prepareTrack (session, temp.dir);

    const auto take1 = recordTake (session, 0, 10000);
    {
        auto& trimmed = session.track (0).regions.front();
        moveRegionStartKeepingTakes (trimmed, 2000);
        trimmed.sourceOffset = 2000;
        trimmed.lengthInSamples = 8000;
    }
    const auto take2 = recordTake (session, 1000, 11000);

    const auto& retake = regionFor (session, take2);
    REQUIRE (retake.previousTakes.size() == 1);
    requireTake (retake.previousTakes[0], take1, 2000, 8000, 1000);

    auto popped = retake;
    REQUIRE (popAudioTake (popped));
    CHECK (popped.timelineStart == 2000);
    CHECK (popped.sourceOffset == 2000);
    CHECK (popped.lengthInSamples == 8000);
}

TEST_CASE ("An overdub over the head of a later take keeps that head where it was",
           "[recording][recordmanager][takes]")
{
    const auto temp = makeSessionDir ("dusk-overdub-head-");
    Session session;
    prepareTrack (session, temp.dir);

    const auto take1 = recordTake (session, 1000, 1000);
    const auto take2 = recordTake (session, 500, 1000);

    const auto& remainder = regionFor (session, take1);
    CHECK (remainder.timelineStart == 1500 - kFade);

    const auto& retake = regionFor (session, take2);
    REQUIRE (retake.previousTakes.size() == 1);
    requireTake (retake.previousTakes[0], take1, 0, 500, 500);

    // Popped back in, take 1's head meets its remainder with the fades the
    // overdub left on both sides of that seam.
    auto popped = retake;
    REQUIRE (popAudioTake (popped));
    CHECK (popped.timelineStart == 1000);
    CHECK (popped.timelineStart + popped.lengthInSamples == 1500);
    CHECK (popped.sourceOffset - popped.timelineStart
           == remainder.sourceOffset - remainder.timelineStart);
    CHECK (popped.fadeOutSamples == remainder.fadeInSamples);
}

TEST_CASE ("Overdub take history stops at eight takes", "[recording][recordmanager][takes]")
{
    const auto temp = makeSessionDir ("dusk-overdub-cap-");
    Session session;
    prepareTrack (session, temp.dir);

    AudioRegion stacked;
    stacked.file = temp.dir.getChildFile ("stacked.wav");
    stacked.lengthInSamples = 1000;
    for (int i = 0; i < 8; ++i)
        stacked.previousTakes.push_back (
            { temp.dir.getChildFile ("older-" + juce::String (i) + ".wav"), 0, 1000, {} });
    session.track (0).regions = { stacked };

    const auto take = recordTake (session, 0, 1000);
    const auto& current = regionFor (session, take);
    REQUIRE (current.previousTakes.size() == 8);
    CHECK (current.previousTakes.front().file == stacked.file);
    CHECK (current.previousTakes.back().file == stacked.previousTakes[6].file);
}

TEST_CASE ("A MIDI retake that starts earlier brings the older take back where it was",
           "[recording][recordmanager][takes][midi]")
{
    const auto temp = makeSessionDir ("dusk-overdub-midi-");
    Session session;
    session.setSessionDirectory (temp.dir);
    session.track (0).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    session.track (0).midiRegions.mutate ([] (std::vector<MidiRegion>& v)
    {
        MidiRegion older;
        older.timelineStart   = 48000;
        older.lengthInSamples = 24000;
        older.lengthInTicks   = 480;
        older.notes = { { 1, 60, 90, 0, 240 } };
        v.push_back (std::move (older));
    });

    RecordManager manager (session);
    REQUIRE (manager.startRecording (48000.0, 24000, 0));
    dusk::MidiBuffer block;
    const std::uint8_t noteOn[]  { 0x90, 64, 100 };
    const std::uint8_t noteOff[] { 0x80, 64, 0 };
    block.addEvent (noteOn, 3, 0);
    block.addEvent (noteOff, 3, 200);
    manager.writeMidiBlock (0, block, 0);
    manager.stopRecording (24000 + 96000);

    const auto regions = session.track (0).midiRegions.current();
    REQUIRE (regions.size() == 1);
    const auto& retake = regions.front();
    CHECK (retake.timelineStart == 24000);
    REQUIRE (retake.previousTakes.size() == 1);
    CHECK (retake.previousTakes[0].timelineOffset == 24000);

    auto popped = retake;
    REQUIRE (popMidiTake (popped));
    CHECK (popped.timelineStart == 48000);
    CHECK (popped.lengthInTicks == 480);
    REQUIRE (popped.notes.size() == 1);
    CHECK (popped.notes[0].noteNumber == 60);
}
