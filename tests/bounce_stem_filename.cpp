// Unit tests for BounceEngine::stemOutputFile.
//
// End-to-end stems rendering is exercised manually in Phase 4 tri-OS
// smoke (record a few tracks, bounce stems, check per-track WAVs).
// This test pins the filename derivation rules so a future refactor
// can't silently change how stems are named on disk.

#include <catch2/catch_test_macros.hpp>

#include "engine/BounceEngine.h"

#include <juce_core/juce_core.h>

using duskstudio::BounceEngine;

TEST_CASE ("stemOutputFile builds <dir>/<base>_<NN>_<safe>.wav", "[bounce][stems]")
{
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto base = temp.getChildFile ("mix.wav");

    SECTION ("track index is 1-based + zero-padded to two digits")
    {
        REQUIRE (BounceEngine::stemOutputFile (base, 0, "kick").getFileName()
                  == "mix_01_kick.wav");
        REQUIRE (BounceEngine::stemOutputFile (base, 8, "vox").getFileName()
                  == "mix_09_vox.wav");
        REQUIRE (BounceEngine::stemOutputFile (base, 23, "synth").getFileName()
                  == "mix_24_synth.wav");
    }

    SECTION ("default track name (numeric string matching index) falls back to 'track'")
    {
        // Tracks fresh out of a new session have name == String(idx+1).
        REQUIRE (BounceEngine::stemOutputFile (base, 4, "5").getFileName()
                  == "mix_05_track.wav");
        REQUIRE (BounceEngine::stemOutputFile (base, 0, "").getFileName()
                  == "mix_01_track.wav");
        REQUIRE (BounceEngine::stemOutputFile (base, 11, "   ").getFileName()
                  == "mix_12_track.wav");
    }

    SECTION ("illegal filename characters are scrubbed")
    {
        // juce::File::createLegalFileName replaces path separators + the
        // reserved-on-Windows set. We just assert the result is a legal
        // filename, not the exact substitution character (varies by JUCE
        // version).
        const auto f = BounceEngine::stemOutputFile (base, 2, "lead/synth:01");
        const auto name = f.getFileName();
        REQUIRE (name.startsWith ("mix_03_"));
        REQUIRE (name.endsWith   (".wav"));
        REQUIRE (! name.containsAnyOf ("/\\:?*\"<>|"));
    }

    SECTION ("stem path lives next to the base file")
    {
        const auto f = BounceEngine::stemOutputFile (base, 0, "kick");
        REQUIRE (f.getParentDirectory() == temp);
    }
}
