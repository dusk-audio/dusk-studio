// Loads an LV2 bundle via lilv and enumerates the plugins it advertises. The
// live case is gated on DUSKSTUDIO_TEST_LV2=/path/to/bundle.lv2 so CI without an
// LV2 plugin stays green; the failure case always runs and pins lilv linkage.

#include <catch2/catch_test_macros.hpp>

#include "engine/lv2/Lv2Bundle.h"

#include <cstdlib>
#include <string>

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

TEST_CASE ("Lv2Bundle loads + enumerates a real LV2 bundle", "[lv2][bundle]")
{
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

    bool anyInstrument = false;
    for (const auto& p : bundle.plugins())
        anyInstrument = anyInstrument || p.isInstrument;
    REQUIRE (anyInstrument);
}
