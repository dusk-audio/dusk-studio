#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
 #include <cerrno>
 #include <fcntl.h>
 #include <sys/stat.h>
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

struct SharedStoreEntry
{
    std::filesystem::file_type type = std::filesystem::file_type::none;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type writeTime {};
    std::filesystem::path symlinkTarget;

    bool operator== (const SharedStoreEntry& other) const noexcept
    {
        return type == other.type && size == other.size
            && writeTime == other.writeTime && symlinkTarget == other.symlinkTarget;
    }
};

struct SharedStoreSnapshot
{
    bool rootExisted = false;
    std::filesystem::file_time_type takenAt {};
    std::map<std::filesystem::path, SharedStoreEntry> entries;
};

// Coarsest timestamp step a session directory can land on: ext4 and XFS stamp
// nanoseconds, but ext3, HFS+ and most NFS servers stamp whole seconds and
// FAT/exFAT stamps two-second steps. A file rewritten in place at an identical
// size inside one step keeps an identical (size, mtime) pair, so anything
// stamped this close to the snapshot has to be treated as changed.
inline constexpr std::chrono::seconds kSharedStoreMtimeGranularity { 2 };

struct GenerationTransaction
{
    std::filesystem::path stateDir;
    std::filesystem::path next;
    SharedStoreSnapshot copyBefore;
    SharedStoreSnapshot linkBefore;
};

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

// Lilv treats copy/ and link/ as immutable shared stores: a changed scratch
// file gets a new revision name, and a new external path gets a new link. Keep
// the identities present before a save so commit can make only additions
// durable instead of opening and fsyncing every older sample on every autosave.
inline SharedStoreSnapshot snapshotSharedStore (
    const std::filesystem::path& root)
{
    SharedStoreSnapshot snapshot;
    snapshot.takenAt = std::filesystem::file_time_type::clock::now();
    std::error_code ec;
    const auto rootStatus = std::filesystem::symlink_status (root, ec);
    if (ec || rootStatus.type() == std::filesystem::file_type::not_found)
        return snapshot;

    snapshot.rootExisted = true;
    if (! std::filesystem::is_directory (rootStatus)) return snapshot;

    std::filesystem::recursive_directory_iterator it (
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (! ec && it != end)
    {
        const auto path = it->path();
        const auto status = it->symlink_status (ec);
        if (ec) break;

        SharedStoreEntry entry;
        entry.type = status.type();
        if (std::filesystem::is_regular_file (status))
        {
            entry.size = it->file_size (ec);
            if (ec) break;
            entry.writeTime = it->last_write_time (ec);
            if (ec) break;
        }
        else if (std::filesystem::is_symlink (status))
        {
            entry.symlinkTarget = std::filesystem::read_symlink (path, ec);
            if (ec) break;
        }
        snapshot.entries.emplace (path.lexically_relative (root), std::move (entry));
        it.increment (ec);
    }
    return snapshot;
}

struct SharedStoreSyncPlan
{
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;

    bool empty() const noexcept { return files.empty() && directories.empty(); }
};

inline SharedStoreSyncPlan planSharedStoreSync (
    const std::filesystem::path& root, const SharedStoreSnapshot& before)
{
    SharedStoreSyncPlan plan;
    std::set<std::filesystem::path> changedDirectories;
    std::error_code ec;
    const auto rootStatus = std::filesystem::symlink_status (root, ec);
    if (ec || rootStatus.type() == std::filesystem::file_type::not_found)
    {
        if (before.rootExisted) plan.directories.push_back (root.parent_path());
        return plan;
    }

    if (! before.rootExisted)
    {
        changedDirectories.insert (root);
        changedDirectories.insert (root.parent_path());
    }
    if (! std::filesystem::is_directory (rootStatus))
    {
        plan.directories.assign (changedDirectories.begin(), changedDirectories.end());
        return plan;
    }

    const auto rememberParents = [&] (const std::filesystem::path& path)
    {
        auto parent = path.parent_path();
        while (! parent.empty())
        {
            changedDirectories.insert (parent);
            if (parent == root) break;
            parent = parent.parent_path();
        }
    };

    std::set<std::filesystem::path> seen;
    std::filesystem::recursive_directory_iterator it (
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (! ec && it != end)
    {
        const auto path = it->path();
        const auto relative = path.lexically_relative (root);
        seen.insert (relative);
        const auto status = it->symlink_status (ec);
        if (ec) break;

        SharedStoreEntry current;
        current.type = status.type();
        if (std::filesystem::is_regular_file (status))
        {
            current.size = it->file_size (ec);
            if (ec) break;
            current.writeTime = it->last_write_time (ec);
            if (ec) break;
        }
        else if (std::filesystem::is_symlink (status))
        {
            current.symlinkTarget = std::filesystem::read_symlink (path, ec);
            if (ec) break;
        }

        const auto previous = before.entries.find (relative);
        if (previous == before.entries.end() || ! (previous->second == current))
        {
            if (std::filesystem::is_regular_file (status))
                plan.files.push_back (path);
            else if (std::filesystem::is_directory (status))
                changedDirectories.insert (path);
            rememberParents (path);
        }
        else if (std::filesystem::is_regular_file (status)
                 && current.writeTime > before.takenAt - kSharedStoreMtimeGranularity)
        {
            // Same bytes as far as (size, mtime) can tell, but too recent for
            // that pair to distinguish a rewrite in place from the file it
            // replaced. Only the file itself is at risk - its directory entry
            // is unchanged - so the parents stay out of the plan.
            plan.files.push_back (path);
        }
        it.increment (ec);
    }

    // A removed entry changes its former parent directory even though there is
    // no surviving path to visit after the transaction.
    for (const auto& [relative, entry] : before.entries)
    {
        (void) entry;
        if (seen.find (relative) == seen.end()) rememberParents (root / relative);
    }

    plan.directories.assign (changedDirectories.begin(), changedDirectories.end());
    const auto depth = [] (const std::filesystem::path& path)
    {
        return std::distance (path.begin(), path.end());
    };
    std::sort (plan.directories.begin(), plan.directories.end(),
               [&] (const auto& a, const auto& b)
               {
                   const auto aDepth = depth (a);
                   const auto bDepth = depth (b);
                   return aDepth != bDepth ? aDepth > bDepth : a < b;
               });
    return plan;
}

inline void applySharedStoreSyncPlan (const SharedStoreSyncPlan& plan) noexcept
{
    for (const auto& file : plan.files) syncPathBestEffort (file);
    // Deepest first: persist each new directory's contents before its name in
    // the parent, then finally persist a newly-created shared-store root.
    for (const auto& directory : plan.directories) syncPathBestEffort (directory);
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

// Releases before generation-managed state still created cur/ for every LV2
// slot, including blob-only slots, but never wrote state.ttl or .ready there.
// Those two files therefore distinguish a managed cur/ (which must match the
// persisted blob) from the legacy restore root (which the blob parser may use).
// Treat an inspection error conservatively as managed so unreadable new state
// cannot silently fall through to a potentially wrong file-backed restore.
inline bool generationUsesManagedFormat (const std::filesystem::path& generation)
{
    const auto markerExists = [] (const std::filesystem::path& marker)
    {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status (marker, ec);
        // A dangling symlink or unreadable marker is still evidence of the
        // managed format and must take the strict failure path.
        if (ec == std::errc::no_such_file_or_directory) return false;
        return ec || status.type() != std::filesystem::file_type::not_found;
    };
    return markerExists (generation / kStateFileName)
        || markerExists (generation / kReadyMarkerName);
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
// This walks the whole store, so callers run it only when a rotation can
// actually have dropped the last reference to something.
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
    const bool curUsesManagedFormat = hasCur && generationUsesManagedFormat (cur);
    const bool prevUsesManagedFormat = hasPrev && generationUsesManagedFormat (prev);

    enum class Choice { None, Cur, Prev, Next, Older };
    Choice choice = Choice::None;
    // Counts the generations this reconcile deletes, which is the only way it
    // can leave a shared-store entry without a referring symlink.
    std::uintmax_t dropped = 0;
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
        if (choice == Choice::None
            && (curUsesManagedFormat || prevUsesManagedFormat
                || nextIsReady || hasOlder))
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
            dropped += std::filesystem::remove_all (older, ec);
            if (ec) return false;
        }
    }

    // Any staging generation not selected above was never committed by the
    // persisted session. It must not later overwrite the matching cur/.
    std::error_code ignored;
    if (choice != Choice::Next)
        dropped += std::filesystem::remove_all (next, ignored);
    dropped += std::filesystem::remove_all (rejected, ignored);
    std::filesystem::remove (cur / kReadyMarkerName, ignored);
    // Settling on the generation already in cur/ without deleting another one
    // removes no referrer, which is the case the per-save call hits. Anything
    // that discarded or promoted a generation gets the sweep.
    if (dropped > 0 || choice != Choice::Cur)
        pruneUnreferencedSharedFiles (stateDir);
    return true;
}

// Build a save in <dir>/next while <dir>/cur remains untouched. A restored
// file-backed plugin can keep absolute paths into cur while lilv copies those
// files into the fresh generation; rotating cur before asking the plugin for
// its state makes those paths stale during serialization.
inline GenerationTransaction prepareNextGeneration (
    const std::filesystem::path& stateDir, std::error_code& ec)
{
    GenerationTransaction transaction;
    ec.clear();
    if (stateDir.empty()) return {};
    if (! recoverGeneration (stateDir, {}, ec)) return {};
    transaction.stateDir = stateDir;
    transaction.copyBefore = snapshotSharedStore (stateDir / "copy");
    transaction.linkBefore = snapshotSharedStore (stateDir / "link");
    const auto next = stateDir / "next";

    std::filesystem::remove_all (next, ec);
    if (ec) return {};
    std::filesystem::create_directories (next, ec);
    if (ec) return {};
    transaction.next = next;
    return transaction;
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
inline bool commitNextGeneration (const GenerationTransaction& transaction,
                                  std::error_code& ec)
{
    ec.clear();
    const auto& stateDir = transaction.stateDir;
    if (stateDir.empty()) return false;

    const auto next = stateDir / "next";
    if (transaction.next != next)
    {
        ec = std::make_error_code (std::errc::invalid_argument);
        return false;
    }
    if (! std::filesystem::is_directory (next, ec) || ec)
        return false;

    // Mark the generation only after the caller has successfully serialized
    // it. Recovery can then distinguish publish interruption from an incomplete
    // staging write.
    syncTreeBestEffort (next);
    const auto copyPlan = planSharedStoreSync (stateDir / "copy", transaction.copyBefore);
    const auto linkPlan = planSharedStoreSync (stateDir / "link", transaction.linkBefore);
    applySharedStoreSyncPlan (copyPlan);
    applySharedStoreSyncPlan (linkPlan);
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

    if (! publishReadyNext (stateDir, ec)) return false;

    // Sweeping unconditionally is what made every save cost time proportional to
    // the store's file count. A save that added nothing to either store can at
    // worst defer an orphan to the next save that does add something, or to the
    // sweep on session load; an orphan costs disk space, not correctness.
    if (! copyPlan.empty() || ! linkPlan.empty())
        pruneUnreferencedSharedFiles (stateDir);
    return true;
}

// How far down the resolved path canonical containment may be enforced. A
// generation's entries are symlinks into <dir>/copy for files the plugin wrote
// and into <dir>/link for files it only referenced, and a link-store entry
// points straight at the user's original file outside the session - resolving
// the leaf would reject an ordinary external sample. Only real directories sit
// above it, so a symlink there is never something lilv wrote.
inline bool parentsResolveInsideStateDirectory (
    const std::filesystem::path& stateDir, const std::filesystem::path& resolved)
{
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical (stateDir, ec);
    const auto root = ec ? stateDir.lexically_normal() : canonicalRoot;
    ec.clear();
    const auto parents = std::filesystem::weakly_canonical (resolved.parent_path(), ec);
    return ! ec && isWithin (root, parents);
}

// Restore side: an abstract path from a state blob resolves against <dir>/cur.
// Already-absolute abstract paths (and an empty stateDir) pass through. A blob
// value that escapes the state directory - lexically through "../" segments, or
// through a symlinked directory planted in its prefix - yields an empty string.
// Handing the unresolved value back instead would leave the plugin resolving it
// against the process working directory, which is both outside the session and
// dependent on where the application was launched from.
inline std::string toAbsolute (const std::filesystem::path& stateDir,
                               const std::string& abstractPath)
{
    const auto abstract = std::filesystem::u8path (abstractPath);
    if (stateDir.empty() || abstract.is_absolute()) return abstractPath;
    if (abstract.empty() || abstract == std::filesystem::path (".")) return {};

    const auto cur      = (stateDir / "cur").lexically_normal();
    const auto resolved = (cur / abstract).lexically_normal();
    if (! isWithin (cur, resolved)) return {};
    if (! parentsResolveInsideStateDirectory (stateDir, resolved)) return {};
    return resolved.u8string();
}

// Create the leading directories of a cur/-relative request without ever
// traversing a symlink, and refuse a leaf that already exists as one. Lexical
// containment alone is not enough: a symlink left in the state tree by a
// crafted session (or by a plugin that wrote one itself) redirects an
// in-root-looking write anywhere on the filesystem. Refusing a symlinked leaf
// costs nothing legitimate - lilv's own leaf symlinks point into the shared
// copy/link stores, and writing through one would corrupt the file every other
// generation references.
inline bool prepareContainedPath (const std::filesystem::path& cur,
                                  const std::filesystem::path& relative,
                                  std::error_code& ec)
{
    std::filesystem::create_directories (cur, ec);
    if (ec) return false;

#if defined(__linux__) || defined(__APPLE__)
    // Each level is created and reopened relative to the previous descriptor
    // with O_NOFOLLOW, so a symlink swapped in between the check and the
    // creation fails the open rather than redirecting it. Opening cur the same
    // way is what rejects a state root that is itself a link.
    const auto fail = [&ec] { ec = std::error_code (errno, std::generic_category()); };

    int dir = ::open (cur.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) { fail(); return false; }

    bool ok = true;
    for (auto part = relative.begin(); part != relative.end(); ++part)
    {
        if (std::next (part) == relative.end())
        {
            struct stat leaf {};
            if (::fstatat (dir, part->c_str(), &leaf, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK (leaf.st_mode))
            {
                ec = std::make_error_code (std::errc::too_many_symbolic_link_levels);
                ok = false;
            }
            break;
        }

        if (::mkdirat (dir, part->c_str(), 0777) != 0 && errno != EEXIST)
        {
            fail();
            ok = false;
            break;
        }
        const int child = ::openat (dir, part->c_str(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) { fail(); ok = false; break; }
        ::close (dir);
        dir = child;
    }

    ::close (dir);
    return ok;
#else
    // No openat equivalent here, so the walk is check-then-create and only as
    // race-resistant as the platform makes it. Testing the type rather than
    // is_symlink also catches a Windows junction, which reports its own
    // file_type and would slip past a symlink-only test.
    using FileType = std::filesystem::file_type;

    // create_directories leaves an existing link alone, so cur has to be shown
    // to be a real directory before anything is built underneath it.
    if (std::filesystem::symlink_status (cur, ec).type() != FileType::directory)
    {
        ec = std::make_error_code (std::errc::too_many_symbolic_link_levels);
        return false;
    }

    auto walk = cur;
    for (auto part = relative.begin(); part != relative.end(); ++part)
    {
        walk /= *part;
        const auto type = std::filesystem::symlink_status (walk, ec).type();
        if (ec && ec != std::errc::no_such_file_or_directory) return false;
        ec.clear();

        const bool isLeaf = std::next (part) == relative.end();
        // A plugin may reuse a directory or file it made earlier; anything else
        // that already occupies the name is a reparse point or worse.
        if (type != FileType::not_found && type != FileType::directory
            && ! (isLeaf && type == FileType::regular))
        {
            ec = std::make_error_code (std::errc::too_many_symbolic_link_levels);
            return false;
        }
        if (isLeaf) break;

        std::filesystem::create_directory (walk, ec);
        if (ec && ! std::filesystem::is_directory (walk)) return false;
        ec.clear();
    }
    return true;
#endif
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

    if (! prepareContainedPath (cur, resolved.lexically_relative (cur), ec))
        return {};
    return resolved;
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
