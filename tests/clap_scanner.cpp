// CLAP discovery: ClapScanner enumerates *.clap files + reads their descriptors.
// The path-shape cases run everywhere; the live load+enumerate case is gated on
// DUSKSTUDIO_TEST_CLAP=/path/to.clap (e.g. ~/.clap/DuskVerb.clap) so CI without a
// CLAP plugin stays green. See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapScanner.h"

#include <cstdlib>

using duskstudio::clap::ClapScanner;

TEST_CASE ("ClapScanner default search paths are existing directories", "[clap][scan]")
{
    for (const auto& d : ClapScanner::defaultSearchPaths())
        REQUIRE (d.isDirectory());
}

TEST_CASE ("ClapScanner finds nothing in an empty directory", "[clap][scan]")
{
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("dusk_clap_scan_empty");
    tmp.deleteRecursively();
    REQUIRE (tmp.createDirectory());

    REQUIRE (ClapScanner::findClapFiles ({ tmp }).empty());
    REQUIRE (ClapScanner::scan ({ tmp }).empty());

    tmp.deleteRecursively();
}

TEST_CASE ("ClapScanner discovers and describes a real CLAP bundle", "[clap][scan]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-scan test");
        return;
    }

    const juce::File bundle (juce::String (path).trim());
    REQUIRE (bundle.existsAsFile());

    const auto found = ClapScanner::scan ({ bundle.getParentDirectory() });
    REQUIRE_FALSE (found.empty());

    bool sawBundle = false;
    for (const auto& s : found)
    {
        if (s.bundlePath == bundle.getFullPathName())
        {
            sawBundle = true;
            REQUIRE_FALSE (s.desc.id.empty());     // a usable plugin id to instantiate
            REQUIRE_FALSE (s.desc.name.empty());   // a human-readable name for the picker
        }
    }
    REQUIRE (sawBundle);
}
