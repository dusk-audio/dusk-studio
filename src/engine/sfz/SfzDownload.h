#pragma once

#include "SfzTransport.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace duskstudio::sfz
{
struct DownloadRequest
{
    std::string url;
    // Partial transfers accumulate here and survive cancellation so the next
    // attempt resumes instead of restarting.
    std::filesystem::path partFile;
    // The verified archive is renamed here; it must sit on the same filesystem
    // as partFile so the publish step is a rename, never a copy.
    std::filesystem::path destination;
    std::string expectedSha256;
    std::uint64_t expectedBytes { 0 };
    // The first attempt plus the bounded resumes that follow a short transfer.
    unsigned maximumAttempts { 4 };
    TransferLimits limits;
};

enum class DownloadStatus
{
    completed,
    cancelled,
    transferFailed,
    sizeMismatch,
    hashMismatch,
    storageFailed
};

struct DownloadResult
{
    DownloadStatus status { DownloadStatus::transferFailed };
    std::uint64_t downloadedBytes { 0 };
    std::string error;

    explicit operator bool() const noexcept
    {
        return status == DownloadStatus::completed;
    }
};

struct DownloadCallbacks
{
    std::function<void (std::uint64_t receivedBytes, std::uint64_t totalBytes)> onProgress;
    // Polled throughout the transfer. Returning true unwinds the attempt and
    // leaves the part file behind for a later resume.
    std::function<bool()> isCancelled;
};

// Streams the archive to the part file, then verifies its SHA-256 before the
// bytes are given a name any other code will open. A hash mismatch deletes the
// part file so a poisoned prefix can never be resumed into a valid archive.
DownloadResult downloadArchive (Transport& transport,
                                const DownloadRequest& request,
                                const DownloadCallbacks& callbacks) noexcept;

// Streaming SHA-256 of a file, lowercase hex. Empty when the file cannot be
// read, and empty when isCancelled (polled per block) asks it to stop, so an
// app quit does not block a join on hashing a multi-gigabyte archive.
std::string hashFileSha256 (const std::filesystem::path& file,
                            const std::function<bool()>& isCancelled = {}) noexcept;
} // namespace duskstudio::sfz
