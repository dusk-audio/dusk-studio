#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/ChordAnalyzer.h"

#include <vector>

using Catch::Matchers::WithinAbs;

// ChordAnalyzer is pure note-set -> chord logic (no audio, no threads). These
// pin the interval-pattern matcher, root/inversion detection, and the static
// note-name helpers so a refactor of the pattern table can't silently mis-label
// chords (the tuner / chord-display read this directly).

TEST_CASE ("ChordAnalyzer: triads match quality + root", "[chord]")
{
    ChordAnalyzer a;

    SECTION ("C major")
    {
        const auto c = a.analyze ({ 60, 64, 67 });
        REQUIRE (c.quality == ChordQuality::Major);
        REQUIRE (c.rootNote == 0);
        REQUIRE (c.name == "C");
        REQUIRE (c.confidence > 0.0f);
    }
    SECTION ("A minor")
    {
        const auto c = a.analyze ({ 57, 60, 64 });
        REQUIRE (c.quality == ChordQuality::Minor);
        REQUIRE (c.rootNote == 9);
        REQUIRE (c.name == "Am");
    }
    SECTION ("C power chord")
    {
        const auto c = a.analyze ({ 60, 67 });
        REQUIRE (c.quality == ChordQuality::Power5);
        REQUIRE (c.rootNote == 0);
    }
}

TEST_CASE ("ChordAnalyzer: seventh chords", "[chord]")
{
    ChordAnalyzer a;

    SECTION ("G dominant 7")
    {
        const auto c = a.analyze ({ 55, 59, 62, 65 });
        REQUIRE (c.quality == ChordQuality::Dominant7);
        REQUIRE (c.rootNote == 7);
        REQUIRE (c.name == "G7");
    }
    SECTION ("C major 7")
    {
        const auto c = a.analyze ({ 60, 64, 67, 71 });
        REQUIRE (c.quality == ChordQuality::Major7);
        REQUIRE (c.rootNote == 0);
    }
    SECTION ("C minor 7")
    {
        const auto c = a.analyze ({ 60, 63, 67, 70 });
        REQUIRE (c.quality == ChordQuality::Minor7);
        REQUIRE (c.rootNote == 0);
    }
}

TEST_CASE ("ChordAnalyzer: first inversion keeps root, reports bass", "[chord]")
{
    ChordAnalyzer a;
    // E G C — C major over E (first inversion).
    const auto c = a.analyze ({ 64, 67, 72 });
    REQUIRE (c.quality == ChordQuality::Major);
    REQUIRE (c.rootNote == 0);   // still C
    REQUIRE (c.bassNote == 4);   // E
    REQUIRE (c.inversion == 1);
}

TEST_CASE ("ChordAnalyzer: degenerate inputs don't crash", "[chord]")
{
    ChordAnalyzer a;

    SECTION ("empty -> placeholder")
    {
        const auto c = a.analyze ({});
        REQUIRE (c.name == "-");
        REQUIRE (c.rootNote == -1);
    }
    SECTION ("single note -> note name")
    {
        const auto c = a.analyze ({ 60 });
        REQUIRE (c.rootNote == 0);
        REQUIRE (c.name.startsWith ("C"));   // "C4" (note name carries octave)
    }
}

TEST_CASE ("ChordAnalyzer: static name helpers round-trip", "[chord]")
{
    REQUIRE (ChordAnalyzer::pitchClassToName (0) == "C");
    REQUIRE (ChordAnalyzer::pitchClassToName (9) == "A");
    REQUIRE (ChordAnalyzer::pitchClassToName (1, /*useFlats*/ true) == "Db");
    REQUIRE (ChordAnalyzer::noteToName (60) == "C4");
    // nameToNote returns a pitch class (0-11), not a MIDI number.
    REQUIRE (ChordAnalyzer::nameToNote ("G") == 7);
    REQUIRE (ChordAnalyzer::nameToNote ("Bb") == 10);
    // pitch-class -> name -> pitch-class round-trips.
    REQUIRE (ChordAnalyzer::nameToNote (ChordAnalyzer::pitchClassToName (7)) == 7);
}
