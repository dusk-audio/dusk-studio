#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "foundation/Fs.h"
#include "session/SessionBackups.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace backups = duskstudio::sessionbackups;
namespace stdfs = std::filesystem;

namespace
{
using Clock = std::chrono::system_clock;
using Days = std::chrono::duration<std::int64_t, std::ratio<86400>>;

constexpr std::int64_t kNovember8th143005 = 1794148205;   // 2026-11-08 14:30:05 UTC

Clock::time_point utc (std::int64_t secondsSinceEpoch)
{
    return Clock::time_point (std::chrono::seconds (secondsSinceEpoch));
}

std::vector<std::string> namesIn (const stdfs::path& folder)
{
    std::vector<std::string> names;
    std::error_code error;
    for (stdfs::directory_iterator it (folder, error), end; ! error && it != end; it.increment (error))
        names.push_back (it->path().filename().u8string());
    std::sort (names.begin(), names.end());
    return names;
}

void write (const stdfs::path& file, const std::string& text)
{
    std::ofstream (file, std::ios::binary) << text;
}

std::string read (const stdfs::path& file)
{
    return dusk::fs::loadFileAsString (file);
}

// A backup name for each of count seconds from start.
std::vector<std::string> namesFrom (Clock::time_point start, int count, std::chrono::seconds step)
{
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i)
        names.push_back (backups::fileName (start + i * step));
    return names;
}
} // namespace

TEST_CASE ("A session backup is named by the UTC second it was saved in, with copies after it",
           "[session][backups]")
{
    const auto at = utc (kNovember8th143005);
    CHECK (backups::fileName (at) == "20261108-143005Z_session.json");
    CHECK (backups::fileName (at, 2) == "20261108-143005Z_session_002.json");
    CHECK (backups::fileName (at, 999) == "20261108-143005Z_session_999.json");

    std::vector<std::string> names { backups::fileName (at, 10), backups::fileName (at + std::chrono::seconds (1)),
                                     backups::fileName (at, 2), backups::fileName (at) };
    std::sort (names.begin(), names.end());
    CHECK (names == std::vector<std::string> { "20261108-143005Z_session.json", "20261108-143005Z_session_002.json",
                                               "20261108-143005Z_session_010.json", "20261108-143006Z_session.json" });

    for (const auto& name : names)
        CHECK (backups::isBackupName (name));
    for (const char* name : { "session.json", "20261108-143005Z_session.json.tmp", "20261108-143005Z_session_001.json",
                              "20261108-143005Z_session_1000.json", "20261108-143005Z_session_02.json",
                              "20261108-143005Z_track01_Keys.mid", "20261108_143005Z_session.json",
                              "20261108-143005Z_sessions.json" })
    {
        CAPTURE (name);
        CHECK_FALSE (backups::isBackupName (name));
    }
}

TEST_CASE ("A session backup's name gives back when it was saved", "[session][backups]")
{
    CHECK (backups::savedAtOf ("/x/backups/20261108-143005Z_session.json") == utc (kNovember8th143005));
    CHECK (backups::savedAtOf ("20261108-143005Z_session_002.json") == utc (kNovember8th143005));
    CHECK (backups::savedAtOf ("20240229-000000Z_session.json") == utc (1709164800));
    CHECK_FALSE (backups::savedAtOf ("20261332-143005Z_session.json"));
    CHECK_FALSE (backups::savedAtOf ("20230229-000000Z_session.json"));
    CHECK_FALSE (backups::savedAtOf ("20261108-246005Z_session.json"));
    CHECK_FALSE (backups::savedAtOf ("20261108-143005Z_track01_Keys.mid"));
}

TEST_CASE ("A saved session.json is kept in backups, once for the same bytes, never over another file",
           "[session][backups]")
{
    const duskstudio::test::TempDirectory temp ("dusk-session-backups-saved-");
    const auto dir = temp.path();
    const auto folder = dir / "backups";
    const auto at = utc (kNovember8th143005);

    auto result = backups::keepSaved (dir, at, "{\"version\":11}");
    CHECK (result.failures.empty());
    REQUIRE (result.written == std::vector<stdfs::path> { folder / "20261108-143005Z_session.json" });
    CHECK (read (folder / "20261108-143005Z_session.json") == "{\"version\":11}");

    result = backups::keepSaved (dir, at + std::chrono::seconds (5), "{\"version\":11}");
    CHECK (result.failures.empty());
    CHECK (result.written.empty());

    result = backups::keepSaved (dir, at, "{\"version\":11,\"tracks\":[]}");
    CHECK (result.written == std::vector<stdfs::path> { folder / "20261108-143005Z_session_002.json" });

    SECTION ("a file already under the name stays as it is")
    {
        write (folder / "20261108-143010Z_session.json", "kept by hand");
        result = backups::keepSaved (dir, at + std::chrono::seconds (5), "{}");
        CHECK (result.written == std::vector<stdfs::path> { folder / "20261108-143010Z_session_002.json" });
        CHECK (read (folder / "20261108-143010Z_session.json") == "kept by hand");
        CHECK (read (folder / "20261108-143010Z_session_002.json") == "{}");
    }

    SECTION ("a session folder that has gone is not made again")
    {
        const auto gone = dir / "gone";
        result = backups::keepSaved (gone, at, "{}");
        CHECK (result.written.empty());
        CHECK (result.failures.size() == 1);
        CHECK_FALSE (stdfs::exists (gone));
    }
}

TEST_CASE ("A session.json about to be replaced is kept when no backup holds it", "[session][backups]")
{
    const duskstudio::test::TempDirectory temp ("dusk-session-backups-current-");
    const auto dir = temp.path();
    const auto folder = dir / "backups";
    const auto current = dir / "session.json";

    SECTION ("no session.json keeps nothing")
    {
        const auto result = backups::keepCurrent (dir, utc (kNovember8th143005));
        CHECK (result.written.empty());
        CHECK (result.failures.empty());
        CHECK_FALSE (stdfs::exists (folder));
    }

    SECTION ("a session.json with no backup is kept under the save's time")
    {
        write (current, "{\"edited\":\"by hand\"}");
        const auto result = backups::keepCurrent (dir, utc (kNovember8th143005) + std::chrono::milliseconds (500));
        CHECK (result.failures.empty());
        REQUIRE (result.written == std::vector<stdfs::path> { folder / "20261108-143005Z_session.json" });
        CHECK (read (result.written.front()) == "{\"edited\":\"by hand\"}");

        CHECK (backups::keepCurrent (dir, utc (kNovember8th143005 + 60)).written.empty());
    }

    SECTION ("a session.json the newest backup holds is not kept again")
    {
        write (current, "{}");
        REQUIRE (backups::keepSaved (dir, utc (kNovember8th143005), "{}").written.size() == 1);
        const auto result = backups::keepCurrent (dir, utc (kNovember8th143005 + 60));
        CHECK (result.written.empty());
        CHECK (namesIn (folder) == std::vector<std::string> { "20261108-143005Z_session.json" });
    }

    SECTION ("a session.json that differs from the newest backup is kept beside it")
    {
        REQUIRE (backups::keepSaved (dir, utc (kNovember8th143005), "{}").written.size() == 1);
        write (current, "{\"damaged\"");
        const auto result = backups::keepCurrent (dir, utc (kNovember8th143005 + 3600) + std::chrono::milliseconds (500));
        CHECK (result.written == std::vector<stdfs::path> { folder / "20261108-153005Z_session.json" });
        CHECK (read (folder / "20261108-153005Z_session.json") == "{\"damaged\"");
        CHECK (read (folder / "20261108-143005Z_session.json") == "{}");
    }

    SECTION ("a session.json kept behind twenty newer backups is not taken by retention")
    {
        const auto now = Clock::now();
        stdfs::create_directories (folder);
        for (const auto& name : namesFrom (now - std::chrono::hours (1), 25, std::chrono::seconds (60)))
            write (folder / name, name);
        write (current, "{\"restored\":\"from an old copy\"}");
        const auto kept = backups::keepCurrent (dir, now);
        REQUIRE (kept.written.size() == 1);
        CHECK (read (kept.written.front()) == "{\"restored\":\"from an old copy\"}");
        CHECK (std::find (kept.removed.begin(), kept.removed.end(), kept.written.front()) == kept.removed.end());

        const auto saved = backups::keepSaved (dir, now, "{\"saved\":true}");
        REQUIRE (saved.written.size() == 1);
        CHECK (stdfs::exists (kept.written.front()));
        CHECK (namesIn (folder).back() == saved.written.front().filename().u8string());
    }
}

TEST_CASE ("Session backup retention keeps everything under the newest twenty", "[session][backups]")
{
    const auto now = utc (kNovember8th143005);
    CHECK (backups::namesToRemove (namesFrom (now - std::chrono::hours (2), 20, std::chrono::seconds (60)), now).empty());
    CHECK (backups::namesToRemove (namesFrom (now - Days (400), 20, Days (1)), now).empty());
}

TEST_CASE ("Session backup retention keeps the newest twenty of one day", "[session][backups]")
{
    const auto now = utc (kNovember8th143005);
    const auto names = namesFrom (now - std::chrono::hours (2), 25, std::chrono::seconds (60));
    CHECK (backups::namesToRemove (names, now) == std::vector<std::string> (names.begin(), names.begin() + 5));
}

TEST_CASE ("Session backup retention keeps one a day for thirty days", "[session][backups]")
{
    const auto now = utc (kNovember8th143005);
    // One a day for 40 days, the last today, and a second one on each day.
    std::vector<std::string> names;
    for (int day = 39; day >= 0; --day)
    {
        names.push_back (backups::fileName (now - Days (day) - std::chrono::hours (1)));
        names.push_back (backups::fileName (now - Days (day) - std::chrono::hours (2)));
    }
    const auto removed = backups::namesToRemove (names, now);

    std::vector<std::string> expected;
    for (int day = 39; day >= 0; --day)
    {
        // The 20 newest are the two of each of the last 10 days.
        if (day >= 10) expected.push_back (backups::fileName (now - Days (day) - std::chrono::hours (2)));
        if (day >= 30) expected.push_back (backups::fileName (now - Days (day) - std::chrono::hours (1)));
    }
    std::sort (expected.begin(), expected.end());
    CHECK (removed == expected);
}

TEST_CASE ("Session backup retention counts thirty dates up to today's, UTC", "[session][backups]")
{
    const auto now = utc (kNovember8th143005);
    auto names = namesFrom (now - std::chrono::hours (2), 20, std::chrono::seconds (60));
    names.push_back ("20261010-000000Z_session.json");   // 29 days before
    names.push_back ("20261010-235959Z_session.json");
    names.push_back ("20261009-235959Z_session.json");   // 30 days before
    const auto removed = backups::namesToRemove (names, now);
    CHECK (removed == std::vector<std::string> { "20261009-235959Z_session.json", "20261010-000000Z_session.json" });
}

TEST_CASE ("Session backup retention leaves every other file alone", "[session][backups]")
{
    const auto now = utc (kNovember8th143005);
    auto names = namesFrom (now - Days (100), 30, std::chrono::seconds (1));
    for (const char* foreign : { "notes.txt", "20200101-000000Z_session.json.tmp", "20200101-000000Z_track01_Keys.mid",
                                 "session.json", ".dusk-1-2.tmp" })
        names.push_back (foreign);
    const auto removed = backups::namesToRemove (names, now);
    CHECK (removed.size() == 10);
    for (const auto& name : removed)
        CHECK (backups::isBackupName (name));
}

TEST_CASE ("Session backup retention keeps the newest of each date after today", "[session][backups]")
{
    const auto now = utc (kNovember8th143005);
    auto names = namesFrom (now + Days (1), 2, std::chrono::seconds (1));
    const auto later = namesFrom (now + Days (2), 20, std::chrono::seconds (1));
    names.insert (names.end(), later.begin(), later.end());
    CHECK (backups::namesToRemove (names, now) == std::vector<std::string> { names.front() });
}

TEST_CASE ("Keeping a session backup applies retention to the backups folder", "[session][backups]")
{
    const duskstudio::test::TempDirectory temp ("dusk-session-backups-retain-");
    const auto dir = temp.path();
    const auto folder = dir / "backups";
    stdfs::create_directory (folder);
    const auto old = namesFrom (Clock::now() - Days (100), 25, std::chrono::seconds (1));
    for (const auto& name : old)
        write (folder / name, name);
    write (folder / "notes.txt", "mine");

    const auto result = backups::keepSaved (dir, Clock::now(), "{}");
    CHECK (result.failures.empty());
    REQUIRE (result.written.size() == 1);
    std::vector<stdfs::path> expected;
    for (int i = 0; i < 6; ++i)
        expected.push_back (folder / old[(std::size_t) i]);
    CHECK (result.removed == expected);
    CHECK (namesIn (folder).size() == 21);
    CHECK (read (folder / "notes.txt") == "mine");
}

TEST_CASE ("The newest session backup, and the newest before another one", "[session][backups]")
{
    const duskstudio::test::TempDirectory temp ("dusk-session-backups-newest-");
    const auto dir = temp.path();
    const auto folder = dir / "backups";
    CHECK_FALSE (backups::newest (dir));

    stdfs::create_directory (folder);
    for (const char* name : { "20261107-091500Z_session.json", "20261108-143005Z_session.json",
                              "20261108-143005Z_session_002.json", "notes.json" })
        write (folder / name, name);
    stdfs::create_directory (folder / "20261109-000000Z_session.json");

    CHECK (backups::newest (dir) == folder / "20261108-143005Z_session_002.json");
    CHECK (backups::newest (dir, folder / "20261108-143005Z_session_002.json")
           == folder / "20261108-143005Z_session.json");
    CHECK (backups::newest (dir, folder / "20261108-143005Z_session.json")
           == folder / "20261107-091500Z_session.json");
    CHECK_FALSE (backups::newest (dir, folder / "20261107-091500Z_session.json"));
    CHECK (backups::newest (dir, dir / "session.json") == folder / "20261108-143005Z_session_002.json");
    CHECK (backups::newest (dir, dir / "other" / "backups" / "20261108-143005Z_session.json")
           == folder / "20261108-143005Z_session_002.json");
}

TEST_CASE ("A backup belongs to the session folder its backups folder is in", "[session][backups]")
{
    const stdfs::path song = stdfs::path ("/music") / "Song";
    CHECK (backups::sessionFolderOf (song / "backups" / "20261108-143005Z_session.json") == song);
    CHECK (backups::sessionFolderOf (song / "backups" / "20261108-143005Z_session_002.json") == song);
    CHECK_FALSE (backups::sessionFolderOf (song / "session.json"));
    CHECK_FALSE (backups::sessionFolderOf (song / "20261108-143005Z_session.json"));
    CHECK_FALSE (backups::sessionFolderOf (song / "old" / "20261108-143005Z_session.json"));
    CHECK_FALSE (backups::sessionFolderOf (song / "backups" / "session.json"));
    CHECK_FALSE (backups::sessionFolderOf (song / "backups" / "20261108-143005Z_session.json.autosave"));
}

TEST_CASE ("Two files hold the same bytes only when both read and match", "[session][backups]")
{
    const duskstudio::test::TempDirectory temp ("dusk-session-backups-same-");
    const auto a = temp.path() / "a.json", b = temp.path() / "b.json", c = temp.path() / "c.json";
    write (a, "{}");
    write (b, "{}");
    write (c, "{ }");
    CHECK (backups::sameBytes (a, b));
    CHECK_FALSE (backups::sameBytes (a, c));
    CHECK_FALSE (backups::sameBytes (a, temp.path() / "missing.json"));
    CHECK_FALSE (backups::sameBytes (temp.path() / "missing.json", temp.path() / "missing.json"));
}
