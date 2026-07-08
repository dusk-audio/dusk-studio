// LV2 state:mapPath abstract<->absolute translation. The live file-backed path
// is only exercised by a gated integration test needing a file-writing plugin,
// so the pure path logic is unit-tested here directly.

#include <catch2/catch_test_macros.hpp>

#include "engine/lv2/Lv2StatePaths.h"

#include <filesystem>

using namespace duskstudio::lv2::statepaths;
namespace stdfs = std::filesystem;

TEST_CASE ("LV2 state paths: empty stateDir passes through", "[lv2][state]")
{
    REQUIRE (toAbsolute ({}, "samples/kick.wav") == "samples/kick.wav");
    REQUIRE (toAbstract ({}, "/anywhere/kick.wav") == "/anywhere/kick.wav");
}

TEST_CASE ("LV2 state paths: abstract resolves under cur/", "[lv2][state]")
{
    const stdfs::path dir = "/session/state/lv2/track01";

    REQUIRE (toAbsolute (dir, "samples/kick.wav")
             == "/session/state/lv2/track01/cur/samples/kick.wav");

    // An already-absolute abstract path is left untouched.
    REQUIRE (toAbsolute (dir, "/opt/shared/ir.wav") == "/opt/shared/ir.wav");
}

TEST_CASE ("LV2 state paths: absolute under cur/ becomes relative", "[lv2][state]")
{
    const stdfs::path dir = "/session/state/lv2/track01";

    REQUIRE (toAbstract (dir, "/session/state/lv2/track01/cur/samples/kick.wav")
             == "samples/kick.wav");

    // Files outside cur/ (and cur/ itself) pass through as absolute.
    REQUIRE (toAbstract (dir, "/etc/passwd") == "/etc/passwd");
    REQUIRE (toAbstract (dir, "/session/state/lv2/track01/cur")
             == "/session/state/lv2/track01/cur");
}

TEST_CASE ("LV2 state paths: abstract<->absolute round-trips under cur/", "[lv2][state]")
{
    const stdfs::path dir = "/session/state/lv2/track01";
    const std::string abstractPath = "banks/piano/A0.flac";

    const auto absolute = toAbsolute (dir, abstractPath);
    REQUIRE (toAbstract (dir, absolute) == abstractPath);
}
