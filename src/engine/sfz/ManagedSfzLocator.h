#pragma once

#include "SfzPathPolicy.h"
#include "../../foundation/Json.h"

#include <optional>
#include <string>

namespace duskstudio::sfz
{
struct ManagedSfzLocator
{
    std::string sourceId, packId, releaseId, archiveSha256, instrumentId, relativePath;

    bool operator== (const ManagedSfzLocator& other) const noexcept
    {
        return sourceId == other.sourceId && packId == other.packId
            && releaseId == other.releaseId && archiveSha256 == other.archiveSha256
            && instrumentId == other.instrumentId && relativePath == other.relativePath;
    }
};

inline dusk::json::Json locatorToJson (const ManagedSfzLocator& locator)
{
    return { { "source", locator.sourceId }, { "pack", locator.packId },
             { "release", locator.releaseId }, { "archive_sha256", locator.archiveSha256 },
             { "instrument", locator.instrumentId }, { "entrypoint", locator.relativePath } };
}

inline std::optional<ManagedSfzLocator> locatorFromJson (const dusk::json::Json& json)
{
    ManagedSfzLocator locator { dusk::json::getString (json, "source"),
        dusk::json::getString (json, "pack"), dusk::json::getString (json, "release"),
        dusk::json::getString (json, "archive_sha256"), dusk::json::getString (json, "instrument"),
        dusk::json::getString (json, "entrypoint") };
    std::string error;
    if (locator.sourceId.empty() || locator.sourceId.size() > 128
        || locator.packId.empty() || locator.packId.size() > 256
        || locator.releaseId.empty() || locator.releaseId.size() > 256
        || locator.instrumentId.empty() || locator.instrumentId.size() > 512
        || ! isSha256 (locator.archiveSha256)
        || ! validateRelativePath (locator.relativePath, error)) return {};
    return locator;
}
} // namespace duskstudio::sfz
