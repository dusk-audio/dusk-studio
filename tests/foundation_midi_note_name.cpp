#include <catch2/catch_test_macros.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include "foundation/MidiNoteName.h"

TEST_CASE ("dusk::midiNoteName matches the JUCE spelling it replaces", "[foundation][midi]")
{
    for (int note = 0; note < 128; ++note)
        REQUIRE (dusk::midiNoteName (note)
                 == juce::MidiMessage::getMidiNoteName (note, true, true, 4).toStdString());
}

TEST_CASE ("dusk::midiNoteName anchors middle C and rejects out-of-range notes",
           "[foundation][midi]")
{
    REQUIRE (dusk::midiNoteName (60) == "C4");
    REQUIRE (dusk::midiNoteName (0) == "C-1");
    REQUIRE (dusk::midiNoteName (61) == "C#4");
    REQUIRE (dusk::midiNoteName (127) == "G9");
    REQUIRE (dusk::midiNoteName (-1).empty());
    REQUIRE (dusk::midiNoteName (128).empty());
}
