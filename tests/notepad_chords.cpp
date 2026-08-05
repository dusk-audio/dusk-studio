#include <catch2/catch_test_macros.hpp>

#include "ui/NotepadChords.h"
#include "ui/NotepadDocument.h"

using duskstudio::NotepadDocument;
namespace chords = duskstudio::notepad::chords;

TEST_CASE ("chord grammar accepts real chord names")
{
    CHECK (chords::isChord ("C"));
    CHECK (chords::isChord ("Am"));
    CHECK (chords::isChord ("Bb"));
    CHECK (chords::isChord ("F#m7b5"));
    CHECK (chords::isChord ("Cmaj7"));
    CHECK (chords::isChord ("Gsus4"));
    CHECK (chords::isChord ("Dadd9"));
    CHECK (chords::isChord ("D/F#"));
    CHECK (chords::isChord ("Eb13"));
}

TEST_CASE ("chord grammar rejects ordinary bracketed text")
{
    CHECK_FALSE (chords::isChord (""));
    CHECK_FALSE (chords::isChord ("Bass"));
    CHECK_FALSE (chords::isChord ("verse 2"));
    CHECK_FALSE (chords::isChord ("TODO"));
    CHECK_FALSE (chords::isChord ("x"));
    CHECK_FALSE (chords::isChord ("Chorus"));
    CHECK_FALSE (chords::isChord ("A/H"));
    CHECK_FALSE (chords::isChord ("N.C."));
}

TEST_CASE ("transpose shifts the root and keeps the quality")
{
    CHECK (chords::transpose ("C", 2, false) == "D");
    CHECK (chords::transpose ("Am7", 3, false) == "Cm7");
    CHECK (chords::transpose ("B", 1, false) == "C");
    CHECK (chords::transpose ("C", -1, false) == "B");
    CHECK (chords::transpose ("Gsus4", 5, false) == "Csus4");

    SECTION ("spelling follows the flat preference")
    {
        CHECK (chords::transpose ("C", 1, false) == "C#");
        CHECK (chords::transpose ("C", 1, true) == "Db");
    }

    SECTION ("a slash bass moves with the root")
    {
        CHECK (chords::transpose ("D/F#", 2, false) == "E/G#");
    }

    SECTION ("non-chords are returned untouched")
    {
        CHECK (chords::transpose ("Chorus", 2, false) == "Chorus");
    }
}

TEST_CASE ("chord brackets are hidden from the document projection")
{
    NotepadDocument document;
    document.setMarkdown ("[Am]one two [C]three");

    REQUIRE (document.documentText() == "one two three");
    REQUIRE (document.chords().size() == 2);
    CHECK (document.chords()[0].name == "Am");
    CHECK (document.chords()[0].documentOffset == 0);
    CHECK (document.chords()[1].name == "C");
    CHECK (document.chords()[1].documentOffset == 8);
    CHECK (document.chordAt (8) == "C");
    CHECK (document.chordAt (3).empty());

    SECTION ("a non-chord bracket stays literal text")
    {
        document.setMarkdown ("[Chorus] one");
        CHECK (document.documentText() == "[Chorus] one");
        CHECK (document.chords().empty());
    }

    SECTION ("a Markdown link is still a link")
    {
        document.setMarkdown ("[C](https://example.com)");
        CHECK (document.documentText() == "C");
        CHECK (document.chords().empty());
        CHECK_FALSE (document.hasChords());
        CHECK (document.linkTargetAt (0) == "https://example.com");
    }

    SECTION ("a chord at the end of a line anchors on its newline")
    {
        document.setMarkdown ("one[G]\ntwo");
        REQUIRE (document.documentText() == "one\ntwo");
        REQUIRE (document.chords().size() == 1);
        CHECK (document.chords()[0].documentOffset == 3);
    }
}

TEST_CASE ("bracket text the projection keeps is never a chord token")
{
    NotepadDocument document;

    SECTION ("an escaped bracket stays literal")
    {
        document.setMarkdown ("\\[Am]one");
        CHECK (document.documentText() == "[Am]one");
        CHECK (document.chords().empty());
        CHECK_FALSE (document.hasChords());

        document.transposeChords (2, false);
        CHECK (document.markdown() == "\\[Am]one");
    }

    SECTION ("a bracket inside a code span stays literal")
    {
        document.setMarkdown ("`[Am]` one");
        CHECK (document.documentText() == "[Am] one");
        CHECK (document.chords().empty());
        CHECK_FALSE (document.hasChords());

        document.transposeChords (2, false);
        CHECK (document.markdown() == "`[Am]` one");
    }

    SECTION ("a bracket inside a link destination stays part of the URL")
    {
        document.setMarkdown ("[song](https://example.com/[Am])");
        CHECK (document.documentText() == "song");
        CHECK (document.chords().empty());
        CHECK_FALSE (document.hasChords());

        document.transposeChords (2, false);
        CHECK (document.markdown() == "[song](https://example.com/[Am])");
        CHECK (document.linkTargetAt (0) == "https://example.com/[Am]");
    }
}

TEST_CASE ("chord anchors survive edits to the lyric around them")
{
    NotepadDocument document;
    document.setMarkdown ("[Am]first line of the [C]verse here");

    REQUIRE (document.replaceDocumentText ("first line of the chorus here"));
    REQUIRE (document.chords().size() == 2);
    CHECK (document.chords()[0].documentOffset == 0);
    CHECK (document.chords()[1].name == "C");
    CHECK (document.markdown() == "[Am]first line of the [C]chorus here");
}

TEST_CASE ("setChordAt inserts, replaces and removes")
{
    NotepadDocument document;
    document.setMarkdown ("one two");

    SECTION ("insert anchors on the character at the offset")
    {
        REQUIRE (document.setChordAt (4, "G"));
        CHECK (document.markdown() == "one [G]two");
        CHECK (document.documentText() == "one two");
        CHECK (document.chordAt (4) == "G");
    }

    SECTION ("a second call at the same anchor replaces")
    {
        REQUIRE (document.setChordAt (4, "G"));
        REQUIRE (document.setChordAt (4, "Am7"));
        CHECK (document.markdown() == "one [Am7]two");
        CHECK (document.chords().size() == 1);
    }

    SECTION ("an empty name removes the chord and its brackets")
    {
        REQUIRE (document.setChordAt (0, "D"));
        REQUIRE (document.setChordAt (0, ""));
        CHECK (document.markdown() == "one two");
        CHECK (document.chords().empty());
    }

    SECTION ("a name that is not a chord is refused")
    {
        CHECK_FALSE (document.setChordAt (0, "Chorus"));
        CHECK (document.markdown() == "one two");
    }

    SECTION ("removing where no chord is anchored is a no-op")
    {
        CHECK_FALSE (document.setChordAt (2, ""));
        CHECK (document.markdown() == "one two");
    }
}

TEST_CASE ("transposeChords rewrites every chord in the source")
{
    NotepadDocument document;
    document.setMarkdown ("[Am]one [F]two [G7]three");

    document.transposeChords (2, false);
    CHECK (document.markdown() == "[Bm]one [G]two [A7]three");
    CHECK (document.documentText() == "one two three");

    SECTION ("down again returns to the original spelling")
    {
        document.transposeChords (-2, false);
        CHECK (document.markdown() == "[Am]one [F]two [G7]three");
    }

    SECTION ("flat spelling is available")
    {
        NotepadDocument flats;
        flats.setMarkdown ("[C]x");
        flats.transposeChords (1, true);
        CHECK (flats.markdown() == "[Db]x");
    }
}

TEST_CASE ("Transpose keeps the notation the document already uses")
{
    NotepadDocument document;
    document.setMarkdown ("[F#5]one [C#m]two");
    CHECK_FALSE (document.prefersFlats());

    // A round trip must return the user's own spelling: the direction of
    // travel is not allowed to rename a chord.
    document.transposeChords (1, document.prefersFlats());
    document.transposeChords (-1, document.prefersFlats());
    CHECK (document.markdown() == "[F#5]one [C#m]two");

    SECTION ("a flat document keeps its flats")
    {
        NotepadDocument flats;
        flats.setMarkdown ("[Bb]one [Eb]two");
        CHECK (flats.prefersFlats());
        flats.transposeChords (2, flats.prefersFlats());
        flats.transposeChords (-2, flats.prefersFlats());
        CHECK (flats.markdown() == "[Bb]one [Eb]two");
    }

    SECTION ("a transpose onto naturals keeps the spelling it was given")
    {
        NotepadDocument flats;
        flats.setMarkdown ("[Db]x");
        REQUIRE (flats.prefersFlats());
        flats.transposeChords (-1, flats.prefersFlats());
        CHECK (flats.markdown() == "[C]x");
        CHECK (flats.prefersFlats());
    }

    SECTION ("loading another document starts from sharps again")
    {
        NotepadDocument flats;
        flats.setMarkdown ("[Bb]one");
        REQUIRE (flats.prefersFlats());

        flats.setMarkdown ("[C]one");
        CHECK_FALSE (flats.prefersFlats());

        flats.setMarkdown ("[Eb]one");
        CHECK (flats.prefersFlats());
    }
}

TEST_CASE ("Chord commands and counts survive the source view")
{
    NotepadDocument document;
    document.setMarkdown ("# Title\n\n[Am]one two");
    const auto renderedWords = document.wordCount();
    const auto renderedCharacters = document.characterCount();

    document.setSourceMode (true);
    CHECK (document.hasChords());
    CHECK (document.wordCount() == renderedWords);
    CHECK (document.characterCount() == renderedCharacters);

    SECTION ("transpose still rewrites the source")
    {
        document.transposeChords (2, document.prefersFlats());
        CHECK (document.markdown() == "# Title\n\n[Bm]one two");
    }

    SECTION ("a chord can be inserted at a source offset")
    {
        REQUIRE (document.setChordAt (document.markdown().size(), "G"));
        CHECK (document.markdown() == "# Title\n\n[Am]one two[G]");
    }

    SECTION ("a caret inside a token edits that chord instead of nesting one")
    {
        // The token runs [9, 13); the caret sits on its 'm'.
        REQUIRE (document.markdown().compare (9, 4, "[Am]") == 0);
        CHECK (document.chordAt (11) == "Am");

        REQUIRE (document.setChordAt (11, "C"));
        CHECK (document.markdown() == "# Title\n\n[C]one two");

        REQUIRE (document.setChordAt (10, ""));
        CHECK (document.markdown() == "# Title\n\none two");
        CHECK_FALSE (document.hasChords());
    }
}
