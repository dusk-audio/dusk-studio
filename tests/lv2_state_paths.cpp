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

    // Compare as paths so the separator is platform-agnostic (toAbsolute
    // returns a native-separator string).
    REQUIRE (stdfs::path (toAbsolute (dir, "samples/kick.wav"))
             == (dir / "cur" / "samples" / "kick.wav").lexically_normal());

    // An already-absolute abstract path is left untouched.
    const auto abs = (stdfs::current_path() / "shared" / "ir.wav").u8string();
    REQUIRE (toAbsolute (dir, abs) == abs);
}

TEST_CASE ("LV2 state paths: absolute under cur/ becomes relative", "[lv2][state]")
{
    const stdfs::path dir = "/session/state/lv2/track01";

    const auto under = (dir / "cur" / "samples" / "kick.wav").u8string();
    REQUIRE (toAbstract (dir, under) == "samples/kick.wav");

    // Files outside cur/ (and cur/ itself) pass through unchanged.
    REQUIRE (toAbstract (dir, "/etc/passwd") == "/etc/passwd");
    const auto curStr = (dir / "cur").u8string();
    REQUIRE (toAbstract (dir, curStr) == curStr);
}

TEST_CASE ("LV2 state paths: refuse escaping abstract paths", "[lv2][state]")
{
    const stdfs::path dir = "/session/state/lv2/track01";
    // A blob that tries to climb out of cur/ is handed back unresolved.
    REQUIRE (toAbsolute (dir, "../../secret.wav") == "../../secret.wav");
}

TEST_CASE ("LV2 state paths: abstract<->absolute round-trips under cur/", "[lv2][state]")
{
    const stdfs::path dir = "/session/state/lv2/track01";
    const std::string abstractPath = "banks/piano/A0.flac";

    const auto absolute = toAbsolute (dir, abstractPath);
    REQUIRE (toAbstract (dir, absolute) == abstractPath);
}
