#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::sfz
{
struct CatalogLicense
{
    std::string spdx;
    std::string sourceUrl;
    std::string file;
    std::string fileSha256;
    bool redistributionAllowed { false };
    bool mirrorAllowed { false };
};

struct CatalogInstrument
{
    std::string id;
    std::string name;
    std::string relativeEntrypoint;
};

struct CatalogPack
{
    std::string id;
    std::string releaseId;
    std::string displayName;
    std::string author;
    std::string sourceUrl;
    std::string sourceRevision;
    std::string downloadUrl;
    std::string archiveSha256;
    std::string archiveFormat;
    std::string expectedRoot;
    std::uint64_t compressedBytes { 0 };
    std::uint64_t expandedBytes { 0 };
    std::uint32_t maxFiles { 0 };
    CatalogLicense license;
    std::vector<CatalogInstrument> instruments;
    std::vector<std::string> tags;
    bool yanked { false };
};

struct CatalogPayload
{
    std::string catalogId;
    std::uint32_t schemaVersion { 0 };
    std::uint64_t catalogRevision { 0 };
    std::string generatedAt;
    std::vector<CatalogPack> packs;
};

struct CatalogParseResult
{
    std::optional<CatalogPayload> catalog;
    std::string error;

    explicit operator bool() const noexcept { return catalog.has_value(); }
};

CatalogParseResult parseCatalogPayload (std::string_view source) noexcept;
} // namespace duskstudio::sfz
