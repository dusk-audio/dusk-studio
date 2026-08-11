#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzCatalog.h"
#include "engine/sfz/SfzCatalogEnvelope.h"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Json = nlohmann::json;
using duskstudio::sfz::CatalogEnvelopeResult;
using duskstudio::sfz::CatalogParseResult;
using duskstudio::sfz::TrustedCatalogKey;
using duskstudio::sfz::parseCatalogPayload;
using duskstudio::sfz::verifyCatalogEnvelope;

const std::string kShaA (64, 'a');
const std::string kShaB (64, 'b');
const std::string kGitRevision (40, 'c');

Json validCatalog()
{
    return {
        { "catalog_id", "dusk.sfz" },
        { "schema_version", 1 },
        { "catalog_revision", 7 },
        { "generated_at", "2026-08-10T12:00:00Z" },
        { "packs", Json::array ({
            {
                { "id", "body-percussion" },
                { "release_id", "v1.0.0+mirror.1" },
                { "display_name", "Body Percussion" },
                { "author", "SFZ Instruments" },
                { "source_url", "https://github.com/example/body/tree/012345" },
                { "source_revision", kGitRevision },
                { "download_url", "https://catalog.dusk.audio/body-v1.zip" },
                { "archive_sha256", kShaA },
                { "archive_format", "zip" },
                { "expected_root", "body-percussion" },
                { "compressed_bytes", 1024 },
                { "expanded_bytes", 4096 },
                { "max_files", 12 },
                { "license", {
                    { "spdx", "CC0-1.0" },
                    { "source_url", "https://github.com/example/body/blob/012345/LICENSE" },
                    { "file", "LICENSE" },
                    { "file_sha256", kShaB },
                    { "redistribution_allowed", true },
                    { "mirror_allowed", true }
                } },
                { "instruments", Json::array ({
                    {
                        { "id", "full-kit" },
                        { "name", "Full Kit" },
                        { "relative_entrypoint", "Instruments/Full Kit.sfz" }
                    }
                }) }
            }
        }) }
    };
}

CatalogParseResult parse (const Json& catalog)
{
    return parseCatalogPayload (catalog.dump());
}

void checkRejected (const Json& catalog, const std::string& errorPath)
{
    const auto result = parse (catalog);
    CHECK_FALSE (result);
    CHECK_FALSE (result.catalog.has_value());
    CHECK (result.error.find (errorPath) != std::string::npos);
}

struct SigningFixture
{
    explicit SigningFixture (std::string keyId = "catalog-2026-a",
                             unsigned char seedByte = 0x2a)
    {
        if (sodium_init() < 0)
            throw std::runtime_error ("libsodium initialization failed");

        std::array<unsigned char, crypto_sign_SEEDBYTES> seed {};
        seed.fill (seedByte);
        key.id = std::move (keyId);
        if (crypto_sign_seed_keypair (key.publicKey.data(), secretKey.data(),
                                      seed.data()) != 0)
            throw std::runtime_error ("Ed25519 key generation failed");
    }

    TrustedCatalogKey key;
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> secretKey {};
};

std::string encodeBase64 (const unsigned char* bytes, std::size_t size)
{
    const auto encodedBytes = sodium_base64_encoded_len (
        size, sodium_base64_VARIANT_ORIGINAL);
    std::string encoded (encodedBytes, '\0');
    if (sodium_bin2base64 (encoded.data(), encoded.size(), bytes, size,
                           sodium_base64_VARIANT_ORIGINAL) == nullptr)
        throw std::runtime_error ("base64 encoding failed");
    encoded.resize (encodedBytes - 1);
    return encoded;
}

Json signedEnvelope (const std::string& payload, const SigningFixture& signing)
{
    std::array<unsigned char, crypto_sign_BYTES> signature {};
    unsigned long long signatureBytes = 0;
    if (crypto_sign_detached (
            signature.data(), &signatureBytes,
            reinterpret_cast<const unsigned char*> (payload.data()),
            static_cast<unsigned long long> (payload.size()),
            signing.secretKey.data()) != 0
        || signatureBytes != signature.size())
        throw std::runtime_error ("Ed25519 signing failed");

    return {
        { "envelope_schema_version", 1 },
        { "algorithm", "Ed25519" },
        { "key_id", signing.key.id },
        { "payload_base64", encodeBase64 (
            reinterpret_cast<const unsigned char*> (payload.data()), payload.size()) },
        { "signature_base64", encodeBase64 (signature.data(), signature.size()) }
    };
}

CatalogEnvelopeResult verify (const Json& envelope,
                              const std::vector<TrustedCatalogKey>& keys)
{
    return verifyCatalogEnvelope (envelope.dump(), keys);
}

void checkEnvelopeRejected (const Json& envelope,
                            const std::vector<TrustedCatalogKey>& keys,
                            const std::string& errorPath)
{
    const auto result = verify (envelope, keys);
    CHECK_FALSE (result);
    CHECK_FALSE (result.catalog.has_value());
    CHECK (result.verifiedKeyId.empty());
    CHECK (result.error.find (errorPath) != std::string::npos);
}
} // namespace

TEST_CASE ("SFZ catalog accepts a complete schema v1 payload", "[sfz][catalog]")
{
    auto source = validCatalog();
    source["packs"][0]["tags"] = Json::array ({ "percussion", "cc0" });
    source["packs"][0]["yanked"] = true;

    const auto result = parse (source);
    REQUIRE (result);
    REQUIRE (result.error.empty());
    REQUIRE (result.catalog.has_value());

    const auto& catalog = *result.catalog;
    CHECK (catalog.catalogId == "dusk.sfz");
    CHECK (catalog.schemaVersion == 1);
    CHECK (catalog.catalogRevision == 7);
    CHECK (catalog.generatedAt == "2026-08-10T12:00:00Z");
    REQUIRE (catalog.packs.size() == 1);

    const auto& pack = catalog.packs.front();
    CHECK (pack.id == "body-percussion");
    CHECK (pack.releaseId == "v1.0.0+mirror.1");
    CHECK (pack.sourceRevision == kGitRevision);
    CHECK (pack.archiveFormat == "zip");
    CHECK (pack.expectedRoot == "body-percussion");
    CHECK (pack.compressedBytes == 1024);
    CHECK (pack.expandedBytes == 4096);
    CHECK (pack.maxFiles == 12);
    CHECK (pack.license.spdx == "CC0-1.0");
    CHECK (pack.license.redistributionAllowed);
    CHECK (pack.license.mirrorAllowed);
    REQUIRE (pack.instruments.size() == 1);
    CHECK (pack.instruments.front().relativeEntrypoint
           == "Instruments/Full Kit.sfz");
    CHECK (pack.tags == std::vector<std::string> { "percussion", "cc0" });
    CHECK (pack.yanked);
}

TEST_CASE ("SFZ catalog rejects unknown schema v1 fields", "[sfz][catalog]")
{
    SECTION ("root")
    {
        auto catalog = validCatalog();
        catalog["future_root_field"] = true;
        checkRejected (catalog, "root.future_root_field");
    }
    SECTION ("pack")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["future_pack_field"] = true;
        checkRejected (catalog, "root.packs[0].future_pack_field");
    }
    SECTION ("license")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["future_license_field"] = true;
        checkRejected (catalog, "license.future_license_field");
    }
    SECTION ("instrument")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["instruments"][0]["future_instrument_field"] = true;
        checkRejected (catalog, "instruments[0].future_instrument_field");
    }
}

TEST_CASE ("SFZ catalog rejects oversized payloads before JSON parsing",
           "[sfz][catalog]")
{
    const std::string oversized (8 * 1024 * 1024 + 1, ' ');
    CatalogParseResult result;
    REQUIRE_NOTHROW (result = parseCatalogPayload (oversized));
    CHECK_FALSE (result);
    CHECK (result.error == "root: payload exceeds the 8 MiB limit");
}

TEST_CASE ("SFZ catalog rejects duplicate JSON object keys", "[sfz][catalog]")
{
    SECTION ("root object")
    {
        auto source = validCatalog().dump();
        source.insert (1, R"("catalog_id":"shadow",)");

        const auto result = parseCatalogPayload (source);
        CHECK_FALSE (result);
        CHECK (result.error == "root: duplicate object key 'catalog_id'");
    }
    SECTION ("nested object")
    {
        auto source = validCatalog().dump();
        const std::string marker { R"("license":{)" };
        const auto position = source.find (marker);
        REQUIRE (position != std::string::npos);
        source.insert (position + marker.size(), R"("spdx":"shadow",)");

        const auto result = parseCatalogPayload (source);
        CHECK_FALSE (result);
        CHECK (result.error == "root: duplicate object key 'spdx'");
    }
}

TEST_CASE ("SFZ catalog rejects malformed roots and unsupported schemas",
           "[sfz][catalog]")
{
    SECTION ("malformed JSON does not throw")
    {
        CatalogParseResult result;
        REQUIRE_NOTHROW (result = parseCatalogPayload ("{not-json"));
        CHECK_FALSE (result);
        CHECK (result.error.find ("malformed JSON") != std::string::npos);
    }
    SECTION ("root must be an object")
    {
        const auto result = parseCatalogPayload ("[]");
        CHECK_FALSE (result);
        CHECK (result.error.find ("root") != std::string::npos);
    }
    SECTION ("schema is required")
    {
        auto catalog = validCatalog();
        catalog.erase ("schema_version");
        checkRejected (catalog, "root.schema_version");
    }
    SECTION ("future schema")
    {
        auto catalog = validCatalog();
        catalog["schema_version"] = 2;
        checkRejected (catalog, "root.schema_version");
    }
    SECTION ("fractional schema")
    {
        auto catalog = validCatalog();
        catalog["schema_version"] = 1.0;
        checkRejected (catalog, "root.schema_version");
    }
    SECTION ("revision must be positive")
    {
        auto catalog = validCatalog();
        catalog["catalog_revision"] = 0;
        checkRejected (catalog, "root.catalog_revision");
    }
    SECTION ("packs must not be empty")
    {
        auto catalog = validCatalog();
        catalog["packs"] = Json::array();
        checkRejected (catalog, "root.packs");
    }
}

TEST_CASE ("SFZ catalog requires typed security fields", "[sfz][catalog]")
{
    SECTION ("archive digest cannot default")
    {
        auto catalog = validCatalog();
        catalog["packs"][0].erase ("archive_sha256");
        checkRejected (catalog, "archive_sha256");
    }
    SECTION ("source revision cannot default")
    {
        auto catalog = validCatalog();
        catalog["packs"][0].erase ("source_revision");
        checkRejected (catalog, "source_revision");
    }
    SECTION ("license permission cannot default")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"].erase ("mirror_allowed");
        checkRejected (catalog, "mirror_allowed");
    }
    SECTION ("instrument entrypoint cannot default")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["instruments"][0].erase ("relative_entrypoint");
        checkRejected (catalog, "relative_entrypoint");
    }
    SECTION ("integer fields reject strings")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["compressed_bytes"] = "1024";
        checkRejected (catalog, "compressed_bytes");
    }
    SECTION ("boolean fields reject integers")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["mirror_allowed"] = 1;
        checkRejected (catalog, "mirror_allowed");
    }
}

TEST_CASE ("SFZ catalog constrains identities and rejects duplicates",
           "[sfz][catalog]")
{
    SECTION ("stable IDs are conservative lowercase ASCII")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["id"] = "Body Percussion";
        checkRejected (catalog, "root.packs[0].id");
    }
    SECTION ("opaque release IDs remain bounded conservative tokens")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["release_id"] = "release/../../../other";
        checkRejected (catalog, "root.packs[0].release_id");
    }
    SECTION ("release IDs reject non-ASCII bytes")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["release_id"] = std::string ("v1-") + "\xc3\xa9-x";
        checkRejected (catalog, "root.packs[0].release_id");
    }
    SECTION ("pack and release identity is unique")
    {
        auto catalog = validCatalog();
        catalog["packs"].push_back (catalog["packs"][0]);
        checkRejected (catalog, "root.packs[1]");
    }
    SECTION ("instrument identity is unique within a release")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["instruments"].push_back (
            catalog["packs"][0]["instruments"][0]);
        checkRejected (catalog, "root.packs[0].instruments[1].id");
    }
}

TEST_CASE ("SFZ catalog requires immutable normalized mirror fields",
           "[sfz][catalog]")
{
    SECTION ("40-digit Git object ID")
    {
        const auto result = parse (validCatalog());
        REQUIRE (result);
        CHECK (result.catalog->packs[0].sourceRevision == kGitRevision);
    }
    SECTION ("64-digit Git object ID")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["source_revision"] = std::string (64, 'd');
        const auto result = parse (catalog);
        REQUIRE (result);
        CHECK (result.catalog->packs[0].sourceRevision == std::string (64, 'd'));
    }
    SECTION ("Git object ID length")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["source_revision"] = std::string (39, 'c');
        checkRejected (catalog, "source_revision");
    }
    SECTION ("Git object ID lowercase hex")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["source_revision"] = std::string (40, 'C');
        checkRejected (catalog, "source_revision");
    }
    SECTION ("archive format")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["archive_format"] = "tar.gz";
        checkRejected (catalog, "archive_format");
    }
    SECTION ("expected root equals pack ID")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["expected_root"] = "other-root";
        checkRejected (catalog, "expected_root");
    }
    SECTION ("expected root is traversal-safe")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["expected_root"] = "../body-percussion";
        checkRejected (catalog, "expected_root");
    }
}

TEST_CASE ("SFZ catalog requires HTTPS and lowercase SHA-256 fields",
           "[sfz][catalog]")
{
    SECTION ("pack source uses HTTPS")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["source_url"] = "http://example.test/source";
        checkRejected (catalog, "source_url");
    }
    SECTION ("download uses HTTPS")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["download_url"] = "file:///tmp/archive.zip";
        checkRejected (catalog, "download_url");
    }
    SECTION ("archive hash is lowercase hex")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["archive_sha256"] = std::string (64, 'A');
        checkRejected (catalog, "archive_sha256");
    }
    SECTION ("license hash has exact length")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["file_sha256"] = "abc";
        checkRejected (catalog, "file_sha256");
    }
}

TEST_CASE ("SFZ catalog requires ASCII DNS HTTPS authorities", "[sfz][catalog]")
{
    auto catalog = validCatalog();
    auto& url = catalog["packs"][0]["download_url"];

    SECTION ("space") { url = "https://bad host.example/archive.zip"; }
    SECTION ("backslash") { url = "https://bad\\host.example/archive.zip"; }
    SECTION ("percent escape") { url = "https://bad%20host.example/archive.zip"; }
    SECTION ("empty label") { url = "https://bad..example/archive.zip"; }
    SECTION ("leading hyphen") { url = "https://-bad.example/archive.zip"; }
    SECTION ("trailing hyphen") { url = "https://bad-.example/archive.zip"; }
    SECTION ("non-ASCII")
    {
        url = std::string ("https://m") + "\xc3\xbc" + "sic.example/archive.zip";
    }
    SECTION ("label longer than 63 bytes")
    {
        url = "https://" + std::string (64, 'a') + ".example/archive.zip";
    }
    SECTION ("authority longer than 253 bytes")
    {
        const auto label = std::string (63, 'a');
        url = "https://" + label + "." + label + "." + label + "." + label
            + "/archive.zip";
    }

    checkRejected (catalog, "download_url");
}

TEST_CASE ("SFZ catalog bounds extraction declarations", "[sfz][catalog]")
{
    SECTION ("compressed size must be positive")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["compressed_bytes"] = 0;
        checkRejected (catalog, "compressed_bytes");
    }
    SECTION ("compressed size has a global ceiling")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["compressed_bytes"] = 8589934593ULL;
        checkRejected (catalog, "compressed_bytes");
    }
    SECTION ("compressed size may exceed expanded size")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["compressed_bytes"] = 4096;
        catalog["packs"][0]["expanded_bytes"] = 512;
        const auto result = parse (catalog);
        REQUIRE (result);
        CHECK (result.catalog->packs[0].compressedBytes == 4096);
        CHECK (result.catalog->packs[0].expandedBytes == 512);
    }
    SECTION ("expanded size must be positive")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["expanded_bytes"] = 0;
        checkRejected (catalog, "expanded_bytes");
    }
    SECTION ("expanded size has a global ceiling")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["expanded_bytes"] = 34359738369ULL;
        checkRejected (catalog, "expanded_bytes");
    }
    SECTION ("file count has a ceiling")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["max_files"] = 100001;
        checkRejected (catalog, "max_files");
    }
    SECTION ("file count must be integral")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["max_files"] = 12.5;
        checkRejected (catalog, "max_files");
    }
}

TEST_CASE ("SFZ catalog enforces the schema v1 license policy", "[sfz][catalog]")
{
    SECTION ("only CC0 is allowed")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["spdx"] = "CC-BY-4.0";
        checkRejected (catalog, "license.spdx");
    }
    SECTION ("redistribution must be allowed")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["redistribution_allowed"] = false;
        checkRejected (catalog, "redistribution_allowed");
    }
    SECTION ("mirroring must be allowed")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["mirror_allowed"] = false;
        checkRejected (catalog, "mirror_allowed");
    }
    SECTION ("license source uses HTTPS")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["source_url"] = "http://example.test/LICENSE";
        checkRejected (catalog, "license.source_url");
    }
}

TEST_CASE ("SFZ catalog rejects unsafe or unsupported relative paths",
           "[sfz][catalog]")
{
    auto catalog = validCatalog();
    auto& entrypoint = catalog["packs"][0]["instruments"][0]["relative_entrypoint"];

    SECTION ("absolute path") { entrypoint = "/etc/passwd.sfz"; }
    SECTION ("UNC path") { entrypoint = "//server/share/file.sfz"; }
    SECTION ("drive root") { entrypoint = "C:/pack/file.sfz"; }
    SECTION ("backslash") { entrypoint = "pack\\file.sfz"; }
    SECTION ("parent segment") { entrypoint = "pack/../file.sfz"; }
    SECTION ("dot segment") { entrypoint = "pack/./file.sfz"; }
    SECTION ("empty segment") { entrypoint = "pack//file.sfz"; }
    SECTION ("trailing slash") { entrypoint = "pack/file.sfz/"; }
    SECTION ("unsupported extension") { entrypoint = "pack/file.wav"; }
    SECTION ("uppercase extension") { entrypoint = "pack/file.SFZ"; }
    SECTION ("NUL") { entrypoint = std::string ("pack/file\0.sfz", 14); }
    SECTION ("colon") { entrypoint = "pack/file:stream.sfz"; }
    SECTION ("other Win32-invalid characters")
    {
        for (const auto invalid : std::string ("<>\"|?*"))
        {
            CAPTURE (invalid);
            entrypoint = "pack/file.sfz";
            entrypoint.get_ref<std::string&>().insert (9, 1, invalid);
            checkRejected (catalog, "relative_entrypoint");
        }
        return;
    }
    SECTION ("segment ending in dot") { entrypoint = "pack./file.sfz"; }
    SECTION ("segment ending in space") { entrypoint = "pack /file.sfz"; }
    SECTION ("reserved basename with extension") { entrypoint = "pack/CON.sfz"; }
    SECTION ("case-insensitive COM device") { entrypoint = "pack/cOm9.sfz"; }
    SECTION ("case-insensitive LPT device") { entrypoint = "pack/LpT1.preset.sfz"; }
    SECTION ("COM device with superscript one")
    {
        entrypoint = std::string ("pack/COM") + "\xc2\xb9" + ".sfz";
    }
    SECTION ("LPT device with superscript three and extension")
    {
        entrypoint = std::string ("pack/LPT") + "\xc2\xb3" + ".preset.sfz";
    }

    checkRejected (catalog, "relative_entrypoint");
}

TEST_CASE ("SFZ catalog bounds relative path depth and length", "[sfz][catalog]")
{
    SECTION ("path depth")
    {
        auto catalog = validCatalog();
        std::string path;
        for (int i = 0; i < 33; ++i)
            path += "dir/";
        path += "file.sfz";
        catalog["packs"][0]["instruments"][0]["relative_entrypoint"] = path;
        checkRejected (catalog, "relative_entrypoint");
    }
    SECTION ("path length")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["license"]["file"] = std::string (513, 'a');
        checkRejected (catalog, "license.file");
    }
}

TEST_CASE ("SFZ catalog validates arrays and optional pack metadata",
           "[sfz][catalog]")
{
    SECTION ("instruments must not be empty")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["instruments"] = Json::array();
        checkRejected (catalog, "instruments");
    }
    SECTION ("instruments must be an array")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["instruments"] = Json::object();
        checkRejected (catalog, "instruments");
    }
    SECTION ("tags must be an array")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["tags"] = "percussion";
        checkRejected (catalog, "tags");
    }
    SECTION ("tags are unique")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["tags"] = Json::array ({ "cc0", "cc0" });
        checkRejected (catalog, "tags[1]");
    }
    SECTION ("yanked must be a boolean")
    {
        auto catalog = validCatalog();
        catalog["packs"][0]["yanked"] = "false";
        checkRejected (catalog, "yanked");
    }
}

TEST_CASE ("SFZ catalog envelope verifies exact payload bytes",
           "[sfz][catalog][envelope]")
{
    const SigningFixture signing;
    const auto payload = validCatalog().dump();
    const auto envelope = signedEnvelope (payload, signing);

    const auto result = verify (envelope, { signing.key });
    REQUIRE (result);
    REQUIRE (result.error.empty());
    REQUIRE (result.catalog.has_value());
    CHECK (result.verifiedKeyId == "catalog-2026-a");
    CHECK (result.catalog->catalogId == "dusk.sfz");
    CHECK (result.catalog->catalogRevision == 7);
}

TEST_CASE ("SFZ catalog envelope rejects payload and signature tampering",
           "[sfz][catalog][envelope]")
{
    const SigningFixture signing;
    const auto originalPayload = validCatalog().dump();

    SECTION ("semantically valid payload with different exact bytes")
    {
        auto envelope = signedEnvelope (originalPayload, signing);
        const auto reformatted = validCatalog().dump (2);
        envelope["payload_base64"] = encodeBase64 (
            reinterpret_cast<const unsigned char*> (reformatted.data()),
            reformatted.size());
        checkEnvelopeRejected (envelope, { signing.key }, "signature verification failed");
    }
    SECTION ("signature bytes")
    {
        auto envelope = signedEnvelope (originalPayload, signing);
        auto& encoded = envelope["signature_base64"].get_ref<std::string&>();
        encoded[0] = encoded[0] == 'A' ? 'B' : 'A';
        checkEnvelopeRejected (envelope, { signing.key }, "signature verification failed");
    }
    SECTION ("signed malformed payload is rejected after verification")
    {
        const auto envelope = signedEnvelope ("{not-json", signing);
        checkEnvelopeRejected (envelope, { signing.key }, "payload: root: malformed JSON");
    }
    SECTION ("signed future payload schema remains fail-closed")
    {
        auto catalog = validCatalog();
        catalog["schema_version"] = 2;
        const auto envelope = signedEnvelope (catalog.dump(), signing);
        checkEnvelopeRejected (envelope, { signing.key }, "payload: root.schema_version");
    }
    SECTION ("signed payload is bound to the Dusk catalog identity")
    {
        auto catalog = validCatalog();
        catalog["catalog_id"] = "other.catalog";
        const auto envelope = signedEnvelope (catalog.dump(), signing);
        checkEnvelopeRejected (envelope, { signing.key },
                               "payload: root.catalog_id is not dusk.sfz");
    }
    SECTION ("signed payload with duplicate keys remains fail-closed")
    {
        auto payload = validCatalog().dump();
        payload.insert (1, R"("catalog_id":"shadow",)");
        const auto envelope = signedEnvelope (payload, signing);
        checkEnvelopeRejected (envelope, { signing.key }, "payload: root: duplicate object key");
    }
}

TEST_CASE ("SFZ catalog envelope selects only an unambiguous trusted key",
           "[sfz][catalog][envelope]")
{
    const SigningFixture signingA { "catalog-2026-a", 0x2a };
    const SigningFixture signingB { "catalog-2026-b", 0x5c };
    auto envelope = signedEnvelope (validCatalog().dump(), signingA);

    SECTION ("unknown key ID")
    {
        envelope["key_id"] = "catalog-unknown";
        checkEnvelopeRejected (envelope, { signingA.key }, "key is not trusted");
    }
    SECTION ("rotated key can follow an older trusted key")
    {
        const auto rotatedEnvelope = signedEnvelope (validCatalog().dump(), signingB);
        const auto result = verify (rotatedEnvelope, { signingA.key, signingB.key });
        REQUIRE (result);
        CHECK (result.verifiedKeyId == signingB.key.id);
    }
    SECTION ("known ID with the wrong public key")
    {
        auto wrongKey = signingB.key;
        wrongKey.id = signingA.key.id;
        checkEnvelopeRejected (envelope, { wrongKey }, "signature verification failed");
    }
    SECTION ("signature key and declared key differ")
    {
        envelope["key_id"] = signingB.key.id;
        checkEnvelopeRejected (envelope, { signingA.key, signingB.key },
                               "signature verification failed");
    }
    SECTION ("empty registry")
    {
        checkEnvelopeRejected (envelope, {}, "trusted_keys");
    }
    SECTION ("duplicate registry IDs")
    {
        checkEnvelopeRejected (envelope, { signingA.key, signingA.key },
                               "duplicate key identifier");
    }
    SECTION ("duplicate registry key material")
    {
        auto alias = signingA.key;
        alias.id = "catalog-2026-alias";
        checkEnvelopeRejected (envelope, { signingA.key, alias },
                               "duplicate public key material");
    }
    SECTION ("invalid registry ID")
    {
        auto invalid = signingA.key;
        invalid.id = "Invalid Key";
        checkEnvelopeRejected (envelope, { invalid }, "invalid key identifier");
    }
    SECTION ("all-zero public key")
    {
        auto invalid = signingA.key;
        invalid.publicKey.fill (0);
        checkEnvelopeRejected (envelope, { invalid }, "invalid public key");
    }
    SECTION ("registry has a fixed ceiling")
    {
        std::vector<TrustedCatalogKey> keys;
        for (int i = 0; i < 17; ++i)
        {
            auto key = signingA.key;
            key.id = "catalog-key-" + std::to_string (i);
            keys.push_back (std::move (key));
        }
        checkEnvelopeRejected (envelope, keys, "between 1 and 16 keys");
    }
}

TEST_CASE ("SFZ catalog envelope schema is closed and versioned",
           "[sfz][catalog][envelope]")
{
    const SigningFixture signing;
    auto envelope = signedEnvelope (validCatalog().dump(), signing);

    SECTION ("unknown field")
    {
        envelope["future_field"] = true;
        checkEnvelopeRejected (envelope, { signing.key }, "envelope.future_field");
    }
    SECTION ("future schema")
    {
        envelope["envelope_schema_version"] = 2;
        checkEnvelopeRejected (envelope, { signing.key }, "envelope_schema_version");
    }
    SECTION ("fractional schema")
    {
        envelope["envelope_schema_version"] = 1.0;
        checkEnvelopeRejected (envelope, { signing.key }, "expected an integer");
    }
    SECTION ("algorithm confusion")
    {
        envelope["algorithm"] = "Ed25519ph";
        checkEnvelopeRejected (envelope, { signing.key }, "envelope.algorithm");
    }
    SECTION ("invalid key ID")
    {
        envelope["key_id"] = "Catalog Key";
        checkEnvelopeRejected (envelope, { signing.key }, "envelope.key_id");
    }
    SECTION ("missing signature")
    {
        envelope.erase ("signature_base64");
        checkEnvelopeRejected (envelope, { signing.key }, "signature_base64");
    }
}

TEST_CASE ("SFZ catalog envelope rejects duplicate keys and malformed JSON",
           "[sfz][catalog][envelope]")
{
    const SigningFixture signing;

    SECTION ("duplicate envelope key")
    {
        auto source = signedEnvelope (validCatalog().dump(), signing).dump();
        source.insert (1, R"("algorithm":"shadow",)");
        const auto result = verifyCatalogEnvelope (source, { signing.key });
        CHECK_FALSE (result);
        CHECK (result.error == "envelope: duplicate object key 'algorithm'");
    }
    SECTION ("malformed JSON")
    {
        CatalogEnvelopeResult result;
        REQUIRE_NOTHROW (result = verifyCatalogEnvelope ("{not-json", { signing.key }));
        CHECK_FALSE (result);
        CHECK (result.error == "envelope: malformed JSON");
    }
    SECTION ("non-object root")
    {
        const auto result = verifyCatalogEnvelope ("[]", { signing.key });
        CHECK_FALSE (result);
        CHECK (result.error == "envelope: expected an object");
    }
}

TEST_CASE ("SFZ catalog envelope requires canonical bounded base64",
           "[sfz][catalog][envelope]")
{
    const SigningFixture signing;
    auto envelope = signedEnvelope (validCatalog().dump(), signing);

    SECTION ("signature whitespace")
    {
        envelope["signature_base64"] =
            envelope["signature_base64"].get<std::string>() + " ";
        checkEnvelopeRejected (envelope, { signing.key }, "signature_base64");
    }
    SECTION ("signature padding is required")
    {
        auto& encoded = envelope["signature_base64"].get_ref<std::string&>();
        REQUIRE (encoded.back() == '=');
        encoded.pop_back();
        checkEnvelopeRejected (envelope, { signing.key }, "signature_base64");
    }
    SECTION ("signature has exact decoded length")
    {
        const std::array<unsigned char, 63> shortSignature {};
        envelope["signature_base64"] = encodeBase64 (
            shortSignature.data(), shortSignature.size());
        checkEnvelopeRejected (envelope, { signing.key }, "must be 64 bytes");
    }
    SECTION ("payload base64 is syntactically valid")
    {
        envelope["payload_base64"] = "***=";
        checkEnvelopeRejected (envelope, { signing.key }, "payload_base64");
    }
}

TEST_CASE ("SFZ catalog envelope bounds raw input before parsing",
           "[sfz][catalog][envelope]")
{
    const std::string oversized (12 * 1024 * 1024 + 1, ' ');
    CatalogEnvelopeResult result;
    REQUIRE_NOTHROW (result = verifyCatalogEnvelope (oversized, {}));
    CHECK_FALSE (result);
    CHECK (result.error == "envelope: exceeds the 12 MiB limit");
}
