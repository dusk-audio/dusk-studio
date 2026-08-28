// LV2 state:mapPath abstract<->absolute translation. The live file-backed path
// is only exercised by a gated integration test needing a file-writing plugin,
// so the pure path logic is unit-tested here directly.

#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "engine/lv2/Lv2StatePaths.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace duskstudio::lv2::statepaths;
namespace stdfs = std::filesystem;

namespace
{
using TempDirectory = duskstudio::test::TempDirectory;

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
    TempDirectory temp ("dusk-lv2-state-generation-");
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

TEST_CASE ("LV2 state paths: restore makePath stays under cur/", "[lv2][state]")
{
    TempDirectory temp ("dusk-lv2-state-generation-");
    std::error_code ec;

    const auto nested = makeRestorePath (temp.path(), "restore/payload.txt", ec);
    REQUIRE_FALSE (ec);
    REQUIRE (nested == temp.path() / "cur" / "restore" / "payload.txt");
    REQUIRE (stdfs::is_directory (nested.parent_path()));

    for (const auto* unsafe : { "", ".", "../escape", "restore/../../escape" })
    {
        REQUIRE (makeRestorePath (temp.path(), unsafe, ec).empty());
        REQUIRE (ec == std::errc::invalid_argument);
    }

    const auto absolute = (temp.path() / "outside.txt").u8string();
    REQUIRE (makeRestorePath (temp.path(), absolute, ec).empty());
    REQUIRE (ec == std::errc::invalid_argument);
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
    TempDirectory temp ("dusk-lv2-state-generation-");
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
    TempDirectory temp ("dusk-lv2-state-generation-");
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

    TempDirectory fallback ("dusk-lv2-state-generation-");
    writeText (fallback.path() / "prev" / "state.bin", "previous");
    writeText (fallback.path() / "next" / "state.bin", "incomplete");
    const auto afterFallback = prepareNextGeneration (fallback.path(), ec);
    REQUIRE_FALSE (ec);
    REQUIRE (readText (fallback.path() / "cur" / "state.bin") == "previous");
    REQUIRE_FALSE (stdfs::exists (afterFallback / "state.bin"));

    // Restore must follow the persisted blob, not whichever rename happened
    // last before a crash. This is the state after next/ became cur/ but before
    // the enclosing session save committed.
    TempDirectory restore ("dusk-lv2-state-generation-");
    writeText (restore.path() / "cur" / kStateFileName, "uncommitted");
    writeText (restore.path() / "prev" / kStateFileName, "persisted");
    writeText (restore.path() / "prev.old" / kStateFileName, "older");
    REQUIRE (recoverGeneration (restore.path(), "persisted", ec));
    REQUIRE_FALSE (ec);
    REQUIRE (readText (restore.path() / "cur" / kStateFileName) == "persisted");
    REQUIRE (readText (restore.path() / "prev" / kStateFileName) == "uncommitted");
    REQUIRE (readText (restore.path() / "prev.old" / kStateFileName) == "older");
    REQUIRE_FALSE (stdfs::exists (restore.path() / "rejected"));
}

TEST_CASE ("LV2 failed file-backed save leaves cur and prev untouched",
           "[lv2][state][regression][issue-357]")
{
    TempDirectory temp ("dusk-lv2-state-generation-");
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
    TempDirectory temp ("dusk-lv2-state-generation-");
    writeText (temp.path() / "cur" / kStateFileName, "newer state");
    writeText (temp.path() / "prev" / kStateFileName, "older state");

    std::error_code ec;
    REQUIRE_FALSE (recoverGeneration (temp.path(), "session backup state", ec));
    REQUIRE (ec == std::errc::state_not_recoverable);
    REQUIRE (readText (temp.path() / "cur" / kStateFileName) == "newer state");
    REQUIRE (readText (temp.path() / "prev" / kStateFileName) == "older state");

    // The presence of a managed-format marker is enough to make a generation
    // strict. A corrupt ready marker must not be mistaken for markerless legacy
    // cur/ and allowed through the blob parser.
    TempDirectory corruptMarker ("dusk-lv2-state-generation-");
    writeText (corruptMarker.path() / "cur" / kReadyMarkerName, "corrupt\n");
    REQUIRE_FALSE (recoverGeneration (
        corruptMarker.path(), "portable blob-only state", ec));
    REQUIRE (ec == std::errc::state_not_recoverable);

    // Old blob-only sessions have no generations and remain parseable.
    TempDirectory legacy ("dusk-lv2-state-generation-");
    REQUIRE (recoverGeneration (legacy.path(), "portable blob-only state", ec));
    REQUIRE_FALSE (ec);

    // An interrupted save that never acquired its ready marker is not a
    // generation. It must not make that same legacy blob ambiguous.
    TempDirectory stagedLegacy ("dusk-lv2-state-generation-");
    writeText (stagedLegacy.path() / "next" / kStateFileName, "incomplete");
    REQUIRE (recoverGeneration (
        stagedLegacy.path(), "portable blob-only state", ec));
    REQUIRE_FALSE (ec);
    REQUIRE_FALSE (stdfs::exists (stagedLegacy.path() / "next"));
}

TEST_CASE ("LV2 restore accepts the empty cur directory shipped by legacy sessions",
           "[lv2][state][regression][issue-385]")
{
    TempDirectory legacy ("dusk-lv2-state-generation-");
    stdfs::create_directories (legacy.path() / "cur");

    std::error_code ec;
    REQUIRE (recoverGeneration (legacy.path(), "legacy serialized state", ec));
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::is_directory (legacy.path() / "cur"));
}

TEST_CASE ("LV2 restore accepts legacy cur and prev file-backed generations",
           "[lv2][state][regression][issue-385]")
{
    TempDirectory legacy ("dusk-lv2-state-generation-");
    const auto currentSample = legacy.path() / "cur" / "samples" / "voice.wav";
    const auto previousSample = legacy.path() / "prev" / "samples" / "voice.wav";
    writeText (currentSample, "current legacy sample bytes");
    writeText (previousSample, "previous legacy sample bytes");

    std::error_code ec;
    REQUIRE (recoverGeneration (legacy.path(), "legacy serialized state", ec));
    REQUIRE_FALSE (ec);
    REQUIRE (stdfs::path (toAbsolute (legacy.path(), "samples/voice.wav"))
             == currentSample);
    REQUIRE (readText (currentSample) == "current legacy sample bytes");
    REQUIRE (readText (previousSample) == "previous legacy sample bytes");

    // A legacy save interrupted after cur/ rotated to prev/ must promote that
    // markerless fallback before resolving the blob's relative file paths.
    TempDirectory interrupted ("dusk-lv2-state-generation-");
    const auto interruptedSample =
        interrupted.path() / "prev" / "samples" / "voice.wav";
    writeText (interruptedSample, "interrupted legacy sample bytes");
    REQUIRE (recoverGeneration (
        interrupted.path(), "legacy serialized state", ec));
    REQUIRE_FALSE (ec);
    const auto promotedSample = interrupted.path() / "cur" / "samples" / "voice.wav";
    REQUIRE (readText (promotedSample) == "interrupted legacy sample bytes");
    REQUIRE_FALSE (stdfs::exists (interrupted.path() / "prev"));
}

TEST_CASE ("LV2 restore makePath does not poison a blob-only session on reopen",
           "[lv2][state][regression][issue-385]")
{
    TempDirectory session ("dusk-lv2-state-generation-");
    constexpr const char* state = "portable blob-only state";
    std::error_code ec;

    // First open has no generated state, so the serialized-state parser is the
    // source of truth. A plugin may call state:makePath while restoring it.
    REQUIRE (recoverGeneration (session.path(), state, ec));
    const auto derived = makeRestorePath (session.path(), "cache/derived.bin", ec);
    REQUIRE_FALSE (ec);
    writeText (derived, "derived on first open");

    // Reopening the unchanged session must still reach that same blob parser;
    // makePath's legacy cur/ is not an incomplete new-format generation.
    REQUIRE (recoverGeneration (session.path(), state, ec));
    REQUIRE_FALSE (ec);
    REQUIRE (readText (derived) == "derived on first open");
}
