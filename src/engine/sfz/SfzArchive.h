#pragma once

#include "SfzOperation.h"
#include "SfzPathPolicy.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace duskstudio::sfz
{
struct ArchiveLimits
{
    std::uint64_t maxArchiveBytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t maxExpandedBytes = 32ULL * 1024 * 1024 * 1024;
    std::uint64_t maxFileBytes = 32ULL * 1024 * 1024 * 1024;
    std::uint32_t maxFiles = 100000; // Includes implicit parent directories.
    PathLimits paths;
    std::uint64_t maxCompressionRatio = 1000;
    std::uint64_t freeSpaceReserveBytes = 64ULL * 1024 * 1024;
};

struct ArchiveRequest
{
    std::filesystem::path archive;
    std::filesystem::path stagingDirectory;
    std::string expectedRoot;
    ArchiveLimits limits;
    std::string licensePath;
    std::string licenseSha256;
    std::vector<std::string> expectedEntrypoints;
};

struct ArchiveResult : OperationResult
{
    std::vector<std::string> sfzPaths;
    std::uint64_t files = 0;
    std::uint64_t expandedBytes = 0;
};

// Staging must be owned by the caller and empty. Failed or interrupted trees
// stay unpublished; their removal and publication belong to the caller.
ArchiveResult extractZip (const ArchiveRequest& request,
                          const OperationControl& control = {});
} // namespace duskstudio::sfz
