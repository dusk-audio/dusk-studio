#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestTempDirectory.h"
#include "foundation/AtomicFile.h"
#include "foundation/Fs.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>


#if defined(__linux__)
 #include <sys/stat.h>

namespace
{
struct PublicationFaults;
thread_local PublicationFaults* publicationFaults = nullptr;
struct PublicationFaults
{
    PublicationFaults() { publicationFaults = this; }
    ~PublicationFaults() { publicationFaults = nullptr; }
    int renameError = EINVAL, linkError = 0, unlinkError = 0, syncError = 0;
    int links = 0, unlinks = 0, folderSyncs = 0;
    bool raceTarget = false;
};
}

extern "C" int __real_renameat2 (int, const char*, int, const char*, unsigned);
extern "C" int __real_link (const char*, const char*);
extern "C" int __real_unlink (const char*);
extern "C" int __real_fsync (int);

extern "C" int __wrap_renameat2 (int sourceDir, const char* source, int targetDir, const char* target, unsigned flags)
{
    if (publicationFaults != nullptr && flags == RENAME_NOREPLACE)
    {
        errno = publicationFaults->renameError;
        return -1;
    }
    return __real_renameat2 (sourceDir, source, targetDir, target, flags);
}

extern "C" int __wrap_link (const char* source, const char* target)
{
    if (publicationFaults != nullptr)
    {
        ++publicationFaults->links;
        if (publicationFaults->linkError != 0)
        {
            errno = publicationFaults->linkError;
            return -1;
        }
        if (publicationFaults->raceTarget)
        {
            const int handle = ::open (target, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (handle < 0) return -1;
            const auto written = ::write (handle, "foreign", 7);
            const int closed = ::close (handle);
            if (written != 7 || closed != 0) { errno = EIO; return -1; }
        }
    }
    return __real_link (source, target);
}

extern "C" int __wrap_unlink (const char* path)
{
    if (publicationFaults != nullptr)
    {
        ++publicationFaults->unlinks;
        if (publicationFaults->unlinkError != 0)
        {
            errno = publicationFaults->unlinkError;
            return -1;
        }
    }
    return __real_unlink (path);
}

extern "C" int __wrap_fsync (int handle)
{
    struct stat info {};
    if (publicationFaults != nullptr && ::fstat (handle, &info) == 0 && S_ISDIR (info.st_mode))
    {
        ++publicationFaults->folderSyncs;
        if (publicationFaults->syncError != 0)
        {
            errno = publicationFaults->syncError;
            return -1;
        }
    }
    return __real_fsync (handle);
}
#endif

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

#if defined(__linux__)
TEST_CASE ("Atomic create falls back to hard links only when no-replace rename is unsupported",
           "[foundation][fs][atomic][atomic-fallback]")
{
    const duskstudio::test::TempDirectory temp ("dusk-atomic-fallback-");
    const auto target = temp.path() / "take.mid";
    PublicationFaults faults;
    faults.renameError = GENERATE (ENOSYS, EINVAL, EOPNOTSUPP);
    bool succeeds = true, published = true, temporaryRemains = false;
    int expectedError = 0, expectedLinks = 1, expectedUnlinks = 1;
    std::string expectedBytes = "complete";
    auto mode = dusk::fs::AtomicWriteMode::CreateNew;
    SECTION ("unsupported no-replace publishes complete bytes") {}
    SECTION ("a destination created before link wins the race")
    {
        faults.raceTarget = true;
        succeeds = published = false;
        expectedError = EEXIST;
        expectedBytes = "foreign";
    }
    SECTION ("link failure preserves its error and cleans the temporary file")
    {
        faults.linkError = expectedError = EPERM;
        succeeds = published = false;
        expectedBytes.clear();
    }
    SECTION ("an unrelated rename error is not retried with link")
    {
        faults.renameError = expectedError = EACCES;
        succeeds = published = false;
        expectedLinks = 0;
        expectedBytes.clear();
    }
    SECTION ("temporary cleanup failure does not undo publication")
    {
        faults.unlinkError = EACCES;
        temporaryRemains = true;
    }
    SECTION ("folder sync failure still reports failed durability")
    {
        faults.syncError = expectedError = EIO;
        succeeds = false;
    }
    SECTION ("ordinary replacement keeps using rename")
    {
        mode = dusk::fs::AtomicWriteMode::Replace;
        expectedLinks = expectedUnlinks = 0;
    }
    std::error_code error;
    CHECK (dusk::fs::writeFileAtomically (target, "complete", 8, error, mode) == succeeds);
    CHECK (error == (expectedError == 0 ? std::error_code {} : std::error_code (expectedError, std::generic_category())));
    CHECK (faults.links == expectedLinks);
    CHECK (faults.unlinks == expectedUnlinks);
    CHECK (faults.folderSyncs == (published ? 1 : 0));
    const auto names = namesIn (temp.path());
    if (expectedBytes.empty())
    {
        CHECK_FALSE (stdfs::exists (target));
        CHECK (names.empty());
    }
    else
    {
        CHECK (dusk::fs::loadFileAsString (target) == expectedBytes);
        CHECK (names.size() == (temporaryRemains ? 2 : 1));
        if (temporaryRemains && names.size() == 2)
            for (const auto& name : names)
                CHECK (stdfs::equivalent (temp.path() / name, target));
    }
}
#endif
