#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"

#include <juce_core/juce_core.h>

using namespace duskstudio;

namespace
{
TakeRef take (const char* name, std::int64_t sourceOffset, std::int64_t length)
{
    return { juce::File::getCurrentWorkingDirectory().getChildFile (name),
             sourceOffset, length, {} };
}

AudioRegion stackOfThree()
{
    AudioRegion region;
    region.file = juce::File::getCurrentWorkingDirectory().getChildFile ("take3.wav");
    region.timelineStart = 65;
    region.lengthInSamples = 4290;
    region.fadeInSamples = 64;
    region.fadeInShape = FadeShape::RaisedCosine;
    region.previousTakes = { take ("take2.wav", 65, 2285), take ("take1.wav", 65, 3135) };
    return region;
}
} // namespace

TEST_CASE ("Delete on a take with earlier takes brings back the one under it",
           "[session][takes]")
{
    auto region = stackOfThree();
    const auto under = region.previousTakes.front();

    REQUIRE (popAudioTake (region));
    CHECK (region.file == under.file);
    CHECK (region.sourceOffset == under.sourceOffset);
    CHECK (region.lengthInSamples == under.lengthInSamples);
    CHECK (region.timelineStart == 65);
    CHECK (region.fadeInSamples == 64);
    CHECK (region.fadeInShape == FadeShape::RaisedCosine);
    REQUIRE (region.previousTakes.size() == 1);
    CHECK (region.previousTakes.front().file.getFileName() == "take1.wav");
}

TEST_CASE ("Delete on the last take leaves the region for removal", "[session][takes]")
{
    AudioRegion region;
    region.file = juce::File::getCurrentWorkingDirectory().getChildFile ("only.wav");
    region.timelineStart = 100;
    region.lengthInSamples = 500;

    const auto before = region;
    CHECK_FALSE (popAudioTake (region));
    CHECK (region.file == before.file);
    CHECK (region.timelineStart == before.timelineStart);
    CHECK (region.lengthInSamples == before.lengthInSamples);
}

TEST_CASE ("Repeated Delete walks down the whole stack", "[session][takes]")
{
    auto region = stackOfThree();
    REQUIRE (popAudioTake (region));
    REQUIRE (popAudioTake (region));
    CHECK (region.file.getFileName() == "take1.wav");
    CHECK (region.lengthInSamples == 3135);
    CHECK (region.previousTakes.empty());
    CHECK_FALSE (popAudioTake (region));
}

TEST_CASE ("A popped take shorter than the region's fades keeps them inside it",
           "[session][takes]")
{
    AudioRegion region;
    region.file = juce::File::getCurrentWorkingDirectory().getChildFile ("long.wav");
    region.lengthInSamples = 1000;
    region.fadeInSamples = 64;
    region.fadeOutSamples = 500;
    region.previousTakes = { take ("short.wav", 0, 300) };

    REQUIRE (popAudioTake (region));
    CHECK (region.lengthInSamples == 300);
    CHECK (region.fadeInSamples == 64);
    CHECK (region.fadeOutSamples == 300 - 64);
}

TEST_CASE ("Delete on a MIDI take with earlier takes brings back the one under it",
           "[session][takes][midi]")
{
    MidiRegion region;
    region.timelineStart = 480;
    region.lengthInTicks = 1920;
    region.notes = { { 1, 64, 100, 0, 240 } };

    MidiTakeRef under;
    under.lengthInTicks = 960;
    under.notes = { { 1, 60, 90, 120, 120 } };
    region.previousTakes = { under };

    REQUIRE (popMidiTake (region));
    CHECK (region.timelineStart == 480);
    CHECK (region.lengthInTicks == 960);
    REQUIRE (region.notes.size() == 1);
    CHECK (region.notes[0].noteNumber == 60);
    CHECK (region.previousTakes.empty());
    CHECK_FALSE (popMidiTake (region));
}
