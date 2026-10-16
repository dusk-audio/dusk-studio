#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzDirectoryLease.h"
#include "foundation/Fs.h"

#include <filesystem>
#include <string>
#include <utility>

namespace
{
namespace fs = std::filesystem;
using duskstudio::sfz::DirectoryLease;
using Access = DirectoryLease::Access;

struct LeaseTree
{
    LeaseTree() : root (fs::canonical (dusk::fs::createUniqueTempDirectory ("dusk-sfz-lease-"))) {}
    ~LeaseTree() { std::error_code error; fs::remove_all (dusk::fs::nativeIOPath (root), error); }
    fs::path root;
};
}

TEST_CASE ("SFZ directory owners exclude maintenance until their last lease ends", "[sfz][store]")
{
    LeaseTree tree;
    auto root = tree.root;
    SECTION ("Ordinary root") {}
   #if defined(_WIN32)
    SECTION ("Extended root") { root = dusk::fs::nativeIOPath (root); }
    SECTION ("UNC inspection accepts forward separators without erasing traversal")
    {
        std::error_code error;
        const auto inspected = dusk::fs::absolutePathForInspection (L"//server/share/alias/../cache", error);
        CHECK_FALSE (error);
        CHECK (inspected.native() == LR"(\\server\share\alias\..\cache)");
    }
   #endif
    SECTION ("Deep root")
    {
        for (int index = 0; index < 4; ++index) root /= std::string (72, char ('a' + index));
        REQUIRE (fs::create_directories (dusk::fs::nativeIOPath (root)));
    }
    std::string error;
    auto nativeError = std::make_error_code (std::errc::io_error);
    DirectoryLease first, second, maintenance;
    REQUIRE (first.acquire (root, Access::Shared, error, &nativeError));
    CHECK_FALSE (nativeError);
    REQUIRE (second.acquire (root, Access::Shared, error));
    nativeError = std::make_error_code (std::errc::io_error);
    CHECK_FALSE (maintenance.acquire (root, Access::Exclusive, error, &nativeError));
    CHECK (error.empty());
    CHECK_FALSE (nativeError);
    DirectoryLease moved (std::move (first));
    CHECK_FALSE (static_cast<bool> (first));
    REQUIRE (static_cast<bool> (moved));
    first = std::move (second);
    CHECK_FALSE (static_cast<bool> (second));
    moved = {};
    CHECK_FALSE (maintenance.acquire (root, Access::Exclusive, error));
    CHECK (error.empty());
    SECTION ("Move assignment releases the destination's previous directory")
    {
        const auto otherRoot = root / "other";
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (otherRoot)));
        DirectoryLease other, otherMaintenance;
        REQUIRE (other.acquire (otherRoot, Access::Shared, error));
        other = std::move (first);
        REQUIRE (otherMaintenance.acquire (otherRoot, Access::Exclusive, error));
        CHECK_FALSE (maintenance.acquire (root, Access::Exclusive, error));
        CHECK (error.empty());
    }
    first = {};
    REQUIRE (maintenance.acquire (root, Access::Exclusive, error));
    CHECK (error.empty());
    maintenance = {};
    CHECK (fs::is_regular_file (dusk::fs::nativeIOPath (root / ".dusk-sfz.lock")));
    REQUIRE (first.acquire (root, Access::Shared, error));
    CHECK_FALSE (maintenance.acquire (root, Access::Exclusive, error));
    CHECK (error.empty());
}

TEST_CASE ("SFZ directory ownership refuses unsafe files and never creates missing parents", "[sfz][store]")
{
    LeaseTree tree;
    DirectoryLease owner;
    std::string error;
    const auto lockFile = tree.root / ".dusk-sfz.lock";
    SECTION ("Missing directory")
    {
        CHECK_FALSE (owner.acquire (tree.root / "absent", Access::Shared, error));
        CHECK_FALSE (fs::exists (tree.root / "absent"));
    }
    SECTION ("Directory in place of the ownership file")
    {
        REQUIRE (fs::create_directory (lockFile));
        std::error_code nativeError;
        CHECK_FALSE (owner.acquire (tree.root, Access::Shared, error, &nativeError));
        CHECK (nativeError);
        CHECK (nativeError != std::errc::no_space_on_device);
        CHECK (fs::is_directory (lockFile));
    }
    SECTION ("Hard-linked ownership file")
    {
        const auto unrelated = tree.root / "unrelated";
        REQUIRE (dusk::fs::writeStringToFile (unrelated, "keep this"));
        std::error_code linkError;
        fs::create_hard_link (unrelated, lockFile, linkError);
        if (linkError) SKIP ("The filesystem cannot create a hard-link fixture");
        CHECK_FALSE (owner.acquire (tree.root, Access::Shared, error));
        CHECK (dusk::fs::loadFileAsString (unrelated) == "keep this");
    }
    SECTION ("Symbolic ownership file")
    {
        const auto unrelated = tree.root / "unrelated";
        REQUIRE (dusk::fs::writeStringToFile (unrelated, "keep this"));
        std::error_code linkError;
        fs::create_symlink (unrelated, lockFile, linkError);
        if (linkError) SKIP ("The filesystem cannot create a symbolic-link fixture");
        CHECK_FALSE (owner.acquire (tree.root, Access::Shared, error));
        CHECK (dusk::fs::loadFileAsString (unrelated) == "keep this");
    }
    SECTION ("Symbolic directory")
    {
        const auto real = tree.root / "real";
        const auto alias = tree.root / "alias";
        REQUIRE (fs::create_directory (real));
        std::error_code linkError;
        fs::create_directory_symlink (real, alias, linkError);
        if (linkError) SKIP ("The filesystem cannot create a symbolic-directory fixture");
        CHECK_FALSE (owner.acquire (alias, Access::Shared, error));
        CHECK_FALSE (fs::exists (real / ".dusk-sfz.lock"));
    }
    SECTION ("A parent traversal cannot erase a symbolic directory")
    {
        const auto real = tree.root / "real";
        const auto alias = tree.root / "alias";
        REQUIRE (fs::create_directories (real / "sub"));
        REQUIRE (fs::create_directory (real / "cache"));
        REQUIRE (fs::create_directory (tree.root / "cache"));
        std::error_code linkError;
        fs::create_directory_symlink (real / "sub", alias, linkError);
        if (linkError) SKIP ("The filesystem cannot create a symbolic-directory fixture");
        CHECK_FALSE (owner.acquire (alias / ".." / "cache", Access::Shared, error));
        CHECK_FALSE (fs::exists (real / "cache/.dusk-sfz.lock"));
        CHECK_FALSE (fs::exists (tree.root / "cache/.dusk-sfz.lock"));
    }
    CHECK_FALSE (error.empty());
    CHECK_FALSE (static_cast<bool> (owner));
}
