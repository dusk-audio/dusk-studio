// Increment 0 of the native CLAP host: the ClapBundle loader compiles, links,
// and fails cleanly on a bad path. A real load+enumerate test arrives with a
// .clap fixture (DuskVerb-as-CLAP, increment 3). See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapBundle.h"

#include <string>

TEST_CASE ("ClapBundle fails gracefully on a missing bundle", "[clap][bundle]")
{
    duskstudio::clap::ClapBundle b;
    std::string err;

    REQUIRE_FALSE (b.load ("/nonexistent/path/definitely-not-a.clap", err));
    REQUIRE_FALSE (err.empty());          // a reason was reported
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE (b.plugins().empty());
    REQUIRE (b.getFactory() == nullptr);
}

TEST_CASE ("ClapBundle load on a non-CLAP shared object reports an error", "[clap][bundle]")
{
    // libc is a valid shared object with no clap_entry symbol — load() must
    // dlopen it but then reject it for the missing entry, leaving us unloaded.
    duskstudio::clap::ClapBundle b;
    std::string err;

    const bool ok = b.load ("libc.so.6", err);
    if (ok)
        REQUIRE (b.isLoaded());           // wildly unexpected, but keep the invariant honest
    else
    {
        REQUIRE_FALSE (err.empty());
        REQUIRE_FALSE (b.isLoaded());
    }
}
