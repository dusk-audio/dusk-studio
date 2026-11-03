// Loads an LV2 bundle via lilv and enumerates the plugins it advertises. The
// live case is gated on DUSKSTUDIO_TEST_LV2=/path/to/bundle.lv2 so CI without an
// LV2 plugin stays green; the failure case always runs and pins lilv linkage.

#include <catch2/catch_test_macros.hpp>

#include "engine/PluginSearchPaths.h"
#include "engine/lv2/Lv2Bundle.h"
#include "engine/lv2/Lv2Scanner.h"
#include "TestProcessEnvironment.h"
#include "TestTempDirectory.h"

#include <lilv/lilv.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_CASE ("Lv2Bundle rejects a non-bundle path cleanly", "[lv2][bundle]")
{
    duskstudio::lv2::Lv2Bundle bundle;
    std::string err;
    REQUIRE_FALSE (bundle.load ("/nonexistent/path/to/nowhere.lv2", err));
    REQUIRE_FALSE (bundle.isLoaded());
    REQUIRE_FALSE (err.empty());
    REQUIRE (bundle.plugins().empty());
    REQUIRE (bundle.world() == nullptr);
}

TEST_CASE ("LV2 discovery cancels and loads a real bundle",
           "[lv2][bundle][scanner][regression][issue-466]")
{
    std::atomic<bool> abort { true };
    CHECK (duskstudio::lv2::Lv2Scanner::scan (&abort).empty());

    const char* path = std::getenv ("DUSKSTUDIO_TEST_LV2");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_LV2 not set — skipping live LV2-bundle test");
        return;
    }

    duskstudio::lv2::Lv2Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE (bundle.isLoaded());
    REQUIRE_FALSE (bundle.plugins().empty());
    REQUIRE (bundle.world() != nullptr);

    const auto& first = bundle.plugins().front();
    REQUIRE_FALSE (first.uri.empty());
    REQUIRE (bundle.pluginByUri (first.uri) != nullptr);
    REQUIRE (bundle.pluginByUri ("http://example.org/does-not-exist") == nullptr);
}

// Instrument classification drives the native-instrument picker rows: an
// atom/MIDI-in + audio-out + no-audio-in plugin must describe as an
// instrument. Gated like the live bundle test so CI without a synth stays
// green (point it at e.g. /usr/lib64/lv2/sfizz.lv2 locally).
TEST_CASE ("Lv2Bundle classifies an instrument bundle", "[lv2][bundle]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_LV2_INSTRUMENT");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_LV2_INSTRUMENT not set — skipping");
        return;
    }

    duskstudio::lv2::Lv2Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE_FALSE (bundle.plugins().empty());

    const auto& plugins = bundle.plugins();
    REQUIRE (std::any_of (plugins.begin(), plugins.end(),
                          [] (const auto& p) { return p.isInstrument; }));
}

namespace
{
void writeLv2Bundle (const std::filesystem::path& bundle, const std::string& uri)
{
    std::filesystem::create_directories (bundle);
    std::ofstream manifest (bundle / "manifest.ttl");
    manifest << "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
             << '<' << uri << "> a lv2:Plugin ;\n"
                " doap:name \"Load once fixture\" ; lv2:binary <missing.so> ;\n"
                " lv2:port [ a lv2:InputPort, lv2:AudioPort ; lv2:index 0 ; lv2:symbol \"in\" ; lv2:name \"Input\" ],\n"
                "          [ a lv2:OutputPort, lv2:AudioPort ; lv2:index 1 ; lv2:symbol \"out\" ; lv2:name \"Output\" ] .\n";
    REQUIRE (manifest.good());
}
} // namespace

TEST_CASE ("LV2 discovery loads a bundle on LV2_PATH once when a search folder reaches it too",
           "[lv2][scanner][regression][issue-936]")
{
    namespace fs = std::filesystem;
    const duskstudio::test::TempDirectory temp ("dusk-lv2-load-once-");
    const auto root = fs::canonical (temp.path());
    const auto installed = root / "installed";
    const auto custom = root / "custom";
    const std::string shared = "urn:duskstudio:test:load-once-shared";
    const std::string extra = "urn:duskstudio:test:load-once-extra";
    writeLv2Bundle (installed / "Shared.lv2", shared);
    writeLv2Bundle (custom / "Vendor" / "Extra.lv2", extra);
    fs::create_directory_symlink (installed, custom / "Linked");

    const duskstudio::test::ScopedEnvironmentVariable home ("HOME", root.u8string());
    const duskstudio::test::ScopedEnvironmentVariable lv2Path ("LV2_PATH", "~/installed/");
    const duskstudio::test::ScopedEnvironmentVariable config ("DUSKSTUDIO_CONFIG_DIR", (root / "config").u8string());
    std::string error;
    REQUIRE (duskstudio::pluginpaths::save ({ custom }, error));

    // Without a reload warning from this lilv there is nothing to observe.
    const auto probe = duskstudio::test::captureStderr (root, [&]
    {
        LilvWorld* world = lilv_world_new();
        LilvNode* uri = lilv_new_file_uri (world, nullptr, (installed / "Shared.lv2/").c_str());
        lilv_world_load_bundle (world, uri);
        lilv_world_load_bundle (world, uri);
        lilv_node_free (uri);
        lilv_world_free (world);
    });
    const bool reportsReloads = probe.find ("Reloading plugin") != std::string::npos;
    if (! reportsReloads) WARN ("this lilv does not report reloads, so a second load is invisible");

    std::vector<duskstudio::lv2::ScannedLv2> scanned;
    bool walkError = true;
    const auto output = duskstudio::test::captureStderr (root, [&]
    { scanned = duskstudio::lv2::Lv2Scanner::scan (nullptr, &walkError); });
    INFO (output);
    CHECK_FALSE (walkError);
    if (reportsReloads)
    {
        CHECK (output.find ("Reloading plugin") == std::string::npos);
        CHECK (output.find ("Duplicate plugin") == std::string::npos);
    }

    const auto rows = [&] (const std::string& uri)
    {
        return std::count_if (scanned.begin(), scanned.end(), [&] (const auto& row) { return row.desc.uri == uri; });
    };
    CHECK (rows (shared) == 1);
    CHECK (rows (extra) == 1);
}
