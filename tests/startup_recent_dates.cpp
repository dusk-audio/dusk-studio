#include <catch2/catch_test_macros.hpp>

#include "TestTempDirectory.h"
#include "ui/imgui/StartupView.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

using duskstudio::imgui::scanRecentSessions;
using duskstudio::test::TempDirectory;

namespace
{
namespace stdfs = std::filesystem;

// Three days back, on the half minute, so the conversion between the file and
// system clocks cannot carry it across a minute boundary.
std::chrono::system_clock::time_point savedAt()
{
    auto stamp = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now() - std::chrono::hours (72));
    std::tm local {};
   #if defined (_WIN32)
    localtime_s (&local, &stamp);
   #else
    localtime_r (&stamp, &local);
   #endif
    stamp += 30 - local.tm_sec;
    return std::chrono::system_clock::from_time_t (stamp);
}

std::string shown (std::chrono::system_clock::time_point when)
{
    const auto stamp = std::chrono::system_clock::to_time_t (when);
    std::tm local {};
   #if defined (_WIN32)
    localtime_s (&local, &stamp);
   #else
    localtime_r (&stamp, &local);
   #endif
    char buffer[32] {};
    std::strftime (buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local);
    return buffer;
}

void stamp (const stdfs::path& path, std::chrono::system_clock::time_point when)
{
    const auto age = std::chrono::system_clock::now() - when;
    stdfs::last_write_time (path, stdfs::file_time_type::clock::now()
                                      - std::chrono::duration_cast<stdfs::file_time_type::duration> (age));
}

void write (const stdfs::path& path)
{
    std::ofstream (path) << "{}";
}

std::string lastModified (const stdfs::path& sessionDir)
{
    const auto rows = scanRecentSessions ({ sessionDir });
    REQUIRE (rows.size() == 1);
    return rows.front().lastModified;
}
} // namespace

TEST_CASE ("a recent session is dated by its saved session file rather than its folder")
{
    TempDirectory root ("dusk-recent-dates");
    const auto session = root.path() / "Song";
    stdfs::create_directories (session);
    const auto when = savedAt();

    SECTION ("session.json")
    {
        write (session / "session.json");
        stamp (session / "session.json", when);
        // What moves the folder's time on Windows: the autosave heartbeat writing beside it.
        write (session / "session.json.autosave");
        CHECK (lastModified (session) == shown (when));
    }

    SECTION ("an autosave alone dates the session it would recover")
    {
        write (session / "session.json.autosave");
        stamp (session / "session.json.autosave", when);
        write (session / "notes.txt");
        CHECK (lastModified (session) == shown (when));
    }

    SECTION ("a folder with no session file falls back to the folder")
    {
        stamp (session, when);
        CHECK (lastModified (session) == shown (when));
    }
}
