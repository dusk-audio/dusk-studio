#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/sfz/SfzLibraryLocations.h"
#include "foundation/AppConfigDir.h"
#include "foundation/Fs.h"

#include <sodium.h>

#include <array>
#include <filesystem>
#include <string>

namespace
{
using namespace duskstudio::sfz;
namespace fs = std::filesystem;

std::string hash (const std::string& text)
{
    REQUIRE (sodium_init() >= 0);
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (text.data()), text.size());
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

struct LibraryTree
{
    LibraryTree() : root (fs::canonical (dusk::fs::createUniqueTempDirectory ("dusk-sfz-location-")))
    {
        REQUIRE (fs::create_directory (source));
        REQUIRE (fs::create_directory (destination));
        const auto configured = changeLibraryLocation ({}, source, false, {}, configuration);
        REQUIRE (configured);
        REQUIRE (configured.locations);
        before = *configured.locations;
        PackStore store (source, before.active.marker);
        std::string error;
        const auto staging = store.createStaging (error);
        REQUIRE_FALSE (staging.empty());
        REQUIRE (fs::create_directory (staging / "Programs"));
        REQUIRE (fs::create_directory (staging / "Samples"));
        REQUIRE (dusk::fs::writeStringToFile (staging / fs::u8path (u8"Programs/Étude.sfz"), "<region> sample=../Samples/tone.wav"));
        sample.assign (150000, 'a');
        REQUIRE (dusk::fs::writeStringToFile (staging / "Samples/tone.wav", sample));
        ProviderPack provider;
        provider.sourceId = "musical-artifacts";
        provider.packId = "42";
        provider.releaseId = provider.expectedSha256 = std::string (64, 'a');
        provider.displayName = "Portable piano";
        provider.sourceUrl = "https://musical-artifacts.com/artifacts/42";
        provider.downloadUrl = provider.sourceUrl + "/piano.zip";
        provider.versionPinned = true;
        const std::string license = "CC0-1.0 https://creativecommons.org/publicdomain/zero/1.0/";
        provider.license = { "CC0-1.0", license, {}, hash (license), provider.sourceUrl, {} };
        provider.instruments.push_back ({ "piano", "Piano", u8"Programs/Étude.sfz", false });
        const auto published = store.publish (provider, provider.expectedSha256, staging, { u8"Programs/Étude.sfz" });
        REQUIRE (published);
        REQUIRE (published.pack);
        pack = *published.pack;
    }
    ~LibraryTree() { std::error_code ignored; fs::remove_all (root, ignored); }
    fs::path root;
    fs::path source = root / "source";
    fs::path destination = root / fs::u8path (u8"External library Étude");
    fs::path configuration = root / "locations.json";
    LibraryLocations before;
    InstalledPack pack;
    std::string sample;
};
} // namespace

TEST_CASE ("SFZ library defaults to application data while location settings stay in config", "[sfz][storage]")
{
    const auto root = dusk::fs::createUniqueTempDirectory ("dusk-sfz-default-location-");
    REQUIRE_FALSE (root.empty());
    const auto locations = readLibraryLocations (root / "absent.json");
    std::error_code ignored;
    fs::remove_all (root, ignored);
    REQUIRE (locations.error.empty());
    REQUIRE (locations.active.root == dusk::fs::appDataDir() / "sfz-library");
    REQUIRE (libraryLocationsFile() == dusk::fs::appConfigDir() / "sfz-library-locations.json");
}

TEST_CASE ("SFZ library locations preserve identities across copies and future-download folders", "[sfz][storage]")
{
    LibraryTree tree;
    const auto locator = tree.pack.locatorFor (tree.pack.instruments.front());
    bool move = true;
    bool chained = false, incomplete = false;
    SECTION ("Moving verifies samples and preserves portable identities") {}
    SECTION ("Future downloads retain the previous library for existing sessions") { move = false; }
    SECTION ("Choosing the same canonical folder preserves every pack") { tree.destination = tree.source / "."; }
    SECTION ("A directory alias never deletes its own installed packs")
    {
        const auto alias = tree.root / "source alias";
        std::error_code error;
        fs::create_directory_symlink (tree.source, alias, error);
        if (error) SKIP ("directory aliases are unavailable");
        tree.destination = alias;
    }
    SECTION ("A chained move does not require the disconnected emptied drive")
    {
        chained = true;
        REQUIRE (fs::create_directory (tree.source / ".downloads"));
        REQUIRE (fs::create_directory (tree.source / ".repository-cache"));
        REQUIRE (dusk::fs::writeStringToFile (tree.source / ".repository-cache/search.json", "cached listing"));
        REQUIRE (dusk::fs::writeStringToFile (tree.source / "unrelated.txt", "keep this file"));
    }
    SECTION ("An incomplete pack keeps its old root registered")
    {
        incomplete = true;
        REQUIRE (fs::create_directory (tree.source / "packs/incomplete"));
        REQUIRE (dusk::fs::writeStringToFile (tree.source / "packs/incomplete/sample.wav", "user-owned sample"));
    }
    const bool sameDirectory = fs::equivalent (tree.source, tree.destination);
    auto changed = changeLibraryLocation (tree.before, tree.destination, move, {}, tree.configuration);
    REQUIRE (changed);
    REQUIRE (changed.locations);
    if (chained)
    {
        REQUIRE (changed.locations->previous.empty());
        CHECK (dusk::fs::loadFileAsString (tree.source / "unrelated.txt") == "keep this file");
        fs::rename (tree.source, tree.root / "disconnected old drive");
        tree.destination = tree.root / "third library";
        REQUIRE (fs::create_directory (tree.destination));
        changed = changeLibraryLocation (*changed.locations, tree.destination, true, {}, tree.configuration);
        REQUIRE (changed);
        REQUIRE (changed.locations);
        CHECK (changed.locations->previous.empty());
    }
    const auto restored = readLibraryLocations (tree.configuration);
    REQUIRE (restored.error.empty());
    CHECK (restored.active.root == fs::canonical (tree.destination));
    REQUIRE (libraryLocationAvailable (restored.active));
    const auto installed = installedLibraryPacks (restored);
    REQUIRE (installed.size() == 1);
    CHECK (installed.front().locatorFor (installed.front().instruments.front()) == locator);
    const auto path = resolveLibraryInstrument (restored, locator);
    REQUIRE (path);
    CHECK (libraryLocatorForFile (restored, *path) == locator);
    CHECK (dusk::fs::loadFileAsString (installed.front().directory / "Samples/tone.wav") == tree.sample);
    if (! sameDirectory)
        CHECK (fs::exists (tree.pack.directory) == ! move);
    if (move) CHECK (installed.front().directory.parent_path().parent_path() == fs::canonical (tree.destination));
    if (incomplete)
    {
        REQUIRE (restored.previous.size() == 1);
        CHECK (restored.previous.front().root == tree.source);
        CHECK (dusk::fs::loadFileAsString (tree.source / "packs/incomplete/sample.wav") == "user-owned sample");
    }
}

TEST_CASE ("SFZ library relocation failures retain the active configuration and every original", "[sfz][storage]")
{
    LibraryTree tree;
    const auto configBefore = dusk::fs::loadFileAsString (tree.configuration);
    auto file = tree.configuration;
    OperationControl control;
    bool cancelled = false;
    bool removedDestination = false, replacedDestination = false;
    fs::path replacedStaging, removedStaging;
   #if defined(_WIN32)
    const bool betweenFiles = true;
   #else
    const bool betweenFiles = GENERATE (false, true);
   #endif
    const auto stagingDirectory = [&]
    {
        std::error_code error;
        for (fs::directory_iterator it (tree.destination, error), end; ! error && it != end; it.increment (error))
            if (it->path().filename().u8string().rfind (".install-", 0) == 0) return it->path();
        return fs::path {};
    };
    const auto atTransferCheckpoint = [&] (auto action)
    {
        // Windows cannot rename the simulated drive while a copied file is open.
        if (betweenFiles)
            control.cancelled = [&, action]
            {
                if (! stagingDirectory().empty()) action();
                return false;
            };
        else control.progress = [action] (const auto&) { action(); };
    };
    SECTION ("Cancellation during copying removes destination-owned staging")
    {
        control.cancelled = [&] { return cancelled; };
        control.progress = [&] (const auto& progress) { cancelled = progress.completedBytes > 0; };
    }
    SECTION ("A failed configuration commit rolls back published copies")
    {
        file = tree.root / "blocked";
        REQUIRE (fs::create_directory (file));
    }
    SECTION ("A conflicting destination version is never overwritten")
    {
        const auto prepared = changeLibraryLocation ({}, tree.destination, false, {}, {});
        REQUIRE (prepared);
        const auto copy = tree.destination / "packs" / tree.pack.directory.filename();
        fs::copy (tree.pack.directory, copy, fs::copy_options::recursive);
        REQUIRE (dusk::fs::writeStringToFile (copy / "Samples/tone.wav", "different samples"));
    }
    SECTION ("A destination nested inside the source is rejected")
    {
        tree.destination = tree.source / "nested";
        REQUIRE (fs::create_directory (tree.destination));
    }
    SECTION ("A source symlink cannot redirect a library move")
    {
        std::error_code error;
        fs::create_symlink (tree.configuration, tree.pack.directory / "linked.json", error);
        if (error) SKIP ("symlink creation is unavailable");
    }
    SECTION ("A destination disconnected during transfer is never recreated")
    {
        atTransferCheckpoint ([&]
        {
            if (removedDestination) return;
            fs::rename (tree.destination, tree.root / "disconnected destination");
            removedDestination = true;
        });
    }
    SECTION ("A replacement mount with a copied marker is rejected")
    {
        atTransferCheckpoint ([&]
        {
            if (replacedDestination) return;
            const auto offline = tree.root / "disconnected destination";
            fs::rename (tree.destination, offline);
            REQUIRE (fs::create_directory (tree.destination));
            REQUIRE (fs::create_directory (tree.destination / "packs"));
            REQUIRE (fs::copy_file (offline / ".dusk-sfz-library", tree.destination / ".dusk-sfz-library"));
            replacedDestination = true;
        });
    }
    SECTION ("A missing staging directory is never recreated")
    {
        atTransferCheckpoint ([&]
        {
            if (! removedStaging.empty()) return;
            removedStaging = stagingDirectory();
            REQUIRE_FALSE (removedStaging.empty());
            fs::rename (removedStaging, tree.root / "disconnected staging");
        });
    }
    SECTION ("A replacement staging directory is retained without writes")
    {
        atTransferCheckpoint ([&]
        {
            if (! replacedStaging.empty()) return;
            replacedStaging = stagingDirectory();
            REQUIRE_FALSE (replacedStaging.empty());
            fs::rename (replacedStaging, tree.root / "disconnected staging");
            REQUIRE (fs::create_directory (replacedStaging));
        });
    }
    const auto result = changeLibraryLocation (tree.before, tree.destination, true, control, file);
    REQUIRE_FALSE (result);
    CHECK_FALSE (result.locations);
    CHECK (dusk::fs::loadFileAsString (tree.configuration) == configBefore);
    CHECK (dusk::fs::loadFileAsString (tree.pack.directory / "Samples/tone.wav") == tree.sample);
    CHECK (resolveLibraryInstrument (tree.before, tree.pack.locatorFor (tree.pack.instruments.front())).has_value());
    if (removedDestination) CHECK_FALSE (fs::exists (tree.destination));
    else
        for (const auto& entry : fs::directory_iterator (tree.destination))
            if (entry.path() != replacedStaging)
                CHECK (entry.path().filename().u8string().rfind (".install-", 0) != 0);
    if (replacedDestination) CHECK (fs::is_empty (tree.destination / "packs"));
    if (! removedStaging.empty()) CHECK_FALSE (fs::exists (removedStaging));
    if (! replacedStaging.empty()) CHECK (fs::is_empty (replacedStaging));
}

TEST_CASE ("SFZ external library markers prevent writes to a disconnected mount point", "[sfz][storage]")
{
    LibraryTree tree;
    const auto missing = tree.root / "disconnected";
    fs::rename (tree.source, missing);
    REQUIRE (fs::create_directory (tree.source));
    PackStore guarded (tree.source, tree.before.active.marker);
    std::string error;
    REQUIRE_FALSE (guarded.initialize (error));
    CHECK_FALSE (error.empty());
    CHECK (fs::is_empty (tree.source));
    CHECK (installedLibraryPacks (tree.before).empty());
    CHECK_FALSE (resolveLibraryInstrument (tree.before, tree.pack.locatorFor (tree.pack.instruments.front())));
    SECTION ("Moving requires the old drive to be connected")
    {
        CHECK_FALSE (changeLibraryLocation (tree.before, tree.destination, true, {}, tree.configuration));
    }
    SECTION ("A future-download folder can recover while the old drive is absent")
    {
        const auto changed = changeLibraryLocation (tree.before, tree.destination, false, {}, tree.configuration);
        REQUIRE (changed);
        REQUIRE (changed.locations);
        CHECK (changed.locations->previous.front().root == tree.source);
        CHECK (fs::is_empty (tree.source));
    }
}

TEST_CASE ("SFZ malformed location configuration reports an error without internal fallback", "[sfz][storage]")
{
    LibraryTree tree;
    std::string bad = "not json";
    SECTION ("Invalid JSON") {}
    SECTION ("Schema has the wrong type") { bad = R"({"schema":"1","active":{},"previous":[]})"; }
    SECTION ("External locations require their recorded marker")
    {
        bad = dusk::json::Json { { "schema", 1 }, { "active", { { "path", tree.source.u8string() }, { "marker", "" } } },
                                { "previous", dusk::json::Json::array() } }.dump();
    }
    SECTION ("Duplicate root keys")
    {
        bad = dusk::fs::loadFileAsString (tree.configuration);
        bad.insert (1, "\"schema\":1,");
    }
    SECTION ("Duplicate location keys")
    {
        bad = dusk::fs::loadFileAsString (tree.configuration);
        bad.insert (bad.find ('{', bad.find ("\"active\"")) + 1, "\"path\":\"/different\",");
    }
    SECTION ("A null in a decoded path")
    {
        auto value = dusk::json::Json::parse (dusk::fs::loadFileAsString (tree.configuration));
        value["active"]["path"] = tree.source.u8string() + std::string (1, '\0');
        bad = value.dump();
    }
    SECTION ("An invalid Unicode path")
    {
        bad = R"({"schema":1,"active":{"path":"/invalid\ud800","marker":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"previous":[]})";
    }
    REQUIRE (dusk::fs::writeStringToFile (tree.configuration, bad));
    const auto result = readLibraryLocations (tree.configuration);
    CHECK_FALSE (result.error.empty());
    CHECK (result.active.root.empty());
    CHECK (result.previous.empty());
}
