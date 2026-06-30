// Increment 0 of the native CLAP host: the ClapBundle loader compiles, links,
// and fails cleanly on a bad path. A real load+enumerate test arrives with a
// .clap fixture (DuskVerb-as-CLAP, increment 3). See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapBundle.h"

#include <cstdio>
#include <dlfcn.h>
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
    // A valid shared object that dlopens fine but has no clap_entry exercises the
    // post-dlopen rejection path. Resolve the host libc portably (its soname differs
    // across libc / OS) via dladdr instead of hardcoding libc.so.6.
    Dl_info info {};
    if (dladdr (reinterpret_cast<void*> (&std::printf), &info) == 0 || info.dli_fname == nullptr)
    {
        SUCCEED ("dladdr could not resolve a host shared object — skipping");
        return;
    }
    duskstudio::clap::ClapBundle b;
    std::string err;
    REQUIRE_FALSE (b.load (info.dli_fname, err));
    REQUIRE_FALSE (b.isLoaded());
    REQUIRE_FALSE (err.empty());
    INFO ("rejection reason: " << err);
}
