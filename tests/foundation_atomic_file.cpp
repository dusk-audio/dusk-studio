#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "foundation/AtomicFile.h"
#include "foundation/Fs.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace stdfs = std::filesystem;

namespace
{
std::vector<std::string> namesIn (const stdfs::path& folder)
{
    std::vector<std::string> names;
    for (const auto& entry : stdfs::directory_iterator (folder))
        names.push_back (entry.path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

bool writeText (const stdfs::path& target, const std::string& text, std::error_code& error)
{
    return dusk::fs::writeFileAtomically (target, text.data(), text.size(), error);
}
} // namespace

TEST_CASE ("writeFileAtomically creates a file holding exactly the bytes given", "[foundation][fs][atomic]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-create-");
    const auto target = temp.path() / "take.mid";
    std::error_code error;

    SECTION ("bytes of every value")
    {
        const std::string bytes ("MThd\0\0\0\x06\x80\xff", 10);
        REQUIRE (writeText (target, bytes, error));
        CHECK_FALSE (error);
        CHECK (dusk::fs::loadFileAsString (target) == bytes);
    }

    SECTION ("no bytes at all")
    {
        REQUIRE (dusk::fs::writeFileAtomically (target, nullptr, 0, error));
        CHECK_FALSE (error);
        CHECK (stdfs::is_regular_file (target));
        CHECK (stdfs::file_size (target) == 0);
    }
}

TEST_CASE ("writeFileAtomically replaces an existing file whole", "[foundation][fs][atomic]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-replace-");
    const auto target = temp.path() / "take.mid";
    std::error_code error;

    REQUIRE (writeText (target, "the first and longer version", error));
    REQUIRE (writeText (target, "second", error));
    CHECK_FALSE (error);
    CHECK (dusk::fs::loadFileAsString (target) == "second");
}

TEST_CASE ("writeFileAtomically leaves nothing beside the target", "[foundation][fs][atomic]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-tidy-");
    const auto target = temp.path() / "take.mid";
    std::error_code error;

    for (const char* text : { "one", "two", "three" })
        REQUIRE (writeText (target, text, error));
    CHECK (namesIn (temp.path()) == std::vector<std::string> { "take.mid" });
}

TEST_CASE ("writeFileAtomically reports a write it cannot make and leaves nothing behind",
           "[foundation][fs][atomic]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-fail-");
    std::error_code error;

    SECTION ("into a folder that does not exist")
    {
        const auto missing = temp.path() / "missing";
        CHECK_FALSE (writeText (missing / "take.mid", "bytes", error));
        CHECK (error == std::errc::no_such_file_or_directory);
        CHECK_FALSE (stdfs::exists (missing));
        CHECK (namesIn (temp.path()).empty());
    }

    SECTION ("over a folder")
    {
        const auto folder = temp.path() / "take.mid";
        REQUIRE (stdfs::create_directory (folder));
        CHECK_FALSE (writeText (folder, "bytes", error));
        CHECK (error);
        CHECK (stdfs::is_directory (folder));
        CHECK (namesIn (temp.path()) == std::vector<std::string> { "take.mid" });
    }
}

TEST_CASE ("Atomic create publishes complete bytes only under an unused name", "[foundation][fs][atomic]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-create-only-");
    const auto target = temp.path() / "take.mid";
    const std::string bytes ("MThd\0\0\0\x06\x80\xff", 10);
    std::error_code error;
    REQUIRE (dusk::fs::writeFileAtomically (target, bytes.data(), bytes.size(), error,
                                          dusk::fs::AtomicWriteMode::CreateNew));
    CHECK_FALSE (error);
    CHECK (dusk::fs::loadFileAsString (target) == bytes);
    CHECK (namesIn (temp.path()) == std::vector<std::string> { "take.mid" });
}

TEST_CASE ("Atomic create preserves an existing destination and removes its temporary file",
           "[foundation][fs][atomic]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-collision-");
    const auto target = temp.path() / "take.mid";
    std::error_code error;

    SECTION ("regular file")
    {
        REQUIRE (writeText (target, "someone else's file", error));
        CHECK_FALSE (dusk::fs::writeFileAtomically (target, "new", 3, error, dusk::fs::AtomicWriteMode::CreateNew));
        CHECK (error == std::errc::file_exists);
        CHECK (dusk::fs::loadFileAsString (target) == "someone else's file");
    }
    SECTION ("directory")
    {
        REQUIRE (stdfs::create_directory (target));
        CHECK_FALSE (dusk::fs::writeFileAtomically (target, "new", 3, error, dusk::fs::AtomicWriteMode::CreateNew));
        CHECK (error);
        CHECK (stdfs::is_directory (target));
    }
    SECTION ("dangling symlink")
    {
        stdfs::create_symlink ("absent.mid", target, error);
        if (error) SKIP ("the test filesystem cannot create symlinks");
        CHECK_FALSE (dusk::fs::writeFileAtomically (target, "new", 3, error, dusk::fs::AtomicWriteMode::CreateNew));
        CHECK (error == std::errc::file_exists);
        CHECK (stdfs::is_symlink (stdfs::symlink_status (target)));
        CHECK (stdfs::read_symlink (target) == "absent.mid");
        CHECK_FALSE (stdfs::exists (temp.path() / "absent.mid"));
    }
    CHECK (namesIn (temp.path()) == std::vector<std::string> { "take.mid" });
}
