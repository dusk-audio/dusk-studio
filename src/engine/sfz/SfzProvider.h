#pragma once

#include "SfzTransfer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::sfz
{
struct ProviderSource
{
    std::string id, name, url;
};

struct ProviderDemo
{
    std::string label, url;
};

struct ProviderPresentation
{
    std::optional<std::uint64_t> stars {}, favorites {};
    std::vector<ProviderDemo> demos {};
    std::string artworkUrl, categoryHint;
};

struct ProviderSummary
{
    std::string sourceId, packId, locator, name, author, description, sourceUrl, licenseHint;
    ProviderPresentation presentation {};
};

struct ProviderRateLimit
{
    std::string remaining, reset, retryAfter;
};

struct ProviderPageResult : OperationResult
{
    std::vector<ProviderSummary> items;
    unsigned nextPage = 0;
    ProviderRateLimit rateLimit;
};

struct ProviderLicense
{
    std::string id, text, path, sha256, sourceUrl, constraints;
};

struct ProviderInstrument
{
    std::string id, name, relativePath;
    bool candidateOnly = true;
};

struct ProviderPack
{
    std::string sourceId, packId, releaseId, displayName, author, sourceUrl;
    std::string downloadUrl, expectedRoot, expectedSha256;
    std::uint64_t advertisedBytes = 0;
    bool versionPinned = false;
    ProviderLicense license;
    std::vector<ProviderInstrument> instruments;
    ProviderPresentation presentation {};
};

struct ProviderPackResult : OperationResult
{
    std::optional<ProviderPack> pack;
    ProviderRateLimit rateLimit;
};

inline constexpr std::size_t kMaxLicenseBytes = 200000;
inline constexpr std::size_t kMaxLicenseDocuments = 32;
inline constexpr std::size_t kMaxCombinedLicenseBytes = 2 * 1048576;
bool isPackLicenseDocument (std::string_view path);
OperationResult validatePackLicenseDocument (const ProviderLicense&, std::string_view path, std::string_view text);

std::vector<ProviderSource> providerSources();
std::vector<std::string> artworkHostsForSource (std::string_view sourceId);
bool isInstrumentArtworkPath (std::string_view path);
ProviderPageResult browseProvider (HttpTransport&, const std::string& sourceId,
                                   unsigned page = 1, const std::string& query = {},
                                   const OperationControl& = {});
ProviderPackResult resolveProviderPack (HttpTransport&, const ProviderSummary&,
                                       const OperationControl& = {});

std::string providerPackToJson (const ProviderPack&);
enum class PackDescriptorUse { Download, InstalledReceipt };
ProviderPackResult providerPackFromJson (std::string_view, PackDescriptorUse = PackDescriptorUse::Download);
} // namespace duskstudio::sfz
