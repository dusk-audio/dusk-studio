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

TEST_CASE ("Chord entry candidates use a claimed key and otherwise sort alphabetically")
{
    const std::vector<std::string> candidates {
        "F#m", "A", "D", "Ebm", "C#dim", "C", "A", "Chorus"
    };

    SECTION ("diatonic triads lead when key detection has made a claim")
    {
        CHECK (chords::rankCandidates (candidates, "D")
               == std::vector<std::string> { "A", "C#dim", "D", "F#m", "C", "Ebm" });
    }

    SECTION ("blank and invalid keys never influence the order")
    {
        const std::vector<std::string> alphabetical {
            "A", "C", "C#dim", "D", "Ebm", "F#m"
        };
        CHECK (chords::rankCandidates (candidates, {}) == alphabetical);
        CHECK (chords::rankCandidates (candidates, "not a key") == alphabetical);
        CHECK (chords::rankCandidates (candidates, "D7") == alphabetical);
    }

    SECTION ("an infixed suspension is not the degree's major triad")
    {
        // F is the fourth of C and F7sus4 sits on it, but a suspension names no
        // triad, so the plain fifth leads even though it sorts later.
        CHECK (chords::rankCandidates ({ "F7sus4", "G" }, "C")
               == std::vector<std::string> { "G", "F7sus4" });
    }
}

TEST_CASE ("A partly typed chord name completes case-insensitively")
{
    // What the slot shows is what committing it places, so the match that
    // drives the completion has to accept the casing a hurried writer types.
    CHECK (chords::completes ("Am", "am"));
    CHECK (chords::completes ("Am", "A"));
    CHECK (chords::completes ("Cmaj7", "cM"));
    CHECK (chords::completes ("Am", "Am"));

    CHECK_FALSE (chords::completes ("Am", "Amj"));
    CHECK_FALSE (chords::completes ("Am", "m"));
    CHECK_FALSE (chords::completes ("A", "Am"));
    // Nothing typed suggests nothing: an empty slot must not adopt the first
    // ranked candidate when it closes.
    CHECK_FALSE (chords::completes ("Am", ""));
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

    SECTION ("rewriting a chord as itself reports that nothing changed")
    {
        // Closing a chord slot without editing it must not look like an edit:
        // the caller records an undo step on true, and recording one would
        // both add a phantom step and throw away the redo stack.
        REQUIRE (document.setChordAt (4, "G"));
        CHECK_FALSE (document.setChordAt (4, "G"));
        CHECK (document.markdown() == "one [G]two");
    }

    SECTION ("removing where no chord is anchored is a no-op")
    {
        CHECK_FALSE (document.setChordAt (2, ""));
        CHECK (document.markdown() == "one two");
    }
}

TEST_CASE ("repeatPreviousChordAt copies the preceding chord in either view")
{
    NotepadDocument document;
    document.setMarkdown ("[Am]one [F]two three");

    SECTION ("the rendered chart repeats onto the lyric anchor")
    {
        REQUIRE (document.repeatPreviousChordAt (8));
        CHECK (document.markdown() == "[Am]one [F]two [F]three");
    }

    SECTION ("there is nothing to repeat before the first chord")
    {
        CHECK_FALSE (document.repeatPreviousChordAt (0));
        CHECK (document.markdown() == "[Am]one [F]two three");
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

    SECTION ("stepping back through history keeps the document's own reading")
    {
        // Undo and redo put earlier states of the same document back. The state
        // they restore can be all naturals, and re-deriving there would hand a
        // flat writer a document spelled in sharps.
        NotepadDocument flats;
        flats.setMarkdown ("[Db]x");
        REQUIRE (flats.prefersFlats());

        flats.transposeChords (-1, flats.prefersFlats());
        REQUIRE (flats.markdown() == "[C]x");

        flats.restoreMarkdown ("[Db]x");
        flats.restoreMarkdown ("[C]x");
        CHECK (flats.prefersFlats());
    }
}

TEST_CASE ("Chord commands reach the source behind the hidden syntax")
{
    NotepadDocument document;
    document.setMarkdown ("# Title\n\n[Am]one two");
    // The counts describe the lyric, never the syntax the projection hides.
    CHECK (document.wordCount() == 3);
    CHECK (document.characterCount() == std::string ("Title\n\none two").size());

    SECTION ("transpose rewrites the source")
    {
        document.transposeChords (2, document.prefersFlats());
        CHECK (document.markdown() == "# Title\n\n[Bm]one two");
    }

    SECTION ("a chord can be appended past the last lyric character")
    {
        REQUIRE (document.setChordAt (document.documentText().size(), "G"));
        CHECK (document.markdown() == "# Title\n\n[Am]one two[G]");
    }
}

TEST_CASE ("Key detection only claims a key the evidence supports")
{
    SECTION ("a clear diatonic progression names its key")
    {
        CHECK (chords::detectKey ({ "Am", "F", "C", "G" }) == "Am");
        CHECK (chords::detectKey ({ "C", "F", "G", "Am" }) == "C");
        CHECK (chords::detectKey ({ "D", "G", "A", "Bm" }) == "D");
    }

    SECTION ("power chords and suspensions carry no third")
    {
        // Roots enough for a key, but nothing that can argue major or minor.
        CHECK (chords::detectKey ({ "A5", "D5", "E5", "G5" }).empty());
        CHECK (chords::detectKey ({ "Asus4", "Dsus2", "Esus4" }).empty());

        // A power chord still joins the root set, so it cannot flip the mode
        // its neighbours establish.
        CHECK (chords::detectKey ({ "Am", "Gb5" }).empty());
        CHECK (chords::detectKey ({ "Am", "F", "C", "E5" }) == "Am");
    }

    SECTION ("a suspension behind a figure is still a suspension")
    {
        // "7sus4" and friends put the figure first; the third is replaced all
        // the same, so they may not vote as major triads.
        CHECK (chords::detectKey ({ "A7sus4", "D9sus2", "E13sus4" }).empty());
        CHECK (chords::detectKey ({ "C", "F", "G7sus4" }).empty());
    }

    SECTION ("a tie the opening triad cannot break stays blank")
    {
        // Every key that fits C-Bb-F-G is a tie, and C major is not among them
        // - the opening triad claims a tonic the evidence rules out, so no key
        // is named rather than one of the ties being picked arbitrarily.
        CHECK (chords::detectKey ({ "C", "Bb", "F", "G" }).empty());

        // The same shape with an opening triad that is one of the ties resolves
        // to it.
        CHECK (chords::detectKey ({ "Bb", "C", "F", "G" }) == "Bb");
    }

    SECTION ("two chords are not evidence")
    {
        CHECK (chords::detectKey ({ "Am", "F" }).empty());
        CHECK (chords::detectKey ({ "C" }).empty());
        CHECK (chords::detectKey ({}).empty());
    }

    SECTION ("a modal progression is not forced into a label")
    {
        // Dorian on D: the roots fit C major and D minor equally, so neither
        // may be claimed.
        CHECK (chords::detectKey ({ "Dm", "Em", "Dm", "Em" }).empty());
    }

    SECTION ("keys are spelled the way key signatures are written")
    {
        // C# minor, never Db minor, whatever the chords themselves use.
        CHECK (chords::detectKey ({ "C#m", "F#m", "G#m", "A" }) == "C#m");
        CHECK (chords::detectKey ({ "Dbm", "Gbm", "Abm", "A" }) == "C#m");
        CHECK (chords::detectKey ({ "Eb", "Ab", "Bb", "Cm" }) == "Eb");
    }
}

TEST_CASE ("Section markers are bracketed lines that are not chords")
{
    NotepadDocument document;
    document.setMarkdown ("[Verse]\nline [Am]one\n[Chorus]\nline two\n");
    CHECK (document.sectionCount() == 2);
    CHECK (document.uniqueChordNames().size() == 1);

    SECTION ("a chord alone on a line is not a section")
    {
        document.setMarkdown ("[Am]\nline\n");
        CHECK (document.sectionCount() == 0);
    }
}

TEST_CASE ("Chord spelling follows the song until the user overrides it")
{
    NotepadDocument document;
    document.setMarkdown ("[Bb]one [Eb]two");
    CHECK (document.spellingMode() == NotepadDocument::Spelling::followDocument);
    CHECK (document.prefersFlats());

    // The inferred reading is sticky across a transpose onto naturals, which
    // is why it has to be overridable rather than merely re-derived.
    document.transposeChords (2, document.prefersFlats());
    CHECK (document.markdown() == "[C]one [F]two");
    CHECK (document.prefersFlats());

    SECTION ("an override wins over the inference")
    {
        document.setSpelling (NotepadDocument::Spelling::sharps);
        CHECK_FALSE (document.prefersFlats());
        document.transposeChords (1, document.prefersFlats());
        CHECK (document.markdown() == "[C#]one [F#]two");
    }

    SECTION ("returning to follow restores the song's own reading")
    {
        document.setSpelling (NotepadDocument::Spelling::sharps);
        document.setSpelling (NotepadDocument::Spelling::followDocument);
        CHECK (document.prefersFlats());
    }
}
