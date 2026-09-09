#include "SfzLibrary.h"

#include "../../foundation/Fs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <system_error>

namespace duskstudio::sfz
{
namespace
{
namespace stdfs = std::filesystem;

bool cancelled (const std::atomic<bool>* cancel) noexcept
{
    return cancel != nullptr && cancel->load (std::memory_order_relaxed);
}

std::string toLower (std::string_view text)
{
    std::string out (text);
    std::transform (out.begin(), out.end(), out.begin(),
                    [] (unsigned char c) { return (char) std::tolower (c); });
    return out;
}

bool classify (const stdfs::path& file, LibraryFormat& formatOut)
{
    if (dusk::fs::hasExtension (file, ".sfz")) { formatOut = LibraryFormat::Sfz; return true; }
    if (dusk::fs::hasExtension (file, ".sf2")) { formatOut = LibraryFormat::Sf2; return true; }
    return false;
}

// Identity for the visited set. A symlinked pack directory that points back up
// its own tree is a loop the depth cap alone would still walk to the bottom of,
// once per branch, so identity is what actually stops it.
std::string canonicalKey (const stdfs::path& dir)
{
    std::error_code ec;
    const auto canonical = stdfs::canonical (dir, ec);
    return ec ? dir.u8string() : canonical.u8string();
}

void scanOneRoot (const stdfs::path& root,
                  const std::atomic<bool>* cancel,
                  std::set<std::string>& visited,
                  ScanResult& result)
{
    std::error_code ec;
    if (! stdfs::exists (root, ec) || ec)
    {
        result.problems.push_back ({ root, "not found" });
        return;
    }
    if (! stdfs::is_directory (root, ec) || ec)
    {
        result.problems.push_back ({ root, "not a directory" });
        return;
    }

    struct Pending { stdfs::path dir; int depth; };
    std::vector<Pending> queue { { root, 0 } };
    bool reportedUnreadable = false;

    while (! queue.empty())
    {
        if (cancelled (cancel)) return;

        const auto pending = queue.back();
        queue.pop_back();

        if (! visited.insert (canonicalKey (pending.dir)).second)
            continue;

        std::error_code iterError;
        stdfs::directory_iterator it (pending.dir, iterError);
        if (iterError)
        {
            // One line per root, not per unreadable subdirectory: a permission
            // hole halfway down a pack tree is not worth a wall of rows.
            if (! reportedUnreadable)
            {
                result.problems.push_back ({ root, "could not be read: " + iterError.message() });
                reportedUnreadable = true;
            }
            continue;
        }

        for (const auto& entry : it)
        {
            if (cancelled (cancel)) return;

            std::error_code entryError;
            if (entry.is_directory (entryError) && ! entryError)
            {
                if (pending.depth + 1 <= kMaxScanDepth)
                    queue.push_back ({ entry.path(), pending.depth + 1 });
                continue;
            }
            if (entryError) continue;

            LibraryFormat format {};
            if (! classify (entry.path(), format)) continue;

            LibraryEntry found;
            found.path        = entry.path();
            found.displayName = entry.path().stem().u8string();
            found.folder      = entry.path().parent_path().filename().u8string();
            found.format      = format;
            found.sizeBytes   = dusk::fs::fileSize (entry.path());
            result.entries.push_back (std::move (found));
        }
    }
}
} // namespace

std::string_view formatName (LibraryFormat format) noexcept
{
    switch (format)
    {
        case LibraryFormat::Sfz: return "SFZ";
        case LibraryFormat::Sf2: return "SF2";
    }
    return "";
}

std::vector<std::filesystem::path> defaultLibraryRoots()
{
    const auto home = dusk::fs::userHomeDir();
    std::vector<stdfs::path> roots;

#if defined (_WIN32)
    roots.push_back (home / "Documents" / "Soundfonts");
    if (const char* localAppData = std::getenv ("LOCALAPPDATA");
        localAppData != nullptr && *localAppData != '\0')
        roots.push_back (stdfs::path (localAppData) / "Soundfonts");
#elif defined (__APPLE__)
    roots.push_back ("/Library/Audio/Sounds/Banks");
    roots.push_back (home / "Library" / "Audio" / "Sounds" / "Banks");
    roots.push_back (home / "Music" / "Soundfonts");
#else
    // The first two are where distribution packages install, so a fresh install
    // carrying a packaged General MIDI bank has something to show on first open.
    roots.push_back ("/usr/share/sounds/sf2");
    roots.push_back ("/usr/share/soundfonts");
    roots.push_back (home / ".local" / "share" / "sounds" / "sf2");
    roots.push_back (home / ".local" / "share" / "soundfonts");
    roots.push_back (home / ".local" / "share" / "sfz");
    roots.push_back (home / "soundfonts");
#endif

    return roots;
}

ScanResult scanLibraryRoots (const std::vector<std::filesystem::path>& roots,
                             const std::atomic<bool>* cancel,
                             std::function<void (std::size_t, std::size_t)> progress)
{
    ScanResult result;
    // Shared across roots so two roots that overlap, or one that symlinks into
    // another, cannot list the same instrument twice.
    std::set<std::string> visited;

    for (std::size_t i = 0; i < roots.size(); ++i)
    {
        if (cancelled (cancel))
        {
            result.cancelled = true;
            return result;
        }
        scanOneRoot (roots[i], cancel, visited, result);
        if (progress) progress (i + 1, roots.size());
    }

    if (cancelled (cancel)) result.cancelled = true;

    std::sort (result.entries.begin(), result.entries.end(),
               [] (const LibraryEntry& a, const LibraryEntry& b)
               {
                   const auto an = toLower (a.displayName);
                   const auto bn = toLower (b.displayName);
                   if (an != bn) return an < bn;
                   return a.path < b.path;
               });
    return result;
}

std::vector<const LibraryEntry*> filterEntries (const std::vector<LibraryEntry>& entries,
                                                std::string_view query)
{
    std::vector<const LibraryEntry*> matches;
    matches.reserve (entries.size());

    const auto needle = toLower (query);
    for (const auto& entry : entries)
    {
        if (needle.empty()
            || toLower (entry.displayName).find (needle) != std::string::npos
            || toLower (entry.folder).find (needle) != std::string::npos)
            matches.push_back (&entry);
    }
    return matches;
}
} // namespace duskstudio::sfz
