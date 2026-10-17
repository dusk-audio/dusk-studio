#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/sfz/SfzLibraryLocations.h"
#include "foundation/AppConfigDir.h"
#include "foundation/Fs.h"

#include <sodium.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
using namespace duskstudio::sfz;
namespace fs = std::filesystem;

class ScopedEnvironment
{
public:
    ScopedEnvironment (const char* key, const fs::path& value) : name (key)
    {
       #if defined(_WIN32)
        const auto* existing = ::_wgetenv (name.c_str());
       #else
        const auto* existing = std::getenv (name.c_str());
       #endif
        hadPrevious = existing != nullptr;
        if (existing != nullptr) previous = existing;
       #if defined(_WIN32)
        REQUIRE (::_wputenv_s (name.c_str(), value.c_str()) == 0);
       #else
        REQUIRE (::setenv (name.c_str(), value.c_str(), 1) == 0);
       #endif
    }
    ~ScopedEnvironment()
    {
       #if defined(_WIN32)
        ::_wputenv_s (name.c_str(), hadPrevious ? previous.c_str() : L"");
       #else
        if (hadPrevious) ::setenv (name.c_str(), previous.c_str(), 1);
        else ::unsetenv (name.c_str());
       #endif
    }
    ScopedEnvironment (const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator= (const ScopedEnvironment&) = delete;

private:
    fs::path name;
    fs::path::string_type previous;
    bool hadPrevious = false;
};

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
    explicit LibraryTree (bool deep = false)
        : root (fs::canonical (dusk::fs::createUniqueTempDirectory ("dusk-sfz-location-")))
    {
        if (deep)
        {
            auto directory = root;
            for (int index = 0; index < 4; ++index) directory /= std::string (72, char ('a' + index));
            source = directory / "source";
            destination = directory / fs::u8path (u8"External library Étude");
            configuration = directory / "settings/locations.json";
        }
        REQUIRE (fs::create_directories (dusk::fs::nativeIOPath (source)));
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (destination)));
        const auto configured = changeLibraryLocation ({}, source, false, {}, configuration);
        REQUIRE (configured);
        REQUIRE (configured.locations);
        before = *configured.locations;
        PackStore store (source, before.active.marker);
        std::string error;
        const auto staging = store.createStaging (error);
        REQUIRE_FALSE (staging.empty());
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (staging / "Programs")));
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (staging / "Samples")));
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
    ~LibraryTree() { std::error_code ignored; fs::remove_all (dusk::fs::nativeIOPath (root), ignored); }
    fs::path root;
    fs::path source = root / "source";
    fs::path destination = root / fs::u8path (u8"External library Étude");
    fs::path configuration = root / "locations.json";
    LibraryLocations before;
    InstalledPack pack;
    std::string sample;
};

#if ! defined(_WIN32)
void checkDestinationDisconnection (bool replace)
{
    for (const bool betweenFiles : { false, true })
    {
        LibraryTree tree;
        const auto configBefore = dusk::fs::loadFileAsString (tree.configuration);
        bool changed = false;
        const auto disconnect = [&]
        {
            if (changed) return;
            const auto offline = tree.root / "disconnected destination";
            fs::rename (tree.destination, offline);
            if (replace)
            {
                REQUIRE (fs::create_directory (tree.destination));
                REQUIRE (fs::create_directory (tree.destination / "packs"));
                REQUIRE (fs::copy_file (offline / ".dusk-sfz-library", tree.destination / ".dusk-sfz-library"));
            }
            changed = true;
        };
        OperationControl control;
        if (betweenFiles)
            control.cancelled = [&]
            {
                if (! changed)
                    for (const auto& entry : fs::directory_iterator (tree.destination))
                        if (entry.path().filename().u8string().rfind (".install-", 0) == 0)
                        { disconnect(); break; }
                return false;
            };
        else control.progress = [&] (const auto&) { disconnect(); };
        const auto result = changeLibraryLocation (tree.before, tree.destination, true, control, tree.configuration);
        REQUIRE (changed);
        REQUIRE_FALSE (result);
        CHECK_FALSE (result.locations);
        CHECK (dusk::fs::loadFileAsString (tree.configuration) == configBefore);
        CHECK (dusk::fs::loadFileAsString (tree.pack.directory / "Samples/tone.wav") == tree.sample);
        CHECK (result.failureKind == FailureKind::None);
        CHECK (resolveLibraryInstrument (tree.before, tree.pack.locatorFor (tree.pack.instruments.front())).has_value());
        if (replace)
        {
            CHECK (fs::is_empty (tree.destination / "packs"));
            for (const auto& entry : fs::directory_iterator (tree.destination))
                CHECK (entry.path().filename().u8string().rfind (".install-", 0) != 0);
        }
        else CHECK_FALSE (fs::exists (tree.destination));
    }
}
#endif
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

TEST_CASE ("SFZ library upgrades retain packs in the former default folder", "[sfz][storage]")
{
    LibraryTree tree;
    const ScopedEnvironment specialLocations ("DUSKSTUDIO_TEST_SPECIAL_LOCATIONS_ROOT", tree.root);
    const ScopedEnvironment noOverride ("DUSKSTUDIO_CONFIG_DIR", "");
    const ScopedEnvironment noScenarios ("DUSKSTUDIO_RUN_SCENARIOS", "");
    const ScopedEnvironment noSelfTest ("DUSKSTUDIO_RUN_SELFTEST", "0");
    const auto formerRoot = dusk::fs::appConfigDir() / "sfz-library";
    const auto dataRoot = dusk::fs::appDataDir() / "sfz-library";
    REQUIRE (formerRoot != dataRoot);
    REQUIRE (fs::create_directories (formerRoot.parent_path()));
    fs::copy (tree.source, formerRoot, fs::copy_options::recursive);
    REQUIRE (fs::remove (formerRoot / ".dusk-sfz-library"));
    const auto locator = tree.pack.locatorFor (tree.pack.instruments.front());
    auto expectedRoot = formerRoot;
    auto activeRoot = dataRoot;
    std::string configuration;
    const auto encode = [] (const LibraryLocation& location)
    { return dusk::json::Json { { "path", location.root.u8string() }, { "marker", location.marker } }; };
    SECTION ("The old default did not need a locations file") {}
    SECTION ("Saved external settings retain the former default as a previous root")
    {
        activeRoot = tree.source;
        fs::remove_all (tree.source / "packs");
        REQUIRE (fs::create_directory (tree.source / "packs"));
        configuration = dusk::json::Json { { "schema", 1 }, { "active", encode (tree.before.active) },
            { "previous", dusk::json::Json::array ({ encode ({ formerRoot, {} }) }) } }.dump();
    }
    SECTION ("The former default can remain the configured active root")
    {
        activeRoot = formerRoot;
        configuration = dusk::json::Json { { "schema", 1 }, { "active", encode ({ formerRoot, {} }) },
            { "previous", dusk::json::Json::array() } }.dump();
    }
    SECTION ("The new default takes precedence when both folders contain the same pack")
    {
        REQUIRE (fs::create_directories (dataRoot.parent_path()));
        fs::copy (tree.source, dataRoot, fs::copy_options::recursive);
        expectedRoot = dataRoot;
    }
    if (! configuration.empty())
        REQUIRE (dusk::fs::writeStringToFile (libraryLocationsFile(), configuration));
    const auto locations = readLibraryLocations();
    REQUIRE (locations.error.empty());
    CHECK (locations.active.root == activeRoot);
    const auto installed = installedLibraryPacks (locations);
    REQUIRE (installed.size() == 1);
    const auto resolved = resolveLibraryInstrument (locations, locator);
    REQUIRE (resolved);
    CHECK (*resolved == expectedRoot / tree.pack.directory.lexically_relative (tree.source)
        / fs::u8path (u8"Programs/Étude.sfz"));
    CHECK (libraryLocatorForFile (locations, *resolved) == locator);
    CHECK (dusk::fs::loadFileAsString (installed.front().directory / "Samples/tone.wav") == tree.sample);
    CHECK (fs::is_directory (formerRoot / "packs"));
    if (configuration.empty()) CHECK_FALSE (fs::exists (libraryLocationsFile()));
    else CHECK (dusk::fs::loadFileAsString (libraryLocationsFile()) == configuration);
}

TEST_CASE ("SFZ library locations preserve identities across copies and future-download folders", "[sfz][storage]")
{
    LibraryTree tree (GENERATE (false, true));
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
        fs::create_directory_symlink (dusk::fs::nativeIOPath (tree.source), alias, error);
        if (error) SKIP ("directory aliases are unavailable");
        tree.destination = alias;
    }
    SECTION ("A chained move does not require the disconnected emptied drive")
    {
        chained = true;
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (tree.source / ".downloads")));
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (tree.source / ".repository-cache")));
        REQUIRE (dusk::fs::writeStringToFile (tree.source / ".repository-cache/search.json", "cached listing"));
        REQUIRE (dusk::fs::writeStringToFile (tree.source / "unrelated.txt", "keep this file"));
    }
    SECTION ("An incomplete pack keeps its old root registered")
    {
        incomplete = true;
        REQUIRE (fs::create_directory (dusk::fs::nativeIOPath (tree.source / "packs/incomplete")));
        REQUIRE (dusk::fs::writeStringToFile (tree.source / "packs/incomplete/sample.wav", "user-owned sample"));
    }
    const bool sameDirectory = fs::equivalent (dusk::fs::nativeIOPath (tree.source), dusk::fs::nativeIOPath (tree.destination));
    auto changed = changeLibraryLocation (tree.before, tree.destination, move, {}, tree.configuration);
    REQUIRE (changed);
    REQUIRE (changed.locations);
    if (chained)
    {
        REQUIRE (changed.locations->previous.empty());
        CHECK (dusk::fs::loadFileAsString (tree.source / "unrelated.txt") == "keep this file");
        fs::rename (dusk::fs::nativeIOPath (tree.source), tree.root / "disconnected old drive");
        tree.destination = tree.root / "third library";
        REQUIRE (fs::create_directory (tree.destination));
        changed = changeLibraryLocation (*changed.locations, tree.destination, true, {}, tree.configuration);
        REQUIRE (changed);
        REQUIRE (changed.locations);
        CHECK (changed.locations->previous.empty());
    }
    const auto restored = readLibraryLocations (tree.configuration);
    REQUIRE (restored.error.empty());
    CHECK (restored.active.root == fs::canonical (dusk::fs::nativeIOPath (tree.destination)));
    REQUIRE (libraryLocationAvailable (restored.active));
    const auto installed = installedLibraryPacks (restored);
    REQUIRE (installed.size() == 1);
    CHECK (installed.front().locatorFor (installed.front().instruments.front()) == locator);
    const auto path = resolveLibraryInstrument (restored, locator);
    REQUIRE (path);
    CHECK (libraryLocatorForFile (restored, *path) == locator);
    CHECK (dusk::fs::loadFileAsString (installed.front().directory / "Samples/tone.wav") == tree.sample);
    if (! sameDirectory)
        CHECK (fs::exists (dusk::fs::nativeIOPath (tree.pack.directory)) == ! move);
    if (move) CHECK (installed.front().directory.parent_path().parent_path() == fs::canonical (dusk::fs::nativeIOPath (tree.destination)));
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
    auto failureKind = FailureKind::None;
    OperationControl control;
    bool cancelled = false;
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
        failureKind = FailureKind::Integrity;
    }
    SECTION ("Equal-length destination corruption fails its content hash")
    {
        const auto prepared = changeLibraryLocation ({}, tree.destination, false, {}, {});
        REQUIRE (prepared);
        const auto copy = tree.destination / "packs" / tree.pack.directory.filename();
        fs::copy (tree.pack.directory, copy, fs::copy_options::recursive);
        REQUIRE (dusk::fs::writeStringToFile (copy / "Samples/tone.wav", std::string (tree.sample.size(), 'b')));
        failureKind = FailureKind::Integrity;
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
    CHECK (result.failureKind == failureKind);
    CHECK (resolveLibraryInstrument (tree.before, tree.pack.locatorFor (tree.pack.instruments.front())).has_value());
    for (const auto& entry : fs::directory_iterator (tree.destination))
        if (entry.path() != replacedStaging)
            CHECK (entry.path().filename().u8string().rfind (".install-", 0) != 0);
    if (! removedStaging.empty()) CHECK_FALSE (fs::exists (removedStaging));
    if (! replacedStaging.empty()) CHECK (fs::is_empty (replacedStaging));
}

TEST_CASE ("SFZ library relocation never recreates a disconnected destination", "[sfz][storage][sfz-filesystem-fixture]")
{
   #if defined(_WIN32)
    SKIP ("Windows directory ownership prevents renaming an active library");
   #else
    checkDestinationDisconnection (false);
   #endif
}

TEST_CASE ("SFZ library relocation rejects a replacement destination with a copied marker", "[sfz][storage][sfz-filesystem-fixture]")
{
   #if defined(_WIN32)
    SKIP ("Windows directory ownership prevents renaming an active library");
   #else
    checkDestinationDisconnection (true);
   #endif
}

TEST_CASE ("SFZ relocation reports disk exhaustion without losing originals", "[sfz][storage][sfz-location-disk-full]")
{
   #if defined(__linux__)
    const auto* rootSetting = std::getenv ("DUSK_SFZ_CAPACITY_ROOT");
    const auto* modeSetting = std::getenv ("DUSK_SFZ_CAPACITY_MODE");
    if (! rootSetting || ! modeSetting) SKIP ("Requires the private tmpfs fixture from sfz_disk_full.sh");
    const auto destination = fs::canonical (rootSetting);
    const std::string mode = modeSetting;
    REQUIRE ((mode == "inodes" || mode == "bytes"));
    REQUIRE (fs::is_empty (destination));
    REQUIRE (fs::space (destination).capacity <= 64 * 1024);
    LibraryTree tree;
    const auto configuration = dusk::fs::loadFileAsString (tree.configuration);
    const auto changed = changeLibraryLocation (tree.before, destination, true, {}, tree.configuration);
    INFO (changed.error);
    REQUIRE (changed.status == OperationStatus::Failed);
    CHECK (changed.failureKind == FailureKind::DiskFull);
    CHECK (changed.error == (mode == "inodes" ? "Cannot mark the selected SFZ library folder"
                                             : "Cannot write the destination SFZ file"));
    CHECK_FALSE (changed.locations);
    CHECK (dusk::fs::loadFileAsString (tree.configuration) == configuration);
    CHECK (dusk::fs::loadFileAsString (tree.pack.directory / "Samples/tone.wav") == tree.sample);
    CHECK (resolveLibraryInstrument (tree.before, tree.pack.locatorFor (tree.pack.instruments.front())));
    for (const auto& entry : fs::directory_iterator (destination))
        CHECK (entry.path().filename().u8string().rfind (".install-", 0) != 0);
    if (fs::is_directory (destination / "packs")) CHECK (fs::is_empty (destination / "packs"));
   #else
    SKIP ("The bounded tmpfs fixture requires Linux user and mount namespaces");
   #endif
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
