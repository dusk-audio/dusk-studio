// VST3 discovery: Vst3Scanner enumerates *.vst3 bundles + reads their factory
// descriptors. The path-shape cases run everywhere; the live load+enumerate
// case is gated on DUSKSTUDIO_TEST_VST3=/path/to.vst3 so CI without a VST3
// plugin stays green.

#include <catch2/catch_test_macros.hpp>

#include "engine/vst3/Vst3Scanner.h"

#include <cstdlib>

using duskstudio::vst3::Vst3Scanner;

TEST_CASE ("Vst3Scanner default search paths are existing directories", "[vst3][scan]")
{
    for (const auto& d : Vst3Scanner::defaultSearchPaths())
        REQUIRE (d.isDirectory());
}

TEST_CASE ("Vst3Scanner finds nothing in an empty directory", "[vst3][scan]")
{
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("dusk_vst3_scan_empty_"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    tmp.deleteRecursively();
    REQUIRE (tmp.createDirectory());

    REQUIRE (Vst3Scanner::findVst3Bundles ({ tmp }).empty());
    REQUIRE (Vst3Scanner::scan ({ tmp }).empty());

    tmp.deleteRecursively();
}

TEST_CASE ("Vst3Scanner matches bundles without descending into them", "[vst3][scan]")
{
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("dusk_vst3_scan_shape_"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    tmp.deleteRecursively();
    REQUIRE (tmp.createDirectory());

    // Bundle directory with the usual inner .so — only the bundle itself matches.
    auto bundle = tmp.getChildFile ("Fake.vst3");
    REQUIRE (bundle.getChildFile ("Contents/x86_64-linux").createDirectory());
    REQUIRE (bundle.getChildFile ("Contents/x86_64-linux/Fake.so").create());
    // Vendor subdirectory one level down.
    auto nested = tmp.getChildFile ("Vendor/Nested.vst3");
    REQUIRE (nested.getParentDirectory().createDirectory());
    REQUIRE (nested.create());

    const auto found = Vst3Scanner::findVst3Bundles ({ tmp });
    REQUIRE (found.size() == 2);
    bool sawBundle = false, sawNested = false;
    for (const auto& f : found)
    {
        if (f == bundle) sawBundle = true;
        if (f == nested) sawNested = true;
    }
    REQUIRE (sawBundle);
    REQUIRE (sawNested);

    tmp.deleteRecursively();
}

TEST_CASE ("Vst3Scanner discovers and describes a real VST3 module", "[vst3][scan]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_VST3");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_VST3 not set — skipping live VST3-scan test");
        return;
    }

    const juce::File bundle (juce::String (path).trim());
    REQUIRE (bundle.exists());

    const auto found = Vst3Scanner::scan ({ bundle.getParentDirectory() });
    REQUIRE_FALSE (found.empty());

    bool sawBundle = false;
    for (const auto& s : found)
    {
        if (s.bundlePath == bundle.getFullPathName())
        {
            sawBundle = true;
            REQUIRE_FALSE (s.desc.id.empty());     // a usable class id to instantiate
            REQUIRE_FALSE (s.desc.name.empty());   // a human-readable name for the picker
        }
    }
    REQUIRE (sawBundle);
}
