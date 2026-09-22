#include <catch2/catch_test_macros.hpp>

#include "engine/PluginScanMessage.h"

#include <string>

using duskstudio::scanCompleteBody;
using duskstudio::scanCompleteTitle;

// The scan modal and the alert that follows it both report the same finished
// scan. Running a scan needs a plugin collection; wording the result does not.

TEST_CASE ("scanCompleteTitle: cancelling renames the notice", "[plugin][scan]")
{
    REQUIRE (scanCompleteTitle (false) == "Plugin scan complete");
    REQUIRE (scanCompleteTitle (true) == "Plugin scan cancelled");
}

TEST_CASE ("scanCompleteBody: one plugin is singular, everything else plural",
           "[plugin][scan]")
{
    REQUIRE (scanCompleteBody (1) == "1 new plugin added.");
    REQUIRE (scanCompleteBody (0) == "0 new plugins added.");
    REQUIRE (scanCompleteBody (7) == "7 new plugins added.");
}

TEST_CASE ("scanCompleteBody: a cancelled scan still reports what it added",
           "[plugin][scan]")
{
    // The count is what the scan managed before stopping, not a reset.
    REQUIRE (scanCompleteBody (3) == "3 new plugins added.");
    REQUIRE (scanCompleteTitle (true) == "Plugin scan cancelled");
}
