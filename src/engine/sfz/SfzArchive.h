#pragma once

#include "SfzArchivePolicy.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace duskstudio::sfz
{
struct ExtractionRequest
{
    std::filesystem::path archiveFile;
    // Must exist and be empty. The extractor creates every directory below it
    // itself, which is what makes escaping through a pre-existing link
    // impossible.
    std::filesystem::path destinationDirectory;
    ArchiveLimits limits;
};

enum class ExtractionStatus
{
    completed,
    cancelled,
    // The archive violated the policy. The destination holds a partial tree and
    // the caller must delete it.
    rejected,
    readFailed,
    storageFailed
};

struct ExtractionResult
{
    ExtractionStatus status { ExtractionStatus::readFailed };
    std::uint32_t fileCount { 0 };
    std::uint64_t expandedBytes { 0 };
    std::string error;

    explicit operator bool() const noexcept
    {
        return status == ExtractionStatus::completed;
    }
};

struct ExtractionCallbacks
{
    std::function<void (std::uint64_t expandedBytes)> onProgress;
    std::function<bool()> isCancelled;
};

// Expands a ZIP by creating regular files and directories itself: no archive
// library disk writer, no permissions, ownership, times or extended attributes
// carried over from the archive, and no entry kind other than file and
// directory ever materialised.
ExtractionResult extractZipArchive (const ExtractionRequest& request,
                                    const ExtractionCallbacks& callbacks) noexcept;
} // namespace duskstudio::sfz
