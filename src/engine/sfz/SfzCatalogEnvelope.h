#pragma once

#include "SfzCatalog.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::sfz
{
struct TrustedCatalogKey
{
    std::string id;
    std::array<std::uint8_t, 32> publicKey {};
};

struct CatalogEnvelopeResult
{
    std::optional<CatalogPayload> catalog;
    std::string verifiedKeyId;
    std::string error;

    explicit operator bool() const noexcept { return catalog.has_value(); }
};

// Verifies the detached Ed25519 signature over the exact decoded payload bytes
// before handing those bytes to the catalog payload parser. The envelope is a
// transport container only; its JSON representation is never signed or
// canonicalized.
CatalogEnvelopeResult verifyCatalogEnvelope (
    std::string_view envelope,
    const std::vector<TrustedCatalogKey>& trustedKeys) noexcept;
} // namespace duskstudio::sfz
