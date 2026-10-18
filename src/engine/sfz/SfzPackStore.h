#pragma once

#include "ManagedSfzLocator.h"
#include "SfzDirectoryLease.h"
#include "SfzProvider.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio::sfz
{
std::filesystem::path defaultPackStoreRoot();

struct InstalledPack
{
    ProviderPack provider;
    std::string archiveSha256;
    std::vector<ProviderInstrument> instruments;
    std::vector<ProviderInstrument> programs;
    std::filesystem::path directory;
    std::string installedTreeSha256;

    ManagedSfzLocator locatorFor (const ProviderInstrument& instrument) const
    {
        return { provider.sourceId, provider.packId, provider.releaseId, archiveSha256,
                 instrument.id, instrument.relativePath };
    }
};

struct PackPublishResult : OperationResult
{
    std::optional<InstalledPack> pack;
};

class PackStore
{
public:
    explicit PackStore (std::filesystem::path root, std::string marker = {})
        : storeRoot (std::move (root)), requiredMarker (std::move (marker)) {}
    bool initialize (std::string& error, std::error_code* nativeError = nullptr);
    const std::filesystem::path& root() const noexcept { return storeRoot; }
    std::filesystem::path createStaging (std::string& error, std::error_code* nativeError = nullptr);
    PackPublishResult publish (const ProviderPack&, const std::string& archiveSha256,
                               const std::filesystem::path& staging,
                               const std::vector<std::string>& extractedSfzPaths,
                               const OperationControl& = {});
    std::vector<InstalledPack> installed() const;
    std::optional<std::filesystem::path> resolve (const ManagedSfzLocator&) const;
    std::optional<ManagedSfzLocator> locatorForFile (const std::filesystem::path&) const;
    OperationResult remove (const InstalledPack&, const std::vector<ManagedSfzLocator>& inUse);
    OperationResult verify (const InstalledPack&, const OperationControl& = {}) const;
    static std::string identity (const ManagedSfzLocator&);

private:
    std::optional<InstalledPack> readReceipt (const std::filesystem::path&, bool requireEntrypoints = true,
                                            std::string* receiptBytes = nullptr) const;
    std::filesystem::path storeRoot;
    std::string requiredMarker;
    DirectoryLease rootLease;
};
} // namespace duskstudio::sfz
