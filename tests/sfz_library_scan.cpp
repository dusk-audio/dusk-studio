#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzLibrary.h"
#include "foundation/Fs.h"

#include <atomic>
#include <fstream>
#include <system_error>

using namespace duskstudio::sfz;

namespace
{
namespace stdfs = std::filesystem;

struct ScratchTree
{
    ScratchTree() : root (dusk::fs::createUniqueTempDirectory ("dusk-sfz-library-")) {}
    ~ScratchTree()
    {
        std::error_code ec;
        stdfs::remove_all (root, ec);
    }

    void file (const stdfs::path& relative)
    {
        const auto full = root / relative;
        std::error_code ec;
        stdfs::create_directories (full.parent_path(), ec);
        std::ofstream out (full);
        out << "x";
    }

    stdfs::path root;
};

bool listed (const ScanResult& result, std::string_view name)
{
    for (const auto& entry : result.entries)
        if (entry.displayName == name) return true;
    return false;
}
} // namespace

TEST_CASE ("SfzLibrary lists only soundfonts")
{
    ScratchTree tree;
    tree.file ("piano.sfz");
    tree.file ("bank.sf2");
    tree.file ("readme.txt");
    tree.file ("cover.png");
    tree.file ("notes.sfz.bak");

    const auto result = scanLibraryRoots ({ tree.root });

    CHECK (result.entries.size() == 2);
    CHECK (listed (result, "piano"));
    CHECK (listed (result, "bank"));
    CHECK (result.problems.empty());
    CHECK_FALSE (result.cancelled);
}

TEST_CASE ("SfzLibrary reports a root it cannot use instead of listing nothing")
{
    ScratchTree tree;
    const auto missing = tree.root / "does-not-exist";

    const auto result = scanLibraryRoots ({ missing });

    CHECK (result.entries.empty());
    REQUIRE (result.problems.size() == 1);
    CHECK (result.problems[0].root == missing);
    CHECK_FALSE (result.problems[0].reason.empty());
}

TEST_CASE ("SfzLibrary caps how deep it walks")
{
    ScratchTree tree;
    stdfs::path deep;
    for (int i = 0; i <= kMaxScanDepth + 2; ++i)
        deep /= "d";
    tree.file (deep / "buried.sfz");

    stdfs::path shallow;
    for (int i = 0; i < kMaxScanDepth; ++i)
        shallow /= "s";
    tree.file (shallow / "reachable.sfz");

    const auto result = scanLibraryRoots ({ tree.root });

    CHECK (listed (result, "reachable"));
    CHECK_FALSE (listed (result, "buried"));
}

TEST_CASE ("SfzLibrary does not list the same instrument twice through a loop")
{
    ScratchTree tree;
    tree.file ("packs/one.sfz");

    std::error_code ec;
    stdfs::create_directory_symlink (tree.root, tree.root / "packs" / "loop", ec);
    if (ec) SKIP ("symlinks are not available on this filesystem");

    const auto result = scanLibraryRoots ({ tree.root });

    CHECK (result.entries.size() == 1);
    CHECK (listed (result, "one"));
}

TEST_CASE ("SfzLibrary does not list an instrument twice when roots overlap")
{
    ScratchTree tree;
    tree.file ("packs/one.sfz");

    const auto result = scanLibraryRoots ({ tree.root, tree.root / "packs" });

    CHECK (result.entries.size() == 1);
}

TEST_CASE ("SfzLibrary stops when cancelled")
{
    ScratchTree tree;
    tree.file ("one.sfz");

    std::atomic<bool> cancel { true };
    const auto result = scanLibraryRoots ({ tree.root }, &cancel);

    CHECK (result.cancelled);
    CHECK (result.entries.empty());
}

TEST_CASE ("SfzLibrary reports scan progress once per root")
{
    ScratchTree tree;
    tree.file ("one.sfz");

    std::vector<std::pair<std::size_t, std::size_t>> ticks;
    (void) scanLibraryRoots ({ tree.root, tree.root / "nope" }, nullptr,
                             [&ticks] (std::size_t done, std::size_t total)
                             { ticks.emplace_back (done, total); });

    REQUIRE (ticks.size() == 2);
    CHECK (ticks[0] == std::make_pair (std::size_t (1), std::size_t (2)));
    CHECK (ticks[1] == std::make_pair (std::size_t (2), std::size_t (2)));
}

TEST_CASE ("SfzLibrary filtering is case-insensitive and clears back to everything")
{
    std::vector<LibraryEntry> entries {
        { "/a/Grand Piano.sfz", "Grand Piano", "keys", LibraryFormat::Sfz, 1 },
        { "/a/Rhodes.sfz",      "Rhodes",      "keys", LibraryFormat::Sfz, 1 },
        { "/b/Kit.sf2",         "Kit",         "drums", LibraryFormat::Sf2, 1 },
    };

    CHECK (filterEntries (entries, "").size() == 3);
    CHECK (filterEntries (entries, "piano").size() == 1);
    CHECK (filterEntries (entries, "PIANO").size() == 1);
    CHECK (filterEntries (entries, "keys").size() == 2);
    CHECK (filterEntries (entries, "nothing here").empty());
}

TEST_CASE ("SfzLibrary default roots are absolute and distinct")
{
    const auto roots = defaultLibraryRoots();

    REQUIRE_FALSE (roots.empty());
    for (const auto& root : roots)
        CHECK (root.is_absolute());

    auto sorted = roots;
    std::sort (sorted.begin(), sorted.end());
    CHECK (std::unique (sorted.begin(), sorted.end()) == sorted.end());
}
