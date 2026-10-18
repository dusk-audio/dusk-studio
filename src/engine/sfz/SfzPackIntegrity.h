#pragma once

#include "SfzArchive.h"

namespace duskstudio::sfz
{
struct PackContentsResult : OperationResult
{
    std::string sha256;
};

// The root installation receipt is excluded to avoid hashing its own digest.
PackContentsResult hashPackContents (const std::filesystem::path&, const OperationControl& = {},
                                    const ArchiveLimits& = {});
} // namespace duskstudio::sfz
