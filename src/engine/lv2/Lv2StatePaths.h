#pragma once

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
 #include <fcntl.h>
 #include <unistd.h>
#endif

// LV2 state:mapPath translation and crash-safe generation management. Split
// out so it can be unit-tested without a lilv world or a real
// file-writing plugin (the live path is otherwise only exercised by a gated
// integration test). stateDir empty => paths pass through unchanged, matching
// the blob-only (in-memory) save behaviour.
namespace duskstudio::lv2::statepaths
{
inline constexpr const char* kReadyMarkerName = ".ready";
inline constexpr const char* kStateFileName = "state.ttl";

// Resolve symlinked existing prefixes while preserving a state-directory
// suffix that has not been created yet. On macOS this also normalizes the
// /var -> /private/var alias before lilv hands mapPath a canonical path.
inline std::filesystem::path normalizeStateDirectory (
    const std::filesystem::path& stateDir)
{
    if (stateDir.empty()) return {};
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical (stateDir, ec);
    return ec ? stateDir.lexically_normal() : canonical;
}

// Match SessionSerializer's durability policy without coupling this pure
// filesystem helper to JUCE. The LV2 host currently ships on Linux and macOS;
// other platforms keep the same process-crash-safe rename protocol but omit
// the platform-specific durability sync here.
inline void syncPathBestEffort (const std::filesystem::path& path) noexcept
{
#if defined(__linux__) || defined(__APPLE__)
    int flags = O_RDONLY;
   #ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
   #endif
    const int fd = ::open (path.c_str(), flags);
    if (fd < 0) return;
    (void) ::fsync (fd);
    (void) ::close (fd);
#else
    (void) path;
#endif
}

inline void syncTreeBestEffort (const std::filesystem::path& root) noexcept
{
    std::error_code ec;
    if (! std::filesystem::exists (root, ec) || ec) return;
    std::filesystem::recursive_directory_iterator it (
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (! ec && it != end)
    {
        const auto status = it->symlink_status (ec);
        if (ec) break;
        if (std::filesystem::is_regular_file (status)
            || std::filesystem::is_directory (status))
            syncPathBestEffort (it->path());
        it.increment (ec);
    }
    syncPathBestEffort (root);
}

inline bool generationIsReady (const std::filesystem::path& generation)
{
    std::ifstream marker (generation / kReadyMarkerName, std::ios::binary);
    std::string markerContents;
    std::getline (marker, markerContents);
    return markerContents == "ready"
        && marker.peek() == std::char_traits<char>::eof();
}

inline bool generationMatches (const std::filesystem::path& generation,
                               std::string_view serializedState)
{
    if (serializedState.empty()) return false;
    const auto stateFile = generation / kStateFileName;
    std::error_code ec;
    const auto size = std::filesystem::file_size (stateFile, ec);
    if (ec || size != serializedState.size()) return false;

    std::ifstream input (stateFile, std::ios::binary);
    if (! input) return false;
    std::array<char, 4096> buffer {};
    size_t offset = 0;
    while (offset < serializedState.size())
    {
        const auto remaining = serializedState.size() - offset;
        const auto chunk = remaining < buffer.size() ? remaining : buffer.size();
        input.read (buffer.data(), static_cast<std::streamsize> (chunk));
        if (input.gcount() != static_cast<std::streamsize> (chunk)
            || std::memcmp (buffer.data(), serializedState.data() + offset, chunk) != 0)
            return false;
        offset += chunk;
    }
    return ! input.bad();
}

inline bool isWithin (const std::filesystem::path& root,
                      const std::filesystem::path& candidate)
{
    const auto relative = candidate.lexically_relative (root);
    return ! relative.empty() && relative != std::filesystem::path (".")
        && *relative.begin() != std::filesystem::path ("..");
}

// Lilv uses symlinks from each saved generation into stable copy/link stores so
// unchanged banks are not duplicated. Retain only entries referenced by cur or
// prev; older revisions otherwise accumulate for the lifetime of the session.
inline void pruneUnreferencedSharedFiles (const std::filesystem::path& stateDir) noexcept
{
    std::error_code ec;
    const auto absoluteState = std::filesystem::absolute (stateDir, ec).lexically_normal();
    if (ec) return;
    const std::array<std::filesystem::path, 2> sharedRoots {
        absoluteState / "copy", absoluteState / "link" };
    std::set<std::filesystem::path> referenced;

    for (const char* generationName : { "cur", "prev" })
    {
        const auto generation = absoluteState / generationName;
        std::filesystem::recursive_directory_iterator it (
            generation, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        if (ec) { ec.clear(); continue; }
        while (! ec && it != end)
        {
            if (std::filesystem::is_symlink (it->symlink_status (ec)) && ! ec)
            {
                const auto target = std::filesystem::read_symlink (it->path(), ec);
                if (! ec)
                {
                    const auto resolved = (target.is_absolute()
                        ? target : it->path().parent_path() / target).lexically_normal();
                    for (const auto& root : sharedRoots)
                        if (isWithin (root, resolved)) referenced.insert (resolved);
                }
            }
            if (ec) break;
            it.increment (ec);
        }
        ec.clear();
    }

    for (const auto& root : sharedRoots)
    {
        std::vector<std::filesystem::path> directories;
        std::filesystem::recursive_directory_iterator it (
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        if (ec) { ec.clear(); continue; }
        while (! ec && it != end)
        {
            const auto path = std::filesystem::absolute (it->path(), ec).lexically_normal();
            if (ec) break;
            const auto status = it->symlink_status (ec);
            if (ec) break;
            if (std::filesystem::is_directory (status))
                directories.push_back (path);
            else if (referenced.find (path) == referenced.end())
            {
                // A locked or permission-denied orphan must not prevent the
                // rest of the shared store from being swept.
                std::error_code removeError;
                std::filesystem::remove (path, removeError);
            }
            it.increment (ec);
        }
        ec.clear();
        for (auto dir = directories.rbegin(); dir != directories.rend(); ++dir)
        {
            std::filesystem::remove (*dir, ec); // removes empty directories only
            ec.clear();
        }
    }
}

// Publish an already-marked next/ without deleting prev/ first. prev.old is
// retained until next/ has become cur/, so every intermediate state has at
// least one recoverable generation.
inline bool publishReadyNext (const std::filesystem::path& stateDir,
                              std::error_code& ec)
{
    const auto cur = stateDir / "cur";
    const auto prev = stateDir / "prev";
    const auto older = stateDir / "prev.old";
    const auto next = stateDir / "next";

    if (! std::filesystem::is_directory (next, ec) || ec
        || ! generationIsReady (next))
    {
        if (! ec) ec = std::make_error_code (std::errc::state_not_recoverable);
        return false;
    }

    const bool hadCur = std::filesystem::exists (cur, ec);
    if (ec) return false;
    bool hadPrev = std::filesystem::exists (prev, ec);
    if (ec) return false;
    const bool hadOlder = std::filesystem::exists (older, ec);
    if (ec) return false;

    // Finish housekeeping from an earlier interrupted rotation before starting
    // another. If prev/ exists, prev.old is strictly older and redundant.
    if (hadOlder)
    {
        if (! hadPrev)
        {
            std::filesystem::rename (older, prev, ec);
            if (ec) return false;
            syncPathBestEffort (stateDir);
            hadPrev = true;
        }
        else
        {
            std::filesystem::remove_all (older, ec);
            if (ec) return false;
            syncPathBestEffort (stateDir);
        }
    }

    if (! hadCur)
    {
        std::filesystem::rename (next, cur, ec);
        if (ec) return false;
        syncPathBestEffort (stateDir);
        std::error_code ignored;
        std::filesystem::remove (cur / kReadyMarkerName, ignored);
        syncPathBestEffort (cur);
        syncPathBestEffort (stateDir);
        pruneUnreferencedSharedFiles (stateDir);
        return true;
    }

    if (hadPrev)
    {
        std::filesystem::rename (prev, older, ec);
        if (ec) return false;
        syncPathBestEffort (stateDir);
    }

    std::filesystem::rename (cur, prev, ec);
    if (ec)
    {
        const auto rotationError = ec;
        if (hadPrev)
        {
            std::error_code ignored;
            std::filesystem::rename (older, prev, ignored);
        }
        ec = rotationError;
        return false;
    }
    syncPathBestEffort (stateDir);

    std::filesystem::rename (next, cur, ec);
    if (ec)
    {
        const auto publishError = ec;
        std::error_code rollbackError;
        std::filesystem::rename (prev, cur, rollbackError);
        if (! rollbackError && hadPrev)
        {
            std::error_code ignored;
            std::filesystem::rename (older, prev, ignored);
        }
        ec = publishError;
        return false;
    }
    syncPathBestEffort (stateDir);

    std::error_code ignored;
    std::filesystem::remove (cur / kReadyMarkerName, ignored);
    std::filesystem::remove_all (older, ignored);
    syncPathBestEffort (cur);
    syncPathBestEffort (stateDir);
    pruneUnreferencedSharedFiles (stateDir);
    return true;
}

// Reconcile interrupted rotations before either restore or the next save. When
// a persisted state blob is available it is the source of truth: select the
// on-disk generation whose state.ttl matches those exact bytes.
inline bool recoverGeneration (const std::filesystem::path& stateDir,
                               std::string_view serializedState,
                               std::error_code& ec)
{
    ec.clear();
    if (stateDir.empty()) return true;
    std::filesystem::create_directories (stateDir, ec);
    if (ec) return false;

    const auto cur = stateDir / "cur";
    const auto prev = stateDir / "prev";
    const auto older = stateDir / "prev.old";
    const auto next = stateDir / "next";
    const auto rejected = stateDir / "rejected";

    const bool hasCur = std::filesystem::exists (cur, ec);
    if (ec) return false;
    const bool hasPrev = std::filesystem::exists (prev, ec);
    if (ec) return false;
    const bool hasOlder = std::filesystem::exists (older, ec);
    if (ec) return false;
    const bool hasNext = std::filesystem::exists (next, ec);
    if (ec) return false;
    const bool nextIsReady = hasNext && generationIsReady (next);

    enum class Choice { None, Cur, Prev, Next, Older };
    Choice choice = Choice::None;
    if (! serializedState.empty())
    {
        if (hasCur && generationMatches (cur, serializedState)) choice = Choice::Cur;
        else if (hasPrev && generationMatches (prev, serializedState)) choice = Choice::Prev;
        else if (nextIsReady && generationMatches (next, serializedState)) choice = Choice::Next;
        else if (hasOlder && generationMatches (older, serializedState)) choice = Choice::Older;

        // A pre-generation, blob-only session has no directories and remains
        // compatible with the string parser. Once generations exist, however,
        // resolving a non-matching blob against arbitrary cur/ files can hand a
        // restored plugin the wrong sample bank. Refuse that ambiguous restore.
        if (choice == Choice::None && (hasCur || hasPrev || nextIsReady || hasOlder))
        {
            ec = std::make_error_code (std::errc::state_not_recoverable);
            return false;
        }
    }
    if (choice == Choice::None)
    {
        if (hasCur) choice = Choice::Cur;
        else if (hasPrev) choice = Choice::Prev;
        else if (nextIsReady) choice = Choice::Next;
        else if (hasOlder) choice = Choice::Older;
    }

    if (choice == Choice::Prev)
    {
        std::filesystem::remove_all (rejected, ec);
        if (ec) return false;
        if (hasCur)
        {
            std::filesystem::rename (cur, rejected, ec);
            if (ec) return false;
        }
        std::filesystem::rename (prev, cur, ec);
        if (ec)
        {
            const auto recoveryError = ec;
            if (hasCur)
            {
                std::error_code ignored;
                std::filesystem::rename (rejected, cur, ignored);
            }
            ec = recoveryError;
            return false;
        }
        if (hasCur)
        {
            std::filesystem::rename (rejected, prev, ec);
            if (ec) return false;
        }
        else if (hasOlder)
        {
            std::filesystem::rename (older, prev, ec);
            if (ec) return false;
        }
    }
    else if (choice == Choice::Next)
    {
        if (! publishReadyNext (stateDir, ec)) return false;
    }
    else if (choice == Choice::Older)
    {
        if (hasCur || hasPrev)
        {
            ec = std::make_error_code (std::errc::state_not_recoverable);
            return false;
        }
        std::filesystem::rename (older, cur, ec);
        if (ec) return false;
    }
    else if (choice == Choice::Cur && hasOlder)
    {
        if (! hasPrev)
        {
            std::filesystem::rename (older, prev, ec);
            if (ec) return false;
        }
        else
        {
            std::filesystem::remove_all (older, ec);
            if (ec) return false;
        }
    }

    // Any staging generation not selected above was never committed by the
    // persisted session. It must not later overwrite the matching cur/.
    std::error_code ignored;
    if (choice != Choice::Next) std::filesystem::remove_all (next, ignored);
    std::filesystem::remove_all (rejected, ignored);
    std::filesystem::remove (cur / kReadyMarkerName, ignored);
    pruneUnreferencedSharedFiles (stateDir);
    return true;
}

// Build a save in <dir>/next while <dir>/cur remains untouched. A restored
// file-backed plugin can keep absolute paths into cur while lilv copies those
// files into the fresh generation; rotating cur before asking the plugin for
// its state makes those paths stale during serialization.
inline std::filesystem::path prepareNextGeneration (
    const std::filesystem::path& stateDir, std::error_code& ec)
{
    ec.clear();
    if (stateDir.empty()) return {};
    if (! recoverGeneration (stateDir, {}, ec)) return {};
    const auto next = stateDir / "next";

    std::filesystem::remove_all (next, ec);
    if (ec) return {};
    std::filesystem::create_directories (next, ec);
    return ec ? std::filesystem::path {} : next;
}

inline void discardNextGeneration (const std::filesystem::path& stateDir) noexcept
{
    if (stateDir.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all (stateDir / "next", ignored);
}

// Publish a successfully serialized next/ generation. cur remains the active
// generation until this commit point. If publishing next fails after cur was
// moved aside, restore cur before returning so the carried state stays usable.
inline bool commitNextGeneration (const std::filesystem::path& stateDir,
                                  std::error_code& ec)
{
    ec.clear();
    if (stateDir.empty()) return false;

    const auto next = stateDir / "next";
    if (! std::filesystem::is_directory (next, ec) || ec)
        return false;

    // Mark the generation only after the caller has successfully serialized
    // it. Recovery can then distinguish publish interruption from an incomplete
    // staging write.
    syncTreeBestEffort (next);
    syncTreeBestEffort (stateDir / "copy");
    syncTreeBestEffort (stateDir / "link");
    {
        std::ofstream marker (next / kReadyMarkerName,
                              std::ios::binary | std::ios::trunc);
        marker << "ready\n";
        marker.close();
        if (! marker)
        {
            ec = std::make_error_code (std::errc::io_error);
            return false;
        }
    }
    syncPathBestEffort (next / kReadyMarkerName);
    syncPathBestEffort (next);

    return publishReadyNext (stateDir, ec);
}

// Restore side: an abstract path from a state blob resolves against <dir>/cur.
// Already-absolute abstract paths (and an empty stateDir) pass through. A blob
// with "../" segments that would escape cur/ is refused (passed through
// unresolved) rather than mapped to a file outside the state directory.
inline std::string toAbsolute (const std::filesystem::path& stateDir,
                               const std::string& abstractPath)
{
    const auto abstract = std::filesystem::u8path (abstractPath);
    if (stateDir.empty() || abstract.empty()
        || abstract == std::filesystem::path (".") || abstract.is_absolute())
        return abstractPath;
    const auto cur      = stateDir / "cur";
    const auto resolved = (cur / abstract).lexically_normal();
    const auto rel      = resolved.lexically_relative (cur);
    if (rel.empty() || rel == std::filesystem::path (".")
        || *rel.begin() == std::filesystem::path (".."))
        return abstractPath;
    return resolved.u8string();
}

// Restore-side state:makePath. The plugin may create the returned file, so
// create its leading directories while keeping every request inside cur/.
inline std::filesystem::path makeRestorePath (
    const std::filesystem::path& stateDir, const std::string& requestedPath,
    std::error_code& ec)
{
    ec.clear();
    const auto requested = std::filesystem::u8path (requestedPath);
    if (stateDir.empty() || requested.empty()
        || requested == std::filesystem::path (".") || requested.is_absolute())
    {
        ec = std::make_error_code (std::errc::invalid_argument);
        return {};
    }

    const auto cur = (stateDir / "cur").lexically_normal();
    const auto resolved = (cur / requested).lexically_normal();
    if (! isWithin (cur, resolved))
    {
        ec = std::make_error_code (std::errc::invalid_argument);
        return {};
    }

    std::filesystem::create_directories (resolved.parent_path(), ec);
    return ec ? std::filesystem::path {} : resolved;
}

// Save side: an absolute path lilv hands us becomes a cur/-relative abstract
// path when the file lives under <dir>/cur; otherwise it passes through.
inline std::string toAbstract (const std::filesystem::path& stateDir,
                               const std::string& absolutePath)
{
    if (stateDir.empty()) return absolutePath;
    const auto cur = stateDir / "cur";
    const auto rel = std::filesystem::u8path (absolutePath).lexically_relative (cur);
    // lexically_relative yields "" for unrelated roots, "." for the dir itself,
    // and a "../"-leading path for siblings - none of which are children.
    if (! rel.empty() && rel != std::filesystem::path (".")
        && *rel.begin() != std::filesystem::path (".."))
        return rel.generic_string();
    return absolutePath;
}
} // namespace duskstudio::lv2::statepaths
