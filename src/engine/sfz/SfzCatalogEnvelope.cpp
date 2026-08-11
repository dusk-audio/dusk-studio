#include "SfzCatalogEnvelope.h"
#include "SfzCatalogJson.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duskstudio::sfz
{
namespace
{
using Json = dusk::json::Json;

constexpr std::uint32_t kEnvelopeSchemaVersion = 1;
constexpr std::size_t kMaxEnvelopeBytes = 12ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxPayloadBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxPayloadBase64Bytes = ((kMaxPayloadBytes + 2ULL) / 3ULL) * 4ULL;
constexpr std::size_t kMaxKeyIdBytes = 64;
constexpr std::size_t kMaxTrustedKeys = 16;
constexpr std::string_view kExpectedCatalogId { "dusk.sfz" };

static_assert (std::tuple_size<decltype (TrustedCatalogKey::publicKey)>::value
               == crypto_sign_PUBLICKEYBYTES);

bool isLowerAlphaNumeric (unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isValidKeyId (std::string_view value)
{
    if (value.empty() || value.size() > kMaxKeyIdBytes
        || ! isLowerAlphaNumeric (static_cast<unsigned char> (value.front()))
        || ! isLowerAlphaNumeric (static_cast<unsigned char> (value.back())))
        return false;

    return std::all_of (value.begin(), value.end(), [] (unsigned char c)
    {
        return isLowerAlphaNumeric (c) || c == '-' || c == '_' || c == '.';
    });
}

struct EnvelopeParser
{
    std::string error;

    bool fail (std::string path, std::string reason)
    {
        error = std::move (path) + ": " + std::move (reason);
        return false;
    }

    bool validateKeys (const Json& object, const std::string& path,
                       std::initializer_list<std::string_view> allowed)
    {
        for (const auto& item : object.items())
        {
            const auto known = std::any_of (allowed.begin(), allowed.end(),
                [&item] (std::string_view key) { return item.key() == key; });
            if (! known)
                return fail (path + "." + detail::sanitizeJsonKeyForDiagnostic (item.key()),
                             "unknown field");
        }
        return true;
    }

    const Json* required (const Json& object, const char* key)
    {
        const auto it = object.find (key);
        if (it == object.end())
        {
            fail (std::string ("envelope.") + key, "required field is missing");
            return nullptr;
        }
        return &*it;
    }

    bool readString (const Json& object, const char* key, std::string& out,
                     std::size_t maximum)
    {
        const auto* value = required (object, key);
        if (value == nullptr)
            return false;
        if (! value->is_string())
            return fail (std::string ("envelope.") + key, "expected a string");

        out = value->get<std::string>();
        if (out.empty())
            return fail (std::string ("envelope.") + key, "must not be empty");
        if (out.size() > maximum)
            return fail (std::string ("envelope.") + key, "exceeds the length limit");
        return true;
    }

    bool parse (const Json& root, std::string& keyId, std::string& payloadBase64,
                std::string& signatureBase64)
    {
        if (! root.is_object())
            return fail ("envelope", "expected an object");
        if (! validateKeys (root, "envelope",
                            { "envelope_schema_version", "algorithm", "key_id",
                              "payload_base64", "signature_base64" }))
            return false;

        const auto* schema = required (root, "envelope_schema_version");
        if (schema == nullptr)
            return false;
        if (! schema->is_number_unsigned() && ! schema->is_number_integer())
            return fail ("envelope.envelope_schema_version", "expected an integer");

        std::uint64_t schemaVersion = 0;
        if (schema->is_number_unsigned())
            schemaVersion = schema->get<std::uint64_t>();
        else
        {
            const auto signedVersion = schema->get<std::int64_t>();
            if (signedVersion <= 0)
                return fail ("envelope.envelope_schema_version", "must be positive");
            schemaVersion = static_cast<std::uint64_t> (signedVersion);
        }
        if (schemaVersion != kEnvelopeSchemaVersion)
            return fail ("envelope.envelope_schema_version",
                         "unsupported envelope schema version");

        std::string algorithm;
        if (! readString (root, "algorithm", algorithm, 32)
            || ! readString (root, "key_id", keyId, kMaxKeyIdBytes)
            || ! readString (root, "payload_base64", payloadBase64,
                             kMaxPayloadBase64Bytes)
            || ! readString (root, "signature_base64", signatureBase64, 128))
            return false;

        if (algorithm != "Ed25519")
            return fail ("envelope.algorithm", "must be Ed25519");
        if (! isValidKeyId (keyId))
            return fail ("envelope.key_id", "contains an unsupported key identifier");
        return true;
    }
};

bool decodeCanonicalBase64 (const std::string& encoded, std::size_t maximum,
                            const char* field, std::vector<unsigned char>& decoded,
                            std::string& error)
{
    decoded.resize (std::min (encoded.size(), maximum));
    std::size_t decodedBytes = 0;
    const char* end = nullptr;
    if (sodium_base642bin (decoded.data(), decoded.size(), encoded.data(), encoded.size(),
                           nullptr, &decodedBytes, &end,
                           sodium_base64_VARIANT_ORIGINAL) != 0
        || end != encoded.data() + encoded.size())
    {
        error = std::string ("envelope.") + field + ": invalid base64";
        return false;
    }
    decoded.resize (decodedBytes);

    const auto canonicalBytes = sodium_base64_encoded_len (
        decoded.size(), sodium_base64_VARIANT_ORIGINAL);
    std::string canonical (canonicalBytes, '\0');
    if (sodium_bin2base64 (canonical.data(), canonical.size(), decoded.data(),
                           decoded.size(), sodium_base64_VARIANT_ORIGINAL) == nullptr)
    {
        error = std::string ("envelope.") + field + ": base64 decoding failed";
        return false;
    }
    canonical.resize (canonicalBytes - 1);
    if (canonical != encoded)
    {
        error = std::string ("envelope.") + field + ": base64 is not canonical";
        return false;
    }
    return true;
}

const TrustedCatalogKey* validateAndFindKey (
    const std::vector<TrustedCatalogKey>& trustedKeys, std::string_view wanted,
    std::string& error)
{
    if (trustedKeys.empty() || trustedKeys.size() > kMaxTrustedKeys)
    {
        error = "trusted_keys: must contain between 1 and 16 keys";
        return nullptr;
    }

    std::unordered_set<std::string> keyIds;
    std::unordered_set<std::string> publicKeys;
    const TrustedCatalogKey* selected = nullptr;
    for (const auto& key : trustedKeys)
    {
        if (! isValidKeyId (key.id))
        {
            error = "trusted_keys: contains an invalid key identifier";
            return nullptr;
        }
        if (! keyIds.insert (key.id).second)
        {
            error = "trusted_keys: contains a duplicate key identifier";
            return nullptr;
        }
        if (crypto_core_ed25519_is_valid_point (key.publicKey.data()) != 1)
        {
            error = "trusted_keys: contains an invalid public key";
            return nullptr;
        }
        const std::string keyMaterial (
            reinterpret_cast<const char*> (key.publicKey.data()), key.publicKey.size());
        if (! publicKeys.insert (keyMaterial).second)
        {
            error = "trusted_keys: contains duplicate public key material";
            return nullptr;
        }
        if (key.id == wanted)
            selected = &key;
    }

    if (selected == nullptr)
        error = "envelope.key_id: key is not trusted";
    return selected;
}
} // namespace

CatalogEnvelopeResult verifyCatalogEnvelope (
    std::string_view envelope,
    const std::vector<TrustedCatalogKey>& trustedKeys) noexcept
{
    try
    {
        if (envelope.size() > kMaxEnvelopeBytes)
            return { std::nullopt, {}, "envelope: exceeds the 12 MiB limit" };
        if (sodium_init() < 0)
            return { std::nullopt, {}, "envelope: cryptographic initialization failed" };

        std::string error;
        auto parsedJson = detail::parseJsonRejectingDuplicateKeys (envelope, "envelope");
        if (! parsedJson)
            return { std::nullopt, {}, std::move (parsedJson.error) };

        EnvelopeParser parser;
        std::string keyId;
        std::string payloadBase64;
        std::string signatureBase64;
        if (! parser.parse (*parsedJson.root, keyId, payloadBase64, signatureBase64))
            return { std::nullopt, {}, std::move (parser.error) };

        const auto* key = validateAndFindKey (trustedKeys, keyId, error);
        if (key == nullptr)
            return { std::nullopt, {}, std::move (error) };

        std::vector<unsigned char> signature;
        if (! decodeCanonicalBase64 (signatureBase64, crypto_sign_BYTES,
                                     "signature_base64", signature, error))
            return { std::nullopt, {}, std::move (error) };
        if (signature.size() != crypto_sign_BYTES)
            return { std::nullopt, {},
                     "envelope.signature_base64: decoded signature must be 64 bytes" };

        std::vector<unsigned char> payload;
        if (! decodeCanonicalBase64 (payloadBase64, kMaxPayloadBytes,
                                     "payload_base64", payload, error))
            return { std::nullopt, {}, std::move (error) };
        if (payload.empty())
            return { std::nullopt, {}, "envelope.payload_base64: payload is empty" };

        if (crypto_sign_verify_detached (
                signature.data(), payload.data(),
                static_cast<unsigned long long> (payload.size()),
                key->publicKey.data()) != 0)
            return { std::nullopt, {}, "envelope.signature_base64: signature verification failed" };

        auto parsed = parseCatalogPayload (std::string_view (
            reinterpret_cast<const char*> (payload.data()), payload.size()));
        if (! parsed)
            return { std::nullopt, {}, "payload: " + parsed.error };
        if (parsed.catalog->catalogId != kExpectedCatalogId)
            return { std::nullopt, {}, "payload: root.catalog_id is not dusk.sfz" };
        return { std::move (parsed.catalog), std::move (keyId), {} };
    }
    catch (...)
    {
        return { std::nullopt, {}, "envelope: verification failed" };
    }
}
} // namespace duskstudio::sfz
