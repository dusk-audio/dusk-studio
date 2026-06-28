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

TEST_CASE ("ClapBundle rejects a real shared object that is not a CLAP bundle", "[clap][bundle]")
{
    // libc is a valid shared object that dlopens fine but has no clap_entry, so
    // this exercises the post-dlopen rejection path specifically — load() must get
    // past dlopen, then fail for the missing entry, leaving us unloaded.
    duskstudio::clap::ClapBundle b;
    std::string err;
    REQUIRE_FALSE (b.load ("libc.so.6", err));
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE_FALSE (err.empty());
    INFO ("rejection reason: " << err);
}
