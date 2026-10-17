#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzPackStore.h"
#include "engine/sfz/SfzPackIntegrity.h"
#include "foundation/Fs.h"

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

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
    ~StoreTree() { std::error_code ignored; fs::remove_all (dusk::fs::nativeIOPath (root), ignored); }
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

ProviderPack pinnedProvider()
{
    auto pack = provider();
    pack.sourceId = "sfzinstruments";
    pack.releaseId = std::string (40, 'a');
    pack.expectedSha256.clear();
    pack.expectedRoot = "Piano-" + pack.releaseId;
    const std::string base = "https://github.com/sfzinstruments/Piano";
    pack.sourceUrl = base + "/tree/" + pack.releaseId;
    pack.downloadUrl = base + "/archive/" + pack.releaseId + ".zip";
    pack.license.path = "README.md";
    pack.license.sourceUrl = base + "/blob/" + pack.releaseId + "/README.md";
    return pack;
}

fs::path stage (PackStore& store)
{
    std::string error;
    const auto staging = store.createStaging (error);
    REQUIRE_FALSE (staging.empty());
    REQUIRE (error.empty());
    REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (staging / "Programs")));
    REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (staging / "Samples")));
    REQUIRE (dusk::fs::writeStringToFile (staging / fs::u8path (u8"Programs/Étude.sfz"),
                                          "<region> sample=../Samples/tone.wav"));
    REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/tone.wav", "fixture sample"));
    REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/LICENSE", "Samples: " + provider().license.text));
    REQUIRE (fs::create_directories (dusk::fs::nativeIOPath (staging / "LICENSES/nested")));
    REQUIRE (dusk::fs::writeStringToFile (staging / "LICENSES/nested/audio.txt", provider().license.text));
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
    auto root = tree.root / "original";
    SECTION ("Deep library root")
    {
        for (int index = 0; index < 4; ++index) root /= std::string (72, char ('a' + index));
    }
    PackStore original (root);
    const auto staging = stage (original);
    REQUIRE (original.installed().empty());
    auto pack = provider();
    SECTION ("Staging ownership survives store moves until the last owner is released")
    {
        std::string error;
        const auto root = original.root();
        DirectoryLease maintenance;
        CHECK_FALSE (maintenance.acquire (root, DirectoryLease::Access::Exclusive, error));
        CHECK (error.empty());
        PackStore moved (std::move (original));
        REQUIRE (moved.initialize (error));
        CHECK_FALSE (maintenance.acquire (root, DirectoryLease::Access::Exclusive, error));
        CHECK (error.empty());
        original = std::move (moved);
        {
            PackStore additional (tree.root / "additional");
            REQUIRE (additional.initialize (error));
            CHECK_FALSE (maintenance.acquire (additional.root(), DirectoryLease::Access::Exclusive, error));
            CHECK (error.empty());
        }
        REQUIRE (maintenance.acquire (tree.root / "additional", DirectoryLease::Access::Exclusive, error));
    }
   #if ! defined(_WIN32)
    SECTION ("Replacing a root cannot leave its new staging directory unprotected")
    {
        const auto root = tree.root / "replaced-owner";
        PackStore replaced (root);
        std::string error;
        REQUIRE (replaced.initialize (error));
        fs::rename (root, tree.root / "old-owner");
        REQUIRE (fs::create_directories (root / "packs"));
        REQUIRE_FALSE (replaced.createStaging (error).empty());
        DirectoryLease maintenance;
        CHECK_FALSE (maintenance.acquire (root, DirectoryLease::Access::Exclusive, error));
        CHECK (error.empty());
    }
   #endif
    SECTION ("Pinned primary license is kept byte for byte")
    {
        pack = pinnedProvider();
        REQUIRE (dusk::fs::writeStringToFile (staging / "README.md", pack.license.text));
    }
    SECTION ("Initialization reports native failures and clears stale errors")
    {
        const auto blocked = tree.root / "blocked";
        REQUIRE (dusk::fs::writeStringToFile (blocked, "keep this"));
        PackStore unavailable (blocked);
        std::string error;
        std::error_code nativeError;
        CHECK_FALSE (unavailable.initialize (error, &nativeError));
        CHECK (nativeError);
        CHECK (nativeError != std::errc::no_space_on_device);
        CHECK_FALSE (error.empty());
        REQUIRE (original.initialize (error, &nativeError));
        CHECK_FALSE (nativeError);
        CHECK (error.empty());
        nativeError = std::make_error_code (std::errc::io_error);
        const auto extraStaging = original.createStaging (error, &nativeError);
        REQUIRE_FALSE (extraStaging.empty());
        CHECK_FALSE (nativeError);
        CHECK (error.empty());
        CHECK (dusk::fs::loadFileAsString (blocked) == "keep this");
    }
    pack.presentation.artworkUrl = "https://fixtures.example/piano.jpg";
    pack.presentation.categoryHint = "piano";
    SECTION ("Publication interruption leaves no receipt or installed version")
    {
        bool interrupted = false;
        OperationControl control;
        SECTION ("Cancelled before initialization") { control.cancelled = [] { return true; }; }
        SECTION ("Paused while hashing")
        {
            control.paused = [&] { return interrupted; };
            control.progress = [&] (const OperationProgress&) { interrupted = true; };
        }
        const auto stopped = original.publish (pack, std::string (64, 'a'), staging,
                                               { u8"Programs/Étude.sfz" }, control);
        CHECK_FALSE (stopped);
        CHECK (stopped.status == (interrupted ? OperationStatus::Paused : OperationStatus::Cancelled));
        CHECK_FALSE (fs::exists (dusk::fs::nativeIOPath (staging / ".dusk-pack.json")));
        CHECK (original.installed().empty());
    }
    const auto result = original.publish (pack, std::string (64, 'a'), staging, { u8"Programs/Étude.sfz" });
    REQUIRE (result);
    CHECK (result.failureKind == FailureKind::None);
    REQUIRE (result.pack);
    REQUIRE_FALSE (fs::exists (dusk::fs::nativeIOPath (staging)));
    REQUIRE (original.installed().size() == 1);
    CHECK (original.installed().front().provider.presentation.artworkUrl == pack.presentation.artworkUrl);
    CHECK (original.installed().front().provider.presentation.categoryHint == "piano");
    REQUIRE (isSha256 (result.pack->installedTreeSha256));
    CHECK (original.installed().front().installedTreeSha256 == result.pack->installedTreeSha256);
    REQUIRE (original.verify (*result.pack));
    const auto receiptPath = result.pack->directory / ".dusk-pack.json";
    const auto receiptBytes = dusk::fs::loadFileAsString (receiptPath);
    SECTION ("Verification reads the current receipt rather than the copied digest")
    {
        auto selected = *result.pack;
        selected.installedTreeSha256 = std::string (64, 'b');
        CHECK (original.verify (selected));
    }
    SECTION ("Verification detects changed and missing installed files")
    {
        auto path = result.pack->directory / "Samples/tone.wav";
        SECTION ("Equal length sample corruption")
        {
            REQUIRE (dusk::fs::writeStringToFile (path, "corrupt sample"));
        }
        SECTION ("Missing sample") { REQUIRE (fs::remove (dusk::fs::nativeIOPath (path))); }
        SECTION ("Missing SFZ")
        {
            path = result.pack->directory / fs::u8path (u8"Programs/Étude.sfz");
            REQUIRE (fs::remove (dusk::fs::nativeIOPath (path)));
        }
        const auto checked = original.verify (*result.pack);
        CHECK_FALSE (checked);
        CHECK (checked.failureKind == FailureKind::Integrity);
        CHECK (dusk::fs::loadFileAsString (receiptPath) == receiptBytes);
        return;
    }
    SECTION ("Verification refuses a receipt changed during hashing")
    {
        OperationControl control;
        control.progress = [&] (const OperationProgress&)
        { REQUIRE (dusk::fs::writeStringToFile (receiptPath, receiptBytes + "\n")); };
        const auto checked = original.verify (*result.pack, control);
        CHECK_FALSE (checked);
        CHECK (checked.failureKind == FailureKind::Integrity);
        return;
    }
    SECTION ("Verification can pause or cancel without modifying installed data")
    {
        OperationControl control;
        bool interrupted = false;
        SECTION ("Pause") { control.paused = [&] { return interrupted; }; }
        SECTION ("Cancel") { control.cancelled = [&] { return interrupted; }; }
        control.progress = [&] (const OperationProgress&) { interrupted = true; };
        const auto checked = original.verify (*result.pack, control);
        CHECK_FALSE (checked);
        CHECK ((checked.status == OperationStatus::Paused || checked.status == OperationStatus::Cancelled));
        CHECK (dusk::fs::loadFileAsString (receiptPath) == receiptBytes);
        REQUIRE (original.verify (*result.pack));
    }
    SECTION ("Legacy receipts remain usable without inventing a baseline")
    {
        auto receipt = Json::parse (receiptBytes);
        receipt.erase ("installed_tree_sha256_v1");
        const auto legacyBytes = receipt.dump();
        REQUIRE (dusk::fs::writeStringToFile (receiptPath, legacyBytes));
        const auto installed = original.installed();
        REQUIRE (installed.size() == 1);
        CHECK (installed.front().installedTreeSha256.empty());
        const auto unavailable = original.verify (*result.pack);
        CHECK_FALSE (unavailable);
        CHECK (unavailable.failureKind == FailureKind::None);
        CHECK (unavailable.error.find ("unavailable") != std::string::npos);
        const auto reused = original.publish (pack, std::string (64, 'a'), stage (original),
                                              { u8"Programs/Étude.sfz" });
        REQUIRE (reused);
        REQUIRE (reused.pack);
        CHECK (reused.pack->installedTreeSha256.empty());
        CHECK (dusk::fs::loadFileAsString (receiptPath) == legacyBytes);
    }
    SECTION ("Verification rejects a pack from another library")
    {
        auto selected = *result.pack;
        selected.directory = tree.root / "elsewhere" / selected.directory.filename();
        CHECK_FALSE (original.verify (selected));
    }
    SECTION ("prior restricted installations remain usable without permitting a new publication")
    {
        std::string type;
        SECTION ("NonCommercial") { type = "by-nc"; }
        SECTION ("NoDerivatives") { type = "by-nd"; }
        SECTION ("NonCommercial ShareAlike") { type = "by-nc-sa"; }
        SECTION ("NonCommercial NoDerivatives") { type = "by-nc-nd"; }
        auto prior = pack;
        prior.license.id = "CC-" + type + "-4.0";
        for (auto& c : prior.license.id) if (c >= 'a' && c <= 'z') c = (char) (c + 'A' - 'a');
        prior.license.text = prior.license.id + " https://creativecommons.org/licenses/" + type + "/4.0/";
        prior.license.sha256 = hash (prior.license.text);
        const auto descriptor = providerPackToJson (prior);
        REQUIRE_FALSE (providerPackFromJson (descriptor));
        REQUIRE (providerPackFromJson (descriptor, PackDescriptorUse::InstalledReceipt));
        const auto receiptPath = result.pack->directory / ".dusk-pack.json";
        auto receipt = Json::parse (dusk::fs::loadFileAsString (receiptPath));
        receipt["provider"] = Json::parse (descriptor);
        REQUIRE (dusk::fs::writeStringToFile (receiptPath, receipt.dump()));
        const auto installed = original.installed();
        REQUIRE (installed.size() == 1);
        CHECK (installed.front().provider.license.id == prior.license.id);
        const auto locator = installed.front().locatorFor (installed.front().instruments.front());
        const auto path = original.resolve (locator);
        REQUIRE (path);
        CHECK (original.locatorForFile (*path) == locator);
        CHECK_FALSE (original.publish (prior, prior.expectedSha256, stage (original), { u8"Programs/Étude.sfz" }));
        CHECK (original.resolve (locator));
        CHECK_FALSE (original.remove (installed.front(), { locator }));
        CHECK (original.remove (installed.front(), {}));
        auto tampered = prior;
        tampered.license.sha256 = std::string (64, 'b');
        CHECK_FALSE (providerPackFromJson (providerPackToJson (tampered), PackDescriptorUse::InstalledReceipt));
        return;
    }
    SECTION ("portable relocation") {}
    const auto locator = result.pack->locatorFor (result.pack->instruments.front());
    const auto parsed = locatorFromJson (locatorToJson (locator));
    REQUIRE (parsed);
    REQUIRE (*parsed == locator);
    REQUIRE (original.resolve (locator));
    REQUIRE (original.locatorForFile (*original.resolve (locator)) == locator);
    REQUIRE_FALSE (original.locatorForFile (result.pack->directory / "Samples/tone.wav"));

    const auto relocatedRoot = tree.root / "relocated";
    std::error_code error;
    REQUIRE (fs::create_directory (relocatedRoot));
    fs::copy (dusk::fs::nativeIOPath (original.root() / "packs"),
              dusk::fs::nativeIOPath (relocatedRoot / "packs"), fs::copy_options::recursive, error);
    REQUIRE_FALSE (error);
    PackStore relocated (relocatedRoot);
    std::string message;
    REQUIRE (relocated.initialize (message));
    const auto path = relocated.resolve (locator);
    REQUIRE (path);
    REQUIRE (path->parent_path().parent_path().parent_path().parent_path() == relocated.root());
    REQUIRE (dusk::fs::loadFileAsString (*path) == "<region> sample=../Samples/tone.wav");
    REQUIRE (relocated.locatorForFile (*path) == locator);
    REQUIRE (original.remove (*result.pack, {}));
    CHECK_FALSE (original.resolve (locator));
    CHECK (relocated.resolve (locator));
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

TEST_CASE ("SFZ pack storage reports real filesystem capacity failures", "[sfz][store][sfz-store-disk-full]")
{
   #if defined(__linux__)
    const auto* rootSetting = std::getenv ("DUSK_SFZ_CAPACITY_ROOT");
    const auto* modeSetting = std::getenv ("DUSK_SFZ_CAPACITY_MODE");
    if (! rootSetting || ! modeSetting) SKIP ("Requires the private tmpfs fixture from sfz_disk_full.sh");
    const auto root = fs::canonical (rootSetting);
    const std::string mode = modeSetting;
    REQUIRE ((mode == "inodes" || mode == "bytes"));
    REQUIRE (fs::is_empty (root));
    REQUIRE (fs::space (root).capacity <= 64 * 1024);
    PackStore store (root);
    std::string error;
    std::error_code nativeError;
    if (mode == "inodes")
    {
        CHECK_FALSE (store.initialize (error, &nativeError));
        CHECK (nativeError == std::errc::no_space_on_device);
        CHECK (store.createStaging (error, &nativeError).empty());
        CHECK (nativeError == std::errc::no_space_on_device);
        const auto pack = provider();
        const auto result = store.publish (pack, pack.expectedSha256, root / ".install-absent", { u8"Programs/Étude.sfz" });
        CHECK (result.status == OperationStatus::Failed);
        CHECK (result.failureKind == FailureKind::DiskFull);
        CHECK (fs::is_empty (root));
    }
    else
    {
        const auto staging = stage (store);
        const auto filler = root / "filler";
        REQUIRE_FALSE (dusk::fs::writeStringToFile (filler, std::string (64 * 1024, 'f'), &nativeError));
        REQUIRE (nativeError == std::errc::no_space_on_device);
        const auto pack = provider();
        const auto failed = store.publish (pack, pack.expectedSha256, staging, { u8"Programs/Étude.sfz" });
        CHECK (failed.status == OperationStatus::Failed);
        CHECK (failed.failureKind == FailureKind::DiskFull);
        CHECK (failed.error == "Cannot write SFZ installation receipt");
        CHECK_FALSE (failed.pack);
        CHECK (fs::is_empty (root / "packs"));
        CHECK (dusk::fs::loadFileAsString (staging / "Samples/tone.wav") == "fixture sample");
        REQUIRE (fs::remove (filler));
        REQUIRE (fs::remove_all (staging) > 0);
        const auto retried = store.publish (pack, pack.expectedSha256, stage (store), { u8"Programs/Étude.sfz" });
        REQUIRE (retried);
        CHECK (retried.failureKind == FailureKind::None);
        REQUIRE (retried.pack);
        CHECK (store.resolve (retried.pack->locatorFor (retried.pack->instruments.front())));
    }
   #else
    SKIP ("The bounded tmpfs fixture requires Linux user and mount namespaces");
   #endif
}

TEST_CASE ("SFZ pack publication rejects incomplete or ambiguous extracted entrypoints", "[sfz][store]")
{
    StoreTree tree;
    PackStore store (tree.root);
    auto pack = provider();
    auto staging = stage (store);
    std::vector<std::string> entries { u8"Programs/Étude.sfz" };
    std::string archiveDigest = pack.expectedSha256;
    auto failureKind = FailureKind::None;
    SECTION ("Missing entrypoint") { entries = { "missing.sfz" }; }
    SECTION ("No instruments") { entries.clear(); }
    SECTION ("Duplicate path") { entries.push_back (entries.front()); }
    SECTION ("Non SFZ file") { entries = { "Samples/tone.wav" }; }
    SECTION ("Outside staging") { staging = tree.root; }
    SECTION ("Traversal") { entries = { "../Programs/piano.sfz" }; }
    SECTION ("Reserved receipt")
    { REQUIRE (dusk::fs::writeStringToFile (staging / ".dusk-pack.json", "{}")); }
    SECTION ("Invalid archive hash") { archiveDigest = "not a digest"; }
    SECTION ("Wrong upstream archive hash")
    { archiveDigest = std::string (64, 'b'); failureKind = FailureKind::Integrity; }
    SECTION ("Unverified license") { pack.license.text = "Free download"; }
    SECTION ("Restricted sample license")
    { REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/LICENSE.txt", "https://creativecommons.org/licenses/by-nc/4.0/")); }
    SECTION ("Restricted sidecar license")
    { REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/tone.wav.license", "https://creativecommons.org/licenses/by-nd/4.0/")); }
    SECTION ("Conflicting allowed license")
    { REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/COPYING", "https://creativecommons.org/licenses/by/4.0/")); }
    SECTION ("Unknown license in a licensing directory")
    {
        REQUIRE (dusk::fs::writeStringToFile (staging / "LICENSES/audio.txt", "Free to download."));
    }
    SECTION ("Portable license aliases")
    {
        REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/LICENSE", pack.license.text));
        REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/license", pack.license.text));
        if (fs::equivalent (staging / "Samples/LICENSE", staging / "Samples/license"))
            SKIP ("case-distinct filenames are unavailable");
    }
    SECTION ("Licensing file is a symbolic link")
    {
        std::error_code error;
        fs::create_symlink ("Samples/tone.wav", staging / "LICENSE", error);
        if (error) SKIP ("symlinks are unavailable");
    }
    SECTION ("Oversized license document")
    { REQUIRE (dusk::fs::writeStringToFile (staging / "LICENSE", std::string (kMaxLicenseBytes + 1, 'x'))); }
    SECTION ("Too many license documents")
    {
        for (std::size_t i = 0; i <= kMaxLicenseDocuments; ++i)
            REQUIRE (dusk::fs::writeStringToFile (staging / ("LICENSE-" + std::to_string (i)), pack.license.text));
    }
    SECTION ("Combined licensing text is too large")
    {
        const auto text = pack.license.text + std::string (kMaxLicenseBytes - pack.license.text.size(), ' ');
        for (int i = 0; i < 11; ++i)
            REQUIRE (dusk::fs::writeStringToFile (staging / ("LICENSE-" + std::to_string (i)), text));
    }
    SECTION ("Missing pinned primary license")
    {
        pack = pinnedProvider();
        REQUIRE (providerPackFromJson (providerPackToJson (pack)));
        SECTION ("Absent") {}
        SECTION ("Changed bytes")
        { REQUIRE (dusk::fs::writeStringToFile (staging / "README.md", pack.license.text + "changed")); }
    }
    const bool receiptExisted = fs::exists (staging / ".dusk-pack.json");
    const auto result = store.publish (pack, archiveDigest, staging, entries);
    REQUIRE_FALSE (result);
    REQUIRE_FALSE (result.error.empty());
    CHECK (result.failureKind == failureKind);
    REQUIRE (store.installed().empty());
    REQUIRE (fs::exists (staging / ".dusk-pack.json") == receiptExisted);
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
    SECTION ("Invalid installed tree digest") { receipt["installed_tree_sha256_v1"] = "invalid"; }
    SECTION ("Empty installed tree digest") { receipt["installed_tree_sha256_v1"] = ""; }
    SECTION ("Non-string installed tree digest") { receipt["installed_tree_sha256_v1"] = 1; }
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

TEST_CASE ("SFZ installed tree digests include paths and contents without receipt metadata", "[sfz][store]")
{
    StoreTree tree;
    REQUIRE (fs::create_directory (tree.root / "Samples"));
    const std::string entrypoint = "<region> sample=Samples/tone.wav";
    REQUIRE (dusk::fs::writeStringToFile (tree.root / "Piano.sfz", entrypoint));
    REQUIRE (dusk::fs::writeStringToFile (tree.root / "Samples/tone.wav", "original samples"));
    REQUIRE (dusk::fs::writeStringToFile (tree.root / fs::u8path (u8"Samples/Étude.wav"), "unicode samples"));
    const auto baseline = hashPackContents (tree.root);
    REQUIRE (baseline);
    REQUIRE (isSha256 (baseline.sha256));
    bool unchanged = false;
    SECTION ("Receipt metadata is excluded")
    {
        REQUIRE (dusk::fs::writeStringToFile (tree.root / ".dusk-pack.json", "receipt metadata"));
        unchanged = true;
    }
    SECTION ("Canonical Unicode filename spellings have the same digest")
    {
        fs::rename (tree.root / fs::u8path (u8"Samples/Étude.wav"),
                    tree.root / fs::u8path (u8"Samples/E\u0301tude.wav"));
        unchanged = true;
    }
   #if defined(_WIN32)
    SECTION ("A restored last-write time cannot hide an edit to an already hashed file")
    {
        const auto first = tree.root / "Piano.sfz";
        const auto modified = fs::last_write_time (first);
        bool edited = false;
        OperationControl control;
        control.progress = [&] (const OperationProgress& progress)
        {
            if (edited || progress.completedBytes <= entrypoint.size()) return;
            REQUIRE (dusk::fs::writeStringToFile (first, std::string (entrypoint.size(), 'x')));
            fs::last_write_time (first, modified);
            edited = true;
        };
        const auto result = hashPackContents (tree.root, control);
        REQUIRE (edited);
        CHECK (result.status == OperationStatus::Failed);
        CHECK (result.failureKind == FailureKind::Integrity);
        CHECK (result.sha256.empty());
        return;
    }
   #endif
    SECTION ("The absolute library path is excluded")
    {
        StoreTree copy;
        fs::copy (tree.root, copy.root, fs::copy_options::recursive);
        REQUIRE (hashPackContents (copy.root).sha256 == baseline.sha256);
        unchanged = true;
    }
    SECTION ("Equal-length sample corruption")
    { REQUIRE (dusk::fs::writeStringToFile (tree.root / "Samples/tone.wav", "modified samples")); }
    SECTION ("A missing sample") { REQUIRE (fs::remove (tree.root / "Samples/tone.wav")); }
    SECTION ("An added file") { REQUIRE (dusk::fs::writeStringToFile (tree.root / "extra.txt", "extra")); }
    SECTION ("A renamed sample") { fs::rename (tree.root / "Samples/tone.wav", tree.root / "Samples/other.wav"); }
    SECTION ("An empty directory") { REQUIRE (fs::create_directory (tree.root / "Empty")); }
    const auto current = hashPackContents (tree.root);
    REQUIRE (current);
    CHECK ((current.sha256 == baseline.sha256) == unchanged);
}

TEST_CASE ("SFZ installed tree hashing bounds and refuses unsafe entries", "[sfz][store]")
{
    StoreTree tree;
    REQUIRE (fs::create_directory (tree.root / "Samples"));
    REQUIRE (dusk::fs::writeStringToFile (tree.root / "Piano.sfz", "<region>"));
    REQUIRE (dusk::fs::writeStringToFile (tree.root / "Samples/tone.wav", "samples"));
    ArchiveLimits limits;
    SECTION ("Entry count") { limits.maxFiles = 2; }
    SECTION ("Combined bytes") { limits.maxExpandedBytes = 4; }
    SECTION ("Path depth") { limits.paths.maxDepth = 1; }
    SECTION ("Path length") { limits.paths.maxBytes = 8; }
    SECTION ("Symbolic file")
    {
        std::error_code error;
        fs::create_symlink (tree.root / "Piano.sfz", tree.root / "alias.sfz", error);
        if (error) SKIP ("The filesystem cannot create the symbolic-file fixture");
    }
    SECTION ("Hard-linked file")
    {
        std::error_code error;
        fs::create_hard_link (tree.root / "Piano.sfz", tree.root / "alias.sfz", error);
        if (error) SKIP ("The filesystem cannot create the hard-link fixture");
    }
    SECTION ("Symbolic directory")
    {
        std::error_code error;
        fs::create_directory_symlink (tree.root / "Samples", tree.root / "alias", error);
        if (error) SKIP ("The filesystem cannot create the symbolic-directory fixture");
    }
    SECTION ("Portable aliases")
    {
        REQUIRE (dusk::fs::writeStringToFile (tree.root / "PIANO.sfz", "<region>"));
        if (fs::equivalent (tree.root / "Piano.sfz", tree.root / "PIANO.sfz"))
            SKIP ("The filesystem cannot create case-distinct names");
    }
    const auto result = hashPackContents (tree.root, {}, limits);
    CHECK (result.status == OperationStatus::Failed);
    CHECK (result.failureKind == FailureKind::Integrity);
    CHECK (result.sha256.empty());
}

TEST_CASE ("SFZ installed tree hashing yields to pause and cancellation", "[sfz][store]")
{
    StoreTree tree;
    const std::string bytes (3 * 65536, 'a');
    REQUIRE (dusk::fs::writeStringToFile (tree.root / "Piano.sfz", bytes));
    bool interrupted = false;
    auto expected = OperationStatus::Cancelled;
    OperationControl control;
    SECTION ("Cancellation") { control.cancelled = [&] { return interrupted; }; }
    SECTION ("Pause") { expected = OperationStatus::Paused; control.paused = [&] { return interrupted; }; }
    SECTION ("A short pause cannot turn into an empty successful digest")
    {
        int polls = 0;
        control.paused = [&] { return ++polls == 1; };
        const auto result = hashPackContents (tree.root, control);
        CHECK (result.status == OperationStatus::Paused);
        CHECK (result.sha256.empty());
        return;
    }
    std::uint64_t completed = 0;
    control.progress = [&] (const OperationProgress& progress)
    { completed = progress.completedBytes; interrupted = true; };
    const auto result = hashPackContents (tree.root, control);
    CHECK (result.status == expected);
    CHECK (result.failureKind == FailureKind::None);
    CHECK (result.sha256.empty());
    CHECK (completed > 0);
    CHECK (completed <= 65536);
    CHECK (dusk::fs::loadFileAsString (tree.root / "Piano.sfz") == bytes);
}
