#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzPackStore.h"
#include "foundation/Fs.h"

#include <sodium.h>

#include <array>
#include <filesystem>
#include <string>

namespace
{
using namespace duskstudio::sfz;
namespace fs = std::filesystem;
using Json = dusk::json::Json;

std::string hash (const std::string& text)
{
    REQUIRE (sodium_init() >= 0);
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (text.data()), text.size());
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

struct StoreTree
{
    StoreTree() : root (fs::canonical (dusk::fs::createUniqueTempDirectory ("dusk-sfz-store-"))) {}
    ~StoreTree() { std::error_code ignored; fs::remove_all (root, ignored); }
    fs::path root;
};

ProviderPack provider (char version = 'a')
{
    ProviderPack pack;
    pack.sourceId = "musical-artifacts";
    pack.packId = "42";
    pack.releaseId = pack.expectedSha256 = std::string (64, version);
    pack.displayName = "Portable piano";
    pack.author = "Fixture author";
    pack.sourceUrl = "https://musical-artifacts.com/artifacts/42";
    pack.downloadUrl = pack.sourceUrl + "/piano.zip";
    pack.versionPinned = true;
    const std::string license = "CC0-1.0 https://creativecommons.org/publicdomain/zero/1.0/";
    pack.license = { "CC0-1.0", license, {}, hash (license), pack.sourceUrl, {} };
    pack.instruments.push_back ({ "piano", "Piano", u8"Programs/Étude.sfz", false });
    return pack;
}

fs::path stage (PackStore& store)
{
    std::string error;
    const auto staging = store.createStaging (error);
    REQUIRE_FALSE (staging.empty());
    REQUIRE (error.empty());
    REQUIRE (fs::create_directory (staging / "Programs"));
    REQUIRE (fs::create_directory (staging / "Samples"));
    REQUIRE (dusk::fs::writeStringToFile (staging / fs::u8path (u8"Programs/Étude.sfz"),
                                          "<region> sample=../Samples/tone.wav"));
    REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/tone.wav", "fixture sample"));
    return staging;
}

InstalledPack publish (PackStore& store, const ProviderPack& pack)
{
    const auto result = store.publish (pack, pack.expectedSha256, stage (store), { u8"Programs/Étude.sfz" });
    REQUIRE (result);
    REQUIRE (result.pack);
    return *result.pack;
}
} // namespace

TEST_CASE ("SFZ packs publish complete trees atomically and resolve portable locators", "[sfz][store]")
{
    StoreTree tree;
    PackStore original (tree.root / "original");
    const auto staging = stage (original);
    REQUIRE (original.installed().empty());
    auto pack = provider();
    pack.presentation.artworkUrl = "https://fixtures.example/piano.jpg";
    pack.presentation.categoryHint = "piano";
    const auto result = original.publish (pack, pack.expectedSha256, staging, { u8"Programs/Étude.sfz" });
    REQUIRE (result);
    REQUIRE (result.pack);
    REQUIRE_FALSE (fs::exists (staging));
    REQUIRE (original.installed().size() == 1);
    CHECK (original.installed().front().provider.presentation.artworkUrl == pack.presentation.artworkUrl);
    CHECK (original.installed().front().provider.presentation.categoryHint == "piano");
    const auto locator = result.pack->locatorFor (result.pack->instruments.front());
    const auto parsed = locatorFromJson (locatorToJson (locator));
    REQUIRE (parsed);
    REQUIRE (*parsed == locator);
    REQUIRE (original.resolve (locator));
    REQUIRE (original.locatorForFile (*original.resolve (locator)) == locator);
    REQUIRE_FALSE (original.locatorForFile (result.pack->directory / "Samples/tone.wav"));

    const auto relocatedRoot = tree.root / "relocated";
    std::error_code error;
    fs::copy (original.root(), relocatedRoot, fs::copy_options::recursive, error);
    REQUIRE_FALSE (error);
    PackStore relocated (relocatedRoot);
    std::string message;
    REQUIRE (relocated.initialize (message));
    const auto path = relocated.resolve (locator);
    REQUIRE (path);
    REQUIRE (path->parent_path().parent_path().parent_path().parent_path() == relocated.root());
    REQUIRE (dusk::fs::loadFileAsString (*path) == "<region> sample=../Samples/tone.wav");
    REQUIRE (relocated.locatorForFile (*path) == locator);
}

TEST_CASE ("SFZ pack versions coexist without changing an older session locator", "[sfz][store]")
{
    StoreTree tree;
    PackStore store (tree.root);
    const auto first = publish (store, provider ('a'));
    const auto second = publish (store, provider ('b'));
    REQUIRE (store.installed().size() == 2);
    REQUIRE (first.directory != second.directory);
    REQUIRE (store.resolve (first.locatorFor (first.instruments.front())));
    REQUIRE (store.resolve (second.locatorFor (second.instruments.front())));
    auto unknown = first.locatorFor (first.instruments.front());
    unknown.releaseId = std::string (64, 'c');
    REQUIRE_FALSE (store.resolve (unknown));
    unknown = first.locatorFor (first.instruments.front());
    unknown.instrumentId = "unrelated";
    REQUIRE_FALSE (store.resolve (unknown));
}

TEST_CASE ("SFZ pack publication rejects incomplete or ambiguous extracted entrypoints", "[sfz][store]")
{
    StoreTree tree;
    PackStore store (tree.root);
    auto pack = provider();
    auto staging = stage (store);
    std::vector<std::string> entries { u8"Programs/Étude.sfz" };
    std::string archiveDigest = pack.expectedSha256;
    SECTION ("Missing entrypoint") { entries = { "missing.sfz" }; }
    SECTION ("No instruments") { entries.clear(); }
    SECTION ("Duplicate path") { entries.push_back (entries.front()); }
    SECTION ("Non SFZ file") { entries = { "Samples/tone.wav" }; }
    SECTION ("Outside staging") { staging = tree.root; }
    SECTION ("Traversal") { entries = { "../Programs/piano.sfz" }; }
    SECTION ("Reserved receipt")
    { REQUIRE (dusk::fs::writeStringToFile (staging / ".dusk-pack.json", "{}")); }
    SECTION ("Invalid archive hash") { archiveDigest = "not a digest"; }
    SECTION ("Wrong upstream archive hash") { archiveDigest = std::string (64, 'b'); }
    SECTION ("Unverified license") { pack.license.text = "Free download"; }
    const auto result = store.publish (pack, archiveDigest, staging, entries);
    REQUIRE_FALSE (result);
    REQUIRE_FALSE (result.error.empty());
    REQUIRE (store.installed().empty());
}

TEST_CASE ("SFZ malformed receipts stay unavailable to offline session restoration", "[sfz][store]")
{
    StoreTree tree;
    PackStore store (tree.root);
    const auto installed = publish (store, provider());
    const auto locator = installed.locatorFor (installed.instruments.front());
    const auto path = installed.directory / ".dusk-pack.json";
    auto receipt = Json::parse (dusk::fs::loadFileAsString (path));
    bool truncated = false, missing = false;
    SECTION ("Truncated receipt") { truncated = true; }
    SECTION ("Missing receipt") { missing = true; }
    SECTION ("Unknown schema") { receipt["schema"] = 2; }
    SECTION ("Changed archive identity") { receipt["archive_sha256"] = std::string (64, 'b'); }
    SECTION ("Missing entrypoint") { receipt["instruments"][0]["path"] = "missing.sfz"; }
    SECTION ("Traversal") { receipt["instruments"][0]["path"] = "../outside.sfz"; }
    SECTION ("Non SFZ file") { receipt["instruments"][0]["path"] = "Samples/tone.wav"; }
    SECTION ("Duplicate entrypoint") { receipt["instruments"].push_back (receipt["instruments"][0]); }
    SECTION ("Duplicate identifier")
    {
        REQUIRE (dusk::fs::writeStringToFile (installed.directory / "other.sfz", "<region>"));
        auto entry = receipt["instruments"][0];
        entry["path"] = "other.sfz";
        receipt["instruments"].push_back (std::move (entry));
    }
    SECTION ("Invalid license") { receipt["provider"]["license"]["text"] = "unlicensed"; }
    REQUIRE (dusk::fs::writeStringToFile (path, truncated ? "{\"schema\":" : receipt.dump()));
    if (missing) REQUIRE (fs::remove (path));
    REQUIRE (store.installed().empty());
    REQUIRE_FALSE (store.resolve (locator));

    const auto repaired = store.publish (installed.provider, installed.archiveSha256, stage (store),
                                         { u8"Programs/Étude.sfz" });
    REQUIRE (repaired);
    REQUIRE (repaired.pack);
    REQUIRE (repaired.pack->locatorFor (repaired.pack->instruments.front()) == locator);
    REQUIRE (store.resolve (locator));
    REQUIRE (store.installed().size() == 1);
    REQUIRE_FALSE (fs::exists (installed.directory / "other.sfz"));
}

TEST_CASE ("SFZ removal refuses any instrument from a pack used by the current session", "[sfz][store]")
{
    StoreTree tree;
    PackStore store (tree.root);
    const auto installed = publish (store, provider());
    const auto locator = installed.locatorFor (installed.instruments.front());
    auto otherInstrument = locator;
    otherInstrument.instrumentId = "another-program";
    otherInstrument.relativePath = "another.sfz";
    const auto refused = store.remove (installed, { otherInstrument });
    REQUIRE_FALSE (refused);
    REQUIRE (store.resolve (locator));
    const auto removed = store.remove (installed, {});
    REQUIRE (removed);
    REQUIRE_FALSE (fs::exists (installed.directory));
    REQUIRE_FALSE (store.resolve (locator));
}

TEST_CASE ("SFZ publication reuses the identical version without exposing leftover staging", "[sfz][store]")
{
    StoreTree tree;
    PackStore store (tree.root);
    const auto installed = publish (store, provider());
    const auto staging = stage (store);
    const auto result = store.publish (installed.provider, installed.archiveSha256, staging,
                                       { u8"Programs/Étude.sfz" });
    REQUIRE (result);
    REQUIRE (result.pack);
    REQUIRE (result.pack->directory == installed.directory);
    REQUIRE (store.installed().size() == 1);
    REQUIRE_FALSE (fs::exists (staging));
}
