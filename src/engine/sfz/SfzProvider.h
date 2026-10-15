#pragma once

#include "SfzTransfer.h"

#include <optional>
#include <cstdint>
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

std::vector<ProviderSource> providerSources();
ProviderPageResult browseProvider (HttpTransport&, const std::string& sourceId,
                                   unsigned page = 1, const std::string& query = {},
                                   const OperationControl& = {});
ProviderPackResult resolveProviderPack (HttpTransport&, const ProviderSummary&,
                                       const OperationControl& = {});

std::string providerPackToJson (const ProviderPack&);
ProviderPackResult providerPackFromJson (std::string_view);
} // namespace duskstudio::sfz
