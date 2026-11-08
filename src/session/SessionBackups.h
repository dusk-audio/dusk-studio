#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Copies of session.json under <session>/backups, so a session.json that is
// damaged, or replaced by a save the user regrets, still has its earlier
// states beside it. Every save leaves a copy of what it wrote, so the newest
// backup is the last save, and a session.json changed since by anything but a
// save is kept before the next save replaces it.
//
// "20261108-143005Z_session.json" names a backup by the UTC second it was
// saved in; further backups of that second end "_002" to "_999" before
// ".json". A plain name sort is the order they were made in.
//
// A backup is never replaced. Retention, after each write, is the only thing
// that removes files there, and only files with backup names: it keeps the 20
// newest and the newest of each of the last 30 days (UTC), today included.
//
// Nothing here throws. The session folder must exist; backups/ is made in it
// when needed, and a session folder that has gone is never made again.
namespace duskstudio::sessionbackups
{
inline constexpr int kKeepNewest = 20;
inline constexpr int kKeepDays   = 30;

struct Failure
{
    std::filesystem::path path;   // the file or folder the step was on
    std::error_code       error;
};

struct Result
{
    std::vector<std::filesystem::path> written;
    std::vector<std::filesystem::path> removed;
    std::vector<Failure>               failures;
};

std::string fileName (std::chrono::system_clock::time_point savedAt, int copy = 1);
bool isBackupName (std::string_view name) noexcept;

// Before a save replaces session.json: keeps a copy of it named keptAt, the
// save's own time, unless the newest backup already holds its bytes. Nothing
// when there is no session.json.
Result keepCurrent (const std::filesystem::path& sessionDir, std::chrono::system_clock::time_point keptAt) noexcept;

// After a save wrote bytes to session.json: keeps a copy named savedAt, unless
// the newest backup already holds exactly these bytes.
Result keepSaved (const std::filesystem::path& sessionDir, std::chrono::system_clock::time_point savedAt,
                  std::string_view bytes) noexcept;

// Which of names retention removes at now: of the backup names only, every
// one that is neither among the kKeepNewest newest nor the newest of its UTC
// date, for the kKeepDays dates up to now's and any date after it.
std::vector<std::string> namesToRemove (std::vector<std::string> names,
                                        std::chrono::system_clock::time_point now);

// The newest backup of the session, or, when before is a backup in its
// backups folder, the newest one older than that.
std::optional<std::filesystem::path> newest (const std::filesystem::path& sessionDir,
                                             const std::filesystem::path& before = {});

// The session folder a backup belongs to: for a file with a backup name in a
// folder named backups, the folder above it.
std::optional<std::filesystem::path> sessionFolderOf (const std::filesystem::path& file);

// When a backup was saved, from its name.
std::optional<std::chrono::system_clock::time_point> savedAtOf (const std::filesystem::path& backup);

// Whether two files hold the same bytes; false when either cannot be read.
bool sameBytes (const std::filesystem::path& a, const std::filesystem::path& b) noexcept;
} // namespace duskstudio::sessionbackups
