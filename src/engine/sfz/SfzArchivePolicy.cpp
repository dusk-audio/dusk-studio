#include "SfzArchivePolicy.h"
#include "SfzPathRules.h"

#include <utility>

namespace duskstudio::sfz
{
ArchivePolicy::ArchivePolicy (ArchiveLimits limitsToUse)
    : limits (std::move (limitsToUse))
{
}

bool ArchivePolicy::reject (std::string reason)
{
    if (failure.empty())
        failure = std::move (reason);
    return false;
}

bool ArchivePolicy::accept (const ArchiveEntry& entry)
{
    if (! failure.empty())
        return false;
    if (limits.expectedRoot.empty() || limits.maximumEntries == 0
        || limits.maximumExpandedBytes == 0 || limits.maximumFileBytes == 0)
        return reject ("the archive limits are not configured");
    if (entry.kind == ArchiveEntryKind::unsupported)
        return reject ("contains an entry that is not a file or directory");

    std::string path = entry.pathname;
    // A trailing separator is how ZIP marks a directory; it is not part of the
    // name, and leaving it in would break every comparison below.
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();

    if (const auto* reason = paths::relativePathRejectionReason (path))
        return reject (reason);

    const auto rootEnd = path.find ('/');
    if (path.compare (0, rootEnd, limits.expectedRoot) != 0)
        return reject ("contains an entry outside the pack root");
    if (rootEnd == std::string::npos && entry.kind != ArchiveEntryKind::directory)
        return reject ("the pack root must be a directory");

    if (++entries > limits.maximumEntries)
        return reject ("contains more entries than the catalog allows");

    const auto lowered = paths::toLowerAscii (path);
    const auto claimed = claimedPaths.find (lowered);
    if (claimed != claimedPaths.end())
    {
        // Repeated directory entries are harmless because the extractor creates
        // each directory once; a repeated file name is an attempt to have the
        // second copy win, which is exactly what must not happen.
        if (claimed->second == ArchiveEntryKind::regularFile
            || entry.kind != ArchiveEntryKind::directory)
            return reject ("contains a duplicate or case-colliding entry name");
    }
    else
    {
        claimedPaths.emplace (lowered, entry.kind);
    }

    for (auto separator = lowered.find ('/'); separator != std::string::npos;
         separator = lowered.find ('/', separator + 1))
    {
        const auto ancestor = lowered.substr (0, separator);
        const auto existing = claimedPaths.find (ancestor);
        if (existing == claimedPaths.end())
            claimedPaths.emplace (ancestor, ArchiveEntryKind::directory);
        else if (existing->second == ArchiveEntryKind::regularFile)
            return reject ("contains a directory that collides with a file name");
    }

    if (entry.kind == ArchiveEntryKind::regularFile)
    {
        ++files;
        if (entry.sizeKnown)
        {
            if (entry.sizeBytes > limits.maximumFileBytes)
                return reject ("contains a file larger than the catalog allows");
            if (entry.sizeBytes > limits.maximumExpandedBytes - expanded)
                return reject ("expands to more bytes than the catalog allows");
        }
    }

    currentFileBytes = 0;
    lastAcceptedPath = std::move (path);
    return true;
}

bool ArchivePolicy::addFileBytes (std::uint64_t bytes)
{
    if (! failure.empty())
        return false;
    if (bytes > limits.maximumFileBytes - currentFileBytes)
        return reject ("contains a file larger than the catalog allows");
    if (bytes > limits.maximumExpandedBytes - expanded)
        return reject ("expands to more bytes than the catalog allows");

    currentFileBytes += bytes;
    expanded += bytes;
    return true;
}

bool ArchivePolicy::finish()
{
    if (! failure.empty())
        return false;
    if (files == 0)
        return reject ("contains no files");
    return true;
}
} // namespace duskstudio::sfz
