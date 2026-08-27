// LV2 state:mapPath abstract<->absolute translation. The live file-backed path
// is only exercised by a gated integration test needing a file-writing plugin,
// so the pure path logic is unit-tested here directly.

#include <catch2/catch_test_macros.hpp>

#include "engine/lv2/Lv2StatePaths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace duskstudio::lv2::statepaths;
namespace stdfs = std::filesystem;

namespace
{
class TempDirectory
{
public:
    TempDirectory()
    {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            value = stdfs::temp_directory_path()
                  / ("dusk-lv2-state-generation-" + std::to_string (tick)
                     + "-" + std::to_string (attempt));
            std::error_code ec;
            if (stdfs::create_directory (value, ec)) return;
            if (ec)
                throw std::runtime_error ("could not create LV2 state test directory");
            // Another process atomically won this candidate; try the next.
        }
        throw std::runtime_error ("could not allocate LV2 state test directory");
    }

    ~TempDirectory()
    {
        std::error_code ignored;
        stdfs::remove_all (value, ignored);
    }

    const stdfs::path& path() const noexcept { return value; }

private:
    stdfs::path value;
};

void writeText (const stdfs::path& path, const char* text)
{
    stdfs::create_directories (path.parent_path());
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    out << text;
    if (! out) throw std::runtime_error ("could not write LV2 state test file");
}

std::string readText (const stdfs::path& path)
{
    std::ifstream in (path);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}
} // namespace

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
    REQUIRE (toAbsolute (dir, "").empty());
    REQUIRE (toAbsolute (dir, ".") == ".");
    REQUIRE (toAbsolute (dir, "./") == "./");
    REQUIRE (toAbsolute (dir, "samples/..") == "samples/..");
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

#if defined(__linux__) || defined(__APPLE__)
TEST_CASE ("LV2 state paths: canonical state root handles a symlink alias",
           "[lv2][state][regression][issue-357]")
{
    TempDirectory temp;
    const auto realRoot = temp.path() / "real";
    const auto aliasRoot = temp.path() / "alias";
    stdfs::create_directories (realRoot);
    stdfs::create_directory_symlink (realRoot, aliasRoot);

    const auto canonicalRoot = stdfs::weakly_canonical (realRoot);
    const auto stateDir = normalizeStateDirectory (aliasRoot / "state");
    REQUIRE (stateDir == canonicalRoot / "state");

    const auto payload = canonicalRoot / "state" / "cur" / "payload.txt";
    REQUIRE (toAbstract (stateDir, payload.u8string()) == "payload.txt");
}
#endif

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

TEST_CASE ("LV2 file-backed save stages before rotating cur",
           "[lv2][state][regression][issue-357]")
{
    TempDirectory temp;
    const auto curSample = temp.path() / "cur" / "samples" / "voice.wav";
    writeText (curSample, "generation one");

    std::error_code ec;
    const auto next = prepareNextGeneration (temp.path(), ec);
    REQUIRE_FALSE (ec);
    REQUIRE (next == temp.path() / "next");

    // The plugin may retain this exact absolute path after restore. Preparing
    // the next save must not move it before lilv asks the plugin for state.
    REQUIRE (stdfs::exists (curSample));
    REQUIRE (readText (curSample) == "generation one");

    writeText (next / "samples" / "voice.wav", "generation two");
    REQUIRE (commitNextGeneration (temp.path(), ec));
    REQUIRE_FALSE (ec);
    REQUIRE (readText (temp.path() / "cur" / "samples" / "voice.wav")
             == "generation two");
    REQUIRE (readText (temp.path() / "prev" / "samples" / "voice.wav")
             == "generation one");
    REQUIRE_FALSE (stdfs::exists (temp.path() / "prev.old"));
    REQUIRE_FALSE (stdfs::exists (temp.path() / "cur" / kReadyMarkerName));

    // Lilv serializes paths relative to save_dir. After next/ becomes cur/ the
    // restore mapping must resolve that same abstract path to readable bytes.
    const auto restored = stdfs::path (toAbsolute (temp.path(), "samples/voice.wav"));
    REQUIRE (stdfs::is_regular_file (restored));
    REQUIRE (readText (restored) == "generation two");

    // A second successful save retains exactly one fallback generation.
    const auto third = prepareNextGeneration (temp.path(), ec);
    REQUIRE_FALSE (ec);
    writeText (third / "samples" / "voice.wav", "generation three");
    REQUIRE (commitNextGeneration (temp.path(), ec));
    REQUIRE (readText (temp.path() / "cur" / "samples" / "voice.wav")
             == "generation three");
    REQUIRE (readText (temp.path() / "prev" / "samples" / "voice.wav")
             == "generation two");
}

TEST_CASE ("LV2 staging recovers only a complete next generation",
           "[lv2][state][regression][issue-357]")
{
    TempDirectory temp;
    writeText (temp.path() / "next" / "state.bin", "incomplete");

    std::error_code ec;
    const auto fresh = prepareNextGeneration (temp.path(), ec);
    REQUIRE_FALSE (ec);
    REQUIRE (fresh == temp.path() / "next");
    REQUIRE_FALSE (stdfs::exists (fresh / "state.bin"));

    // Simulate a crash after commit marked serialization complete but before it
    // could rename next/ to cur/. The next save promotes the complete state.
    writeText (fresh / "state.bin", "recoverable");
    writeText (fresh / kReadyMarkerName, "ready\n");
    const auto afterRecovery = prepareNextGeneration (temp.path(), ec);
    REQUIRE_FALSE (ec);
    REQUIRE (afterRecovery == temp.path() / "next");
    REQUIRE (readText (temp.path() / "cur" / "state.bin") == "recoverable");
    REQUIRE_FALSE (stdfs::exists (afterRecovery / "state.bin"));

    TempDirectory fallback;
    writeText (fallback.path() / "prev" / "state.bin", "previous");
    writeText (fallback.path() / "next" / "state.bin", "incomplete");
    const auto afterFallback = prepareNextGeneration (fallback.path(), ec);
    REQUIRE_FALSE (ec);
    REQUIRE (readText (fallback.path() / "cur" / "state.bin") == "previous");
    REQUIRE_FALSE (stdfs::exists (afterFallback / "state.bin"));

    // Restore must follow the persisted blob, not whichever rename happened
    // last before a crash. This is the state after next/ became cur/ but before
    // the enclosing session save committed.
    TempDirectory restore;
    writeText (restore.path() / "cur" / kStateFileName, "uncommitted");
    writeText (restore.path() / "prev" / kStateFileName, "persisted");
    writeText (restore.path() / "prev.old" / kStateFileName, "older");
    REQUIRE (recoverGeneration (restore.path(), "persisted", ec));
    REQUIRE_FALSE (ec);
    REQUIRE (readText (restore.path() / "cur" / kStateFileName) == "persisted");
    REQUIRE (readText (restore.path() / "prev" / kStateFileName) == "older");
    REQUIRE_FALSE (stdfs::exists (restore.path() / "rejected"));
}

TEST_CASE ("LV2 failed file-backed save leaves cur and prev untouched",
           "[lv2][state][regression][issue-357]")
{
    TempDirectory temp;
    writeText (temp.path() / "cur"  / "state.bin", "current");
    writeText (temp.path() / "prev" / "state.bin", "previous");

    std::error_code ec;
    const auto next = prepareNextGeneration (temp.path(), ec);
    REQUIRE_FALSE (ec);
    writeText (next / "state.bin", "incomplete");

    // Mirrors a null lilv state or failed Turtle serialization.
    discardNextGeneration (temp.path());
    REQUIRE_FALSE (stdfs::exists (next));
    REQUIRE (readText (temp.path() / "cur"  / "state.bin") == "current");
    REQUIRE (readText (temp.path() / "prev" / "state.bin") == "previous");
}

TEST_CASE ("LV2 restore refuses generations that do not match the persisted blob",
           "[lv2][state][regression][issue-357]")
{
    TempDirectory temp;
    writeText (temp.path() / "cur" / kStateFileName, "newer state");
    writeText (temp.path() / "prev" / kStateFileName, "older state");

    std::error_code ec;
    REQUIRE_FALSE (recoverGeneration (temp.path(), "session backup state", ec));
    REQUIRE (ec == std::errc::state_not_recoverable);
    REQUIRE (readText (temp.path() / "cur" / kStateFileName) == "newer state");
    REQUIRE (readText (temp.path() / "prev" / kStateFileName) == "older state");

    // Old blob-only sessions have no generations and remain parseable.
    TempDirectory legacy;
    REQUIRE (recoverGeneration (legacy.path(), "portable blob-only state", ec));
    REQUIRE_FALSE (ec);
}
