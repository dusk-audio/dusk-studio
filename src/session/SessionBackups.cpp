#include "SessionBackups.h"

#include "RecoveryStamp.h"
#include "../foundation/AtomicFile.h"
#include "../foundation/Fs.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <new>
#include <set>

namespace duskstudio::sessionbackups
{
namespace
{
namespace stdfs = std::filesystem;
using recoverystamp::kStampLength;
using recoverystamp::utcStamp;

constexpr int  kMaxCopies = 999;
constexpr char kFolder[]  = "backups";
constexpr std::string_view kBase { "_session" };
constexpr std::string_view kExtension { ".json" };

std::string nameFor (const std::string& stamp, int copy)
{
    auto name = stamp + std::string (kBase);
    if (copy > 1)
    {
        char suffix[16] {};
        std::snprintf (suffix, sizeof (suffix), "_%03d", copy);
        name += suffix;
    }
    return name + std::string (kExtension);
}

bool readAll (const stdfs::path& file, std::string& content, std::error_code& error)
{
    error.clear();
    std::ifstream in (dusk::fs::nativeIOPath (file), std::ios::binary);
    if (! in)
    {
        error = std::make_error_code (std::errc::permission_denied);
        return false;
    }
    try
    {
        content.assign (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
    }
    catch (const std::ios_base::failure&)
    {
        error = std::make_error_code (std::errc::io_error);
        return false;
    }
    if (in.bad())
    {
        error = std::make_error_code (std::errc::io_error);
        return false;
    }
    return true;
}

bool holds (const stdfs::path& file, std::string_view bytes)
{
    if (dusk::fs::fileSize (file) != (std::int64_t) bytes.size()) return false;
    std::string content;
    std::error_code error;
    return readAll (file, content, error) && content == bytes;
}

// The backups there, oldest first. Only regular files with backup names count.
std::vector<std::string> backupsIn (const stdfs::path& folder, Result* result)
{
    std::vector<std::string> names;
    std::error_code error;
    stdfs::directory_iterator it (dusk::fs::nativeIOPath (folder), error);
    if (error == std::errc::no_such_file_or_directory) return names;
    for (; ! error && it != stdfs::directory_iterator(); it.increment (error))
    {
        auto name = it->path().filename().u8string();
        std::error_code typeError;
        if (isBackupName (name) && it->symlink_status (typeError).type() == stdfs::file_type::regular)
            names.push_back (std::move (name));
    }
    if (error && result != nullptr) result->failures.push_back ({ folder, error });
    std::sort (names.begin(), names.end());
    return names;
}

bool makeFolder (const stdfs::path& sessionDir, const stdfs::path& folder, Result& result)
{
    std::error_code error;
    if (! stdfs::is_directory (dusk::fs::nativeIOPath (sessionDir), error))
    {
        result.failures.push_back ({ sessionDir, error ? error : std::make_error_code (std::errc::not_a_directory) });
        return false;
    }
    // Only the one level: a session folder removed meanwhile must not come
    // back as the parent of its backups.
    stdfs::create_directory (dusk::fs::nativeIOPath (folder), error);
    if (! error) return true;
    result.failures.push_back ({ folder, error });
    return false;
}

void retain (const stdfs::path& folder, Result& result)
{
    for (const auto& name : namesToRemove (backupsIn (folder, &result), std::chrono::system_clock::now()))
    {
        const auto file = folder / stdfs::u8path (name);
        std::error_code error;
        if (stdfs::remove (dusk::fs::nativeIOPath (file), error))
            result.removed.push_back (file);
        else if (error)
            result.failures.push_back ({ file, error });
    }
}

// Writes bytes under the first name of that second nothing in backups/ holds
// yet, so no file there is ever replaced, then applies retention.
void writeNew (Result& result, const stdfs::path& sessionDir, std::chrono::system_clock::time_point savedAt,
               std::string_view bytes)
{
    const auto folder = sessionDir / kFolder;
    if (! makeFolder (sessionDir, folder, result)) return;

    const auto stamp = utcStamp (savedAt);
    for (int copy = 1; copy <= kMaxCopies; ++copy)
    {
        const auto target = folder / stdfs::u8path (nameFor (stamp, copy));
        std::error_code error;
        const auto status = stdfs::symlink_status (dusk::fs::nativeIOPath (target), error);
        if (status.type() != stdfs::file_type::not_found)
        {
            if (! error) continue;
            result.failures.push_back ({ target, error });
            return;
        }
        if (! dusk::fs::writeFileAtomically (target, bytes.data(), bytes.size(), error))
        {
            result.failures.push_back ({ target, error });
            return;
        }
        result.written.push_back (target);
        retain (folder, result);
        return;
    }
    result.failures.push_back ({ folder, std::make_error_code (std::errc::file_exists) });
}

template <typename Work>
Result guarded (const stdfs::path& sessionDir, Work&& work) noexcept
{
    Result result;
    try
    {
        work (result);
    }
    catch (const stdfs::filesystem_error& failure)
    {
        result.failures.push_back ({ failure.path1().empty() ? sessionDir : failure.path1(), failure.code() });
    }
    catch (const std::bad_alloc&)
    {
        result.failures.push_back ({ sessionDir, std::make_error_code (std::errc::not_enough_memory) });
    }
    catch (...)
    {
        result.failures.push_back ({ sessionDir, std::make_error_code (std::errc::io_error) });
    }
    return result;
}

bool isBackupFolderOf (const stdfs::path& folder, const stdfs::path& sessionDir)
{
    const auto expected = sessionDir / kFolder;
    if (folder.lexically_normal() == expected.lexically_normal()) return true;
    std::error_code error;
    return stdfs::equivalent (dusk::fs::nativeIOPath (folder), dusk::fs::nativeIOPath (expected), error);
}
} // namespace

std::string fileName (std::chrono::system_clock::time_point savedAt, int copy)
{
    return nameFor (utcStamp (savedAt), copy);
}

bool isBackupName (std::string_view name) noexcept
{
    if (! recoverystamp::isStamp (name)) return false;
    auto rest = name.substr (kStampLength);
    if (rest.size() < kBase.size() + kExtension.size() || rest.substr (0, kBase.size()) != kBase
        || rest.substr (rest.size() - kExtension.size()) != kExtension)
        return false;
    rest = rest.substr (kBase.size(), rest.size() - kBase.size() - kExtension.size());
    if (rest.empty()) return true;
    if (rest.size() != 4 || rest[0] != '_') return false;
    int copy = 0;
    for (const char c : rest.substr (1))
    {
        if (c < '0' || c > '9') return false;
        copy = copy * 10 + (c - '0');
    }
    return copy >= 2;
}

Result keepCurrent (const std::filesystem::path& sessionDir, std::chrono::system_clock::time_point keptAt) noexcept
{
    return guarded (sessionDir, [&] (Result& result)
    {
        const auto current = sessionDir / "session.json";
        std::error_code error;
        const auto status = stdfs::status (dusk::fs::nativeIOPath (current), error);
        if (status.type() == stdfs::file_type::not_found) return;
        if (error || status.type() != stdfs::file_type::regular)
        {
            if (error) result.failures.push_back ({ current, error });
            return;
        }

        std::string content;
        if (! readAll (current, content, error))
        {
            result.failures.push_back ({ current, error });
            return;
        }
        // Named by the save, not by the file's own time: a copy that sorted
        // among old backups could fall to retention before the save lands.
        const auto folder = sessionDir / kFolder;
        const auto names = backupsIn (folder, &result);
        if (! names.empty() && holds (folder / stdfs::u8path (names.back()), content)) return;
        writeNew (result, sessionDir, keptAt, content);
    });
}

Result keepSaved (const std::filesystem::path& sessionDir, std::chrono::system_clock::time_point savedAt,
                  std::string_view bytes) noexcept
{
    return guarded (sessionDir, [&] (Result& result)
    {
        const auto folder = sessionDir / kFolder;
        const auto names = backupsIn (folder, &result);
        if (! names.empty() && holds (folder / stdfs::u8path (names.back()), bytes)) return;
        writeNew (result, sessionDir, savedAt, bytes);
    });
}

std::vector<std::string> namesToRemove (std::vector<std::string> names, std::chrono::system_clock::time_point now)
{
    names.erase (std::remove_if (names.begin(), names.end(), [] (const std::string& name) { return ! isBackupName (name); }),
                 names.end());
    std::sort (names.begin(), names.end());
    names.erase (std::unique (names.begin(), names.end()), names.end());

    std::vector<bool> keep (names.size(), false);
    const auto newestCount = std::min (names.size(), (std::size_t) kKeepNewest);
    std::fill (keep.end() - (std::ptrdiff_t) newestCount, keep.end(), true);

    // Dates compare as text: "YYYYMMDD" sorts as the dates do.
    const auto firstDate = utcStamp (now - std::chrono::hours (24 * (kKeepDays - 1))).substr (0, 8);
    std::set<std::string> dated;
    for (std::size_t i = names.size(); i-- > 0;)
    {
        auto date = names[i].substr (0, 8);
        if (date >= firstDate && dated.insert (std::move (date)).second) keep[i] = true;
    }

    std::vector<std::string> removed;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (! keep[i]) removed.push_back (names[i]);
    return removed;
}

std::optional<std::filesystem::path> newest (const std::filesystem::path& sessionDir,
                                             const std::filesystem::path& before)
{
    try
    {
        const auto folder = sessionDir / kFolder;
        auto names = backupsIn (folder, nullptr);
        const auto beforeName = before.filename().u8string();
        if (! before.empty() && isBackupName (beforeName) && isBackupFolderOf (before.parent_path(), sessionDir))
            names.erase (std::lower_bound (names.begin(), names.end(), beforeName), names.end());
        if (names.empty()) return std::nullopt;
        return folder / stdfs::u8path (names.back());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> sessionFolderOf (const std::filesystem::path& file)
{
    const auto folder = file.parent_path();
    if (! isBackupName (file.filename().u8string()) || folder.filename() != kFolder) return std::nullopt;
    return folder.parent_path();
}

std::optional<std::chrono::system_clock::time_point> savedAtOf (const std::filesystem::path& backup)
{
    const auto name = backup.filename().u8string();
    if (! isBackupName (name)) return std::nullopt;
    return recoverystamp::timeOfStamp (name);
}

bool sameBytes (const std::filesystem::path& a, const std::filesystem::path& b) noexcept
{
    try
    {
        std::string content;
        std::error_code error;
        return readAll (a, content, error) && holds (b, content);
    }
    catch (...)
    {
        return false;
    }
}
} // namespace duskstudio::sessionbackups
