#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace duskstudio::sfz
{
struct ArchiveLimits
{
    // Single path segment every entry must live under, taken from the catalog's
    // expected_root.
    std::string expectedRoot;
    std::uint64_t maximumExpandedBytes { 0 };
    std::uint64_t maximumFileBytes { 0 };
    // Ceiling on entries of every kind, so an archive of empty directories is
    // bounded the same way an archive of files is.
    std::uint32_t maximumEntries { 0 };
};

enum class ArchiveEntryKind
{
    directory,
    regularFile,
    // Links, devices, sockets and anything else the extractor refuses to
    // materialise. Mapped by the reader, rejected here.
    unsupported
};

struct ArchiveEntry
{
    std::string pathname;
    ArchiveEntryKind kind { ArchiveEntryKind::unsupported };
    std::uint64_t sizeBytes { 0 };
    bool sizeKnown { false };
};

// Decides which archive entries may become files on disk. Holds no file handles
// and performs no I/O, so the whole rejection matrix is testable without an
// archive library. The extractor must treat a false return as fatal for the
// archive - a rejected entry is evidence the whole pack is untrustworthy, not
// something to skip.
class ArchivePolicy
{
public:
    explicit ArchivePolicy (ArchiveLimits limits);

    bool accept (const ArchiveEntry& entry);

    // Charges bytes actually written for the current file against the per-file
    // and whole-archive budgets. Declared sizes are checked too, but only the
    // written count is authoritative: an archive header can understate what the
    // compressed stream expands to.
    bool addFileBytes (std::uint64_t bytes);

    bool finish();

    const std::string& error() const noexcept { return failure; }
    // Accepted path of the most recent entry, forward-slash relative to the
    // extraction directory and free of any trailing separator.
    const std::string& acceptedPath() const noexcept { return lastAcceptedPath; }
    std::uint32_t fileCount() const noexcept { return files; }
    std::uint64_t expandedBytes() const noexcept { return expanded; }

private:
    bool reject (std::string reason);

    ArchiveLimits limits;
    std::unordered_map<std::string, ArchiveEntryKind> claimedPaths;
    std::string failure;
    std::string lastAcceptedPath;
    std::uint64_t expanded { 0 };
    std::uint64_t currentFileBytes { 0 };
    std::uint32_t entries { 0 };
    std::uint32_t files { 0 };
};
} // namespace duskstudio::sfz
