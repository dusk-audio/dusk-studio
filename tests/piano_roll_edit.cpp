#include <catch2/catch_test_macros.hpp>

#include "ui/PianoRollEdit.h"

#include <algorithm>
#include <vector>

using namespace duskstudio;

TEST_CASE ("Glue keeps MIDI channels independent", "[midi][piano-roll]")
{
    std::vector<MidiNote> notes (4);
    notes[0] = { 1, 60, 91, 0, 120 };
    notes[1] = { 2, 60, 72, 120, 120 };
    notes[2] = { 1, 60, 63, 120, 120 };
    notes[3] = { 1, 64, 80, 240, 120 };
    PianoRollEdit edit;
    for (int i = 0; i < 4; ++i) edit.addToSelection (i);

    edit.glueSelectedNotes (notes);

    REQUIRE (notes.size() == 3);
    const auto channelTwo = std::find_if (notes.begin(), notes.end(),
                                         [] (const MidiNote& n) { return n.channel == 2; });
    REQUIRE (channelTwo != notes.end());
    CHECK (channelTwo->startTick == 120);
    CHECK (channelTwo->lengthInTicks == 120);
    CHECK (channelTwo->velocity == 72);
    CHECK (notes[0].channel == 1);
    CHECK (notes[0].lengthInTicks == 240);
    CHECK (notes[0].velocity == 91);
    CHECK (edit.selectedNotes.empty());
}

TEST_CASE ("Glue preserves gaps and unselected notes", "[midi][piano-roll]")
{
    std::vector<MidiNote> notes {
        { 16, 127, 90, 0, 120 },
        { 16, 127, 70, 120, 120 },
        { 16, 127, 60, 300, 120 },
        { 16, 127, 50, 60, 60 },
        { 16, 126, 40, 0, 240 }
    };
    PianoRollEdit edit;
    for (const int i : { 0, 1, 2, 4 }) edit.addToSelection (i);

    edit.glueSelectedNotes (notes);

    REQUIRE (notes.size() == 4);
    CHECK (notes[0].lengthInTicks == 240);
    CHECK (notes[0].velocity == 90);
    CHECK (notes[1].startTick == 300);
    CHECK (notes[1].lengthInTicks == 120);
    CHECK (notes[2].startTick == 60);
    CHECK (notes[2].lengthInTicks == 60);
    CHECK (notes[3].noteNumber == 126);
}
