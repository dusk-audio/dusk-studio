#include <catch2/catch_test_macros.hpp>

#include "engine/NativePluginCache.h"
#include <nlohmann/json.hpp>

using namespace duskstudio;

namespace
{
PluginDescriptor descriptorAt (std::string location)
{
    PluginDescriptor descriptor;
    descriptor.name = "Native";
    descriptor.formatName = "CLAP";
    descriptor.backend = PluginBackend::Native;
    descriptor.location = std::move (location);
    descriptor.pluginId = "org.dusk.native";
    return descriptor;
}
} // namespace

TEST_CASE ("native plugin cache round-trips descriptors and prunes stale locations")
{
    const auto present = descriptorAt ("/plugins/present.clap");
    const auto stale = descriptorAt ("/plugins/stale.clap");
    const auto payload = nativecache::serialize ({ present, stale });

    std::vector<PluginDescriptor> parsed;
    REQUIRE (nativecache::parse (payload,
        [] (std::string_view location) { return location == "/plugins/present.clap"; },
        parsed));
    REQUIRE (parsed.size() == 1);
    CHECK (parsed.front() == present);
}

TEST_CASE ("native plugin cache malformed schema never throws or mutates output")
{
    std::vector<PluginDescriptor> parsed { descriptorAt ("/keep.clap") };
    CHECK_FALSE (nativecache::parse (R"({"version":"1"})", {}, parsed));
    REQUIRE (parsed.size() == 1);
    CHECK (parsed.front().location == "/keep.clap");

    CHECK_FALSE (nativecache::parse (R"({"version":1,"descriptors":{}})", {}, parsed));
    REQUIRE (parsed.size() == 1);
    CHECK (parsed.front().location == "/keep.clap");

    CHECK_FALSE (nativecache::parse (R"({"version":-1,"descriptors":[]})", {}, parsed));
    REQUIRE (parsed.size() == 1);
    CHECK (parsed.front().location == "/keep.clap");
}

TEST_CASE ("native plugin cache skips malformed descriptor rows best-effort")
{
    auto root = nlohmann::json::parse (
        nativecache::serialize ({ descriptorAt ("/valid.clap") }));
    root["descriptors"].push_back (
        nlohmann::json::object ({ { "version", 1 }, { "backend", "native" },
                                  { "unique_id", 4.5 } }));

    std::vector<PluginDescriptor> parsed;
    REQUIRE (nativecache::parse (root.dump(), {}, parsed));
    REQUIRE (parsed.size() == 1);
    CHECK (parsed.front().location == "/valid.clap");
}
