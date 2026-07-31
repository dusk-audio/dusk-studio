// Load a real .vst3 module through the SDK hosting layer and enumerate its
// classes. Gated on DUSKSTUDIO_TEST_VST3=/path/to.vst3 so CI without a plugin
// stays green; the negative path runs everywhere.

#include <catch2/catch_test_macros.hpp>

#include "engine/vst3/Vst3Bundle.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

TEST_CASE ("Vst3Bundle rejects a nonexistent module cleanly", "[vst3][bundle]")
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto missing = std::filesystem::temp_directory_path()
                         / ("dusk_missing_vst3_" + std::to_string (getpid())
                            + "_" + std::to_string (unique) + ".vst3");
    REQUIRE_FALSE (std::filesystem::exists (missing));

    duskstudio::vst3::Vst3Bundle bundle;
    std::string err;
    REQUIRE_FALSE (bundle.load (missing.u8string(), err));
    REQUIRE_FALSE (err.empty());
    REQUIRE_FALSE (bundle.isLoaded());
    REQUIRE (bundle.plugins().empty());
}

TEST_CASE ("Vst3Bundle loads + enumerates a real module", "[vst3][bundle]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_VST3");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_VST3 not set — skipping live VST3 bundle test");
        return;
    }

    duskstudio::vst3::Vst3Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE (bundle.isLoaded());
    REQUIRE (bundle.module() != nullptr);
    REQUIRE_FALSE (bundle.plugins().empty());

    for (const auto& d : bundle.plugins())
    {
        REQUIRE_FALSE (d.id.empty());     // class UID is the session-persisted id
        REQUIRE_FALSE (d.name.empty());
    }

    // Unload releases the module and clears the descriptors.
    bundle.unload();
    REQUIRE_FALSE (bundle.isLoaded());
    REQUIRE (bundle.plugins().empty());
}
