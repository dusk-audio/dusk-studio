#include <catch2/catch_test_macros.hpp>

#include "engine/PluginBackingCheck.h"

#include <juce_core/juce_core.h>

using duskstudio::pluginBackingLooksDead;

// A cached plugin entry should be pruned only when its backing is positively
// gone or hollow — never on a guess. The matrix below pins the four real cases
// plus the LV2-URI escape hatch so a future change can't start dropping valid
// plugins (the clear-and-rescan approach this replaced dropped soundfonts and
// headless-unscannable LV2 plugins; the heuristic must stay conservative).

TEST_CASE ("pluginBackingLooksDead never prunes URI identifiers (LV2)")
{
    // LV2 stores a plugin URI, not a filesystem path. It can't be cheaply
    // validated, so it must always read as alive — otherwise every LV2 in the
    // cache would be wiped.
    REQUIRE_FALSE (pluginBackingLooksDead ("https://dusk-audio.github.io/plugins/duskverb"));
    REQUIRE_FALSE (pluginBackingLooksDead ("http://lv2plug.in/plugins/eg-amp"));
    REQUIRE_FALSE (pluginBackingLooksDead ("urn:dusk:plugin:thing"));
}

TEST_CASE ("pluginBackingLooksDead classifies filesystem backings")
{
    auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("dusk_backing_check_"
                                     + juce::String (juce::Random::getSystemRandom().nextInt()));
    root.deleteRecursively();
    root.createDirectory();

    SECTION ("a path that no longer exists is dead")
    {
        auto gone = root.getChildFile ("ghost.vst3");
        REQUIRE_FALSE (gone.exists());
        REQUIRE (pluginBackingLooksDead (gone.getFullPathName()));
    }

    SECTION ("a present single-file backing is alive")
    {
        auto f = root.getChildFile ("plugin.so");
        f.replaceWithText ("ELF");
        REQUIRE_FALSE (pluginBackingLooksDead (f.getFullPathName()));
    }

    SECTION ("an empty bundle directory is dead (hollow / broken install)")
    {
        // The real DuskAmp.vst3 symptom: bundle + Contents/<arch>/ exist but no
        // binary was ever written inside.
        auto bundle = root.getChildFile ("Hollow.vst3");
        bundle.getChildFile ("Contents/x86_64-linux").createDirectory();
        REQUIRE (bundle.isDirectory());
        REQUIRE (pluginBackingLooksDead (bundle.getFullPathName()));
    }

    SECTION ("a bundle directory holding any file is alive")
    {
        // One file anywhere inside is enough — the binary has no extension on
        // macOS, so the check counts files, not suffixes.
        auto bundle = root.getChildFile ("Valid.vst3");
        bundle.getChildFile ("Contents/x86_64-linux").createDirectory();
        bundle.getChildFile ("Contents/x86_64-linux/Valid.so").replaceWithText ("ELF");
        REQUIRE_FALSE (pluginBackingLooksDead (bundle.getFullPathName()));
    }

    root.deleteRecursively();
}
