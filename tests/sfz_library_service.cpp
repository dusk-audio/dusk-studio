#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzLibraryService.h"
#include "foundation/AppConfigDir.h"
#include "foundation/Fs.h"

#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

std::string archiveBytes()
{
    std::string bytes (1024 * 1024, '\0');
    std::size_t written = 0;
    const std::unique_ptr<struct archive, decltype (&archive_write_free)> writer (
        archive_write_new(), archive_write_free);
    REQUIRE (writer);
    REQUIRE (archive_write_set_format_zip (writer.get()) == ARCHIVE_OK);
    REQUIRE (archive_write_set_format_option (writer.get(), "zip", "compression", "store") == ARCHIVE_OK);
    REQUIRE (archive_write_set_format_option (writer.get(), "zip", "hdrcharset", "UTF-8") == ARCHIVE_OK);
    REQUIRE (archive_write_open_memory (writer.get(), bytes.data(), bytes.size(), &written) == ARCHIVE_OK);
    for (const auto& member : std::vector<std::pair<std::string, std::string>> {
             { "Piano.sfz", "<region> sample=Samples/tone.wav" },
             { "Samples/tone.wav", std::string (4096, 'x') } })
    {
        const std::unique_ptr<struct archive_entry, decltype (&archive_entry_free)> entry (
            archive_entry_new(), archive_entry_free);
        REQUIRE (entry);
        archive_entry_set_pathname_utf8 (entry.get(), member.first.c_str());
        archive_entry_set_filetype (entry.get(), AE_IFREG);
        archive_entry_set_perm (entry.get(), 0644);
        archive_entry_set_size (entry.get(), static_cast<la_int64_t> (member.second.size()));
        REQUIRE (archive_write_header (writer.get(), entry.get()) == ARCHIVE_OK);
        REQUIRE (archive_write_data (writer.get(), member.second.data(), member.second.size())
                 == static_cast<la_ssize_t> (member.second.size()));
    }
    REQUIRE (archive_write_close (writer.get()) == ARCHIVE_OK);
    bytes.resize (written);
    return bytes;
}

ProviderPack packFor (const std::string& bytes)
{
    ProviderPack pack;
    pack.sourceId = "musical-artifacts";
    pack.packId = "42";
    pack.displayName = "Service piano";
    pack.author = "Fixture author";
    pack.sourceUrl = "https://musical-artifacts.com/artifacts/42";
    pack.downloadUrl = pack.sourceUrl + "/piano.zip";
    pack.expectedSha256 = pack.releaseId = hash (bytes);
    pack.advertisedBytes = bytes.size();
    pack.versionPinned = true;
    const std::string license = "CC0-1.0 https://creativecommons.org/publicdomain/zero/1.0/";
    pack.license = { "CC0-1.0", license, {}, hash (license), pack.sourceUrl, {} };
    pack.instruments.push_back ({ "piano", "Piano", "Piano.sfz", false });
    return pack;
}

struct Tree
{
    Tree() : root (fs::canonical (dusk::fs::createUniqueTempDirectory ("dusk-sfz-service-"))) {}
    ~Tree() { std::error_code ignored; fs::remove_all (dusk::fs::nativeIOPath (root), ignored); }
    fs::path root;
};

class ScopedConfigDirectory
{
public:
    explicit ScopedConfigDirectory (const fs::path& directory)
    {
       #if defined(_WIN32)
        const auto* existing = ::_wgetenv (L"DUSKSTUDIO_CONFIG_DIR");
       #else
        const auto* existing = std::getenv (dusk::fs::kConfigDirEnv);
       #endif
        hadPrevious = existing != nullptr;
        if (existing != nullptr) previous = existing;
       #if defined(_WIN32)
        REQUIRE (::_wputenv_s (L"DUSKSTUDIO_CONFIG_DIR", directory.c_str()) == 0);
       #else
        REQUIRE (::setenv (dusk::fs::kConfigDirEnv, directory.c_str(), 1) == 0);
       #endif
    }
    ~ScopedConfigDirectory()
    {
       #if defined(_WIN32)
        ::_wputenv_s (L"DUSKSTUDIO_CONFIG_DIR", hadPrevious ? previous.c_str() : L"");
       #else
        if (hadPrevious) ::setenv (dusk::fs::kConfigDirEnv, previous.c_str(), 1);
        else ::unsetenv (dusk::fs::kConfigDirEnv);
       #endif
    }
    ScopedConfigDirectory (const ScopedConfigDirectory&) = delete;
    ScopedConfigDirectory& operator= (const ScopedConfigDirectory&) = delete;

private:
    fs::path::string_type previous;
    bool hadPrevious = false;
};

struct Reply
{
    std::string body;
    int status = 200;
    HttpHeaders headers {};
    std::size_t chunkBytes = 32768;
    std::function<void (std::size_t)> afterChunk {};
    bool hold = false;
    OperationStatus endStatus = OperationStatus::Complete;
};

class ScriptedTransport final : public HttpTransport
{
public:
    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& onHeaders,
                             const HttpBodyCallback& onBody, const OperationControl& control) override
    {
        Reply reply;
        {
            std::unique_lock<std::mutex> lock (mutex);
            requests.push_back (request);
            entered.notify_all();
            if (requests.size() > replies.size())
                return { OperationStatus::Failed, "Unexpected HTTP request" };
            reply = replies[requests.size() - 1];
            if (reply.hold && ! entered.wait_for (lock, std::chrono::seconds (5), [this] { return released; }))
                return { OperationStatus::Failed, "Fixture barrier expired" };
        }
        const auto aborted = [&]
        {
            const auto status = control.status();
            return OperationResult { status == OperationStatus::Complete ? OperationStatus::Failed : status, "Aborted" };
        };
        if (control.status() != OperationStatus::Complete) return aborted();
        if (! onHeaders ({ reply.status, request.url, reply.headers })) return aborted();
        for (std::size_t offset = 0; offset < reply.body.size(); offset += reply.chunkBytes)
        {
            if (control.status() != OperationStatus::Complete) return aborted();
            const auto chunk = std::string_view (reply.body).substr (offset, reply.chunkBytes);
            if (! onBody (chunk)) return aborted();
            if (reply.afterChunk) reply.afterChunk (offset + chunk.size());
        }
        if (control.status() != OperationStatus::Complete) return aborted();
        return { reply.endStatus, reply.endStatus == OperationStatus::Complete ? "" : "Connection interrupted" };
    }

    bool waitEntered (std::size_t count)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return entered.wait_for (lock, std::chrono::seconds (5), [&] { return requests.size() >= count; });
    }
    void release()
    {
        { const std::lock_guard<std::mutex> lock (mutex); released = true; }
        entered.notify_all();
    }
    std::size_t count() const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return requests.size();
    }
    HttpRequest request (std::size_t index) const
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return requests.at (index);
    }
    void append (Reply reply)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        replies.push_back (std::move (reply));
    }

private:
    mutable std::mutex mutex;
    std::condition_variable entered;
    bool released = false;
    std::vector<Reply> replies;
    std::vector<HttpRequest> requests;
};

LibrarySnapshot settled (LibraryService& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    for (;;)
    {
        auto snapshot = service.snapshot();
        if (! snapshot.busy) return snapshot;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            FAIL ("SFZ library worker did not finish within the bounded fixture deadline");
            return snapshot;
        }
        std::this_thread::yield();
    }
}

std::size_t ownedInstallFiles (const Tree& tree)
{
    std::size_t count = 0;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator (dusk::fs::nativeIOPath (tree.root), error);
         ! error && it != fs::recursive_directory_iterator(); it.increment (error))
    {
        const auto name = it->path().filename().u8string();
        if (name.rfind (".install-", 0) == 0 || name.find (".part") != std::string::npos) ++count;
    }
    return count;
}

void expireApiCache (const Tree& tree)
{
    for (const auto& entry : fs::directory_iterator (tree.root / ".repository-cache"))
    {
        if (entry.path().extension() != ".json") continue;
        const auto json = dusk::json::Json::parse (dusk::fs::loadFileAsString (entry.path()), nullptr, false);
        if (! json.is_discarded() && dusk::json::getString (json, "request_url").rfind ("https://api.github.com/", 0) == 0)
            fs::last_write_time (entry.path(), fs::file_time_type::clock::now() - std::chrono::hours (1));
    }
}

const std::string listing = R"([{"id":42,"name":"Service piano","author":"Fixture author","license":"cc-zero","formats":["sfz"]}])";
std::string githubListing (const std::string& name = "Grand-Piano", unsigned id = 42,
                           const std::string& owner = "sfzinstruments")
{
    return dusk::json::Json::array ({ {
        { "id", id }, { "name", name }, { "description", "Community instrument" },
        { "owner", { { "login", owner } } }, { "private", false }, { "disabled", false },
        { "license", { { "spdx_id", "CC0-1.0" } } }
    } }).dump();
}

void appendCatalogReply (const std::shared_ptr<ScriptedTransport>& transport)
{
    transport->append ({ "- name: Pianos\n  page: pianos\n  instruments:\n"
        "  - name: Grand Piano\n    page: grand_piano\n"
        "    url: https://github.com/sfzinstruments/Grand-Piano\n"
        "    short_description: Community grand piano\n" });
}

void appendAdditionalListings (const std::shared_ptr<ScriptedTransport>& transport,
                               bool instruments = false, int status = 200)
{
    transport->append ({ instruments ? githubListing ("FreePats-Piano", 42, "freepats") : "[]",
                         status, { { "retry-after", "25" } } });
    transport->append ({ instruments ? githubListing ("VCSL", 42, "sgossner") : "[]",
                         status, { { "retry-after", "25" } } });
}

ProviderSummary summary()
{
    return { "musical-artifacts", "42", "42", "Service piano", "Fixture author", {},
             "https://musical-artifacts.com/artifacts/42", "CC0-1.0" };
}
} // namespace

TEST_CASE ("SFZ library initialization and installed scans never contact a repository", "[sfz][service][ipc]")
{
    Tree tree;
    SECTION ("An old interrupted download is not resumed at startup")
    {
        REQUIRE (fs::create_directory (tree.root / ".repository-cache"));
        REQUIRE (dusk::fs::writeStringToFile (tree.root / ".repository-cache/old.part", "partial body"));
    }
    SECTION ("A new library has no repository activity") {}
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    REQUIRE (settled (service).state == LibraryState::Idle);
    REQUIRE (transport->count() == 0);
    REQUIRE (service.requestInstalled());
    REQUIRE (settled (service).installed.empty());
    REQUIRE (transport->count() == 0);
    REQUIRE_FALSE (service.resume());
}

TEST_CASE ("SFZ library sweeps abandoned work without losing reusable downloads", "[sfz][service][storage][ipc]")
{
    Tree tree;
    REQUIRE (fs::create_directory (tree.root / "packs"));
    const auto cache = tree.root / ".repository-cache";
    REQUIRE (fs::create_directory (cache));
    const auto bytes = archiveBytes();
    const auto pack = packFor (bytes);
    const auto part = cache / (hash (providerPackToJson (pack)) + ".part");
    ScriptedTransport interrupted;
    Reply prefix { bytes.substr (0, 64), 200,
        { { "content-length", std::to_string (bytes.size()) }, { "etag", "\"version1\"" } } };
    prefix.endStatus = OperationStatus::Failed;
    interrupted.append (std::move (prefix));
    REQUIRE (downloadFile (interrupted, { pack.downloadUrl, part, pack.expectedSha256, ArchiveLimits{}.maxArchiveBytes }).preservePart);
    const auto staging = tree.root / (".install-" + std::string (32, 'a'));
    const auto rollback = tree.root / (".install-" + std::string (32, 'b') + ".replaced");
    const auto unrelated = tree.root / ".install-unrelated";
    const auto uppercaseStage = tree.root / (".install-" + std::string (32, 'A'));
    for (const auto& directory : { staging, rollback, unrelated, uppercaseStage })
    {
        REQUIRE (fs::create_directory (directory));
        REQUIRE (dusk::fs::writeStringToFile (directory / "keep-or-sweep", "contents"));
    }
    const auto temporary = cache / (".cache-" + std::string (32, 'c'));
    const auto invalid = cache / (std::string (64, 'd') + ".part");
    const auto orphan = cache / (std::string (64, 'e') + ".part.resume.json");
    const auto foreign = cache / ".cache-unrelated";
    const auto uppercaseCache = cache / (".cache-" + std::string (32, 'C'));
    for (const auto& file : { temporary, invalid, orphan, foreign, uppercaseCache })
        REQUIRE (dusk::fs::writeStringToFile (file, "keep-or-sweep"));
    const auto directoryPretendingToBeFile = cache / (".cache-" + std::string (32, 'f'));
    REQUIRE (fs::create_directory (directoryPretendingToBeFile));
    const auto uncertain = cache / (std::string (64, 'f') + ".part");
    REQUIRE (fs::copy_file (part, uncertain));
    auto sidecar = part; sidecar += ".resume.json";
    auto uncertainSidecar = uncertain; uncertainSidecar += ".resume.json";
    REQUIRE (fs::copy_file (sidecar, uncertainSidecar));
    std::error_code linkError;
    fs::create_hard_link (uncertainSidecar, cache / "unrelated-sidecar-link", linkError);
    if (! linkError) REQUIRE (inspectDownloadPart (uncertain, ArchiveLimits{}.maxArchiveBytes) == DownloadPartState::Unavailable);
    const auto outside = tree.root / "unrelated-target";
    REQUIRE (fs::create_directory (outside));
    REQUIRE (dusk::fs::writeStringToFile (outside / "keep", "contents"));
    const auto symbolicStage = tree.root / (".install-" + std::string (32, '0'));
    fs::create_directory_symlink (outside, symbolicStage, linkError);
    const bool madeSymbolicStage = ! linkError;
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    REQUIRE (settled (service).state == LibraryState::Idle);
    CHECK (transport->count() == 0);
    CHECK_FALSE (fs::exists (staging));
    CHECK_FALSE (fs::exists (temporary));
    CHECK_FALSE (fs::exists (invalid));
    CHECK_FALSE (fs::exists (orphan));
    CHECK (fs::exists (rollback / "keep-or-sweep"));
    CHECK (fs::exists (unrelated / "keep-or-sweep"));
    CHECK (fs::exists (uppercaseStage / "keep-or-sweep"));
    CHECK (fs::exists (foreign));
    CHECK (fs::exists (uppercaseCache));
    CHECK (fs::is_directory (directoryPretendingToBeFile));
    CHECK (dusk::fs::loadFileAsString (uncertain) == bytes.substr (0, 64));
    CHECK (fs::exists (uncertainSidecar));
    CHECK (fs::exists (outside / "keep"));
    if (madeSymbolicStage) CHECK (fs::is_symlink (fs::symlink_status (symbolicStage)));
    CHECK (dusk::fs::loadFileAsString (part) == bytes.substr (0, 64));
    transport->append ({ bytes.substr (64), 206,
        { { "content-length", std::to_string (bytes.size() - 64) },
          { "content-range", "bytes 64-" + std::to_string (bytes.size() - 1) + "/" + std::to_string (bytes.size()) },
          { "etag", "\"version1\"" } } });
    REQUIRE (service.install (pack));
    REQUIRE (settled (service).state == LibraryState::Complete);
    REQUIRE (transport->count() == 1);
    CHECK (transport->request (0).headers.at ("range") == "bytes=64-");
    CHECK (transport->request (0).headers.at ("if-range") == "\"version1\"");
    CHECK (service.snapshot().installed.size() == 1);
}

TEST_CASE ("SFZ library startup cannot sweep directories with an active owner", "[sfz][service][storage][ipc]")
{
    Tree tree;
    const auto cache = tree.root / ".repository-cache";
    const auto temporary = cache / (".cache-" + std::string (32, 'a'));
    const auto partial = cache / (std::string (64, 'b') + ".part");
    fs::path staging;
    const auto transport = std::make_shared<ScriptedTransport>();
    {
        PackStore active (tree.root);
        std::string error;
        staging = active.createStaging (error);
        REQUIRE_FALSE (staging.empty());
        REQUIRE (fs::create_directory (cache));
        DirectoryLease repositoryOwner;
        REQUIRE (repositoryOwner.acquire (cache, DirectoryLease::Access::Shared, error));
        REQUIRE (dusk::fs::writeStringToFile (temporary, "active write"));
        REQUIRE (dusk::fs::writeStringToFile (partial, "active download"));
        LibraryService second (tree.root, transport);
        REQUIRE (settled (second).state == LibraryState::Idle);
        CHECK (fs::is_directory (staging));
        CHECK (dusk::fs::loadFileAsString (temporary) == "active write");
        CHECK (dusk::fs::loadFileAsString (partial) == "active download");
        CHECK (transport->count() == 0);
    }
    LibraryService restarted (tree.root, transport);
    REQUIRE (settled (restarted).state == LibraryState::Idle);
    CHECK_FALSE (fs::exists (staging));
    CHECK_FALSE (fs::exists (temporary));
    CHECK_FALSE (fs::exists (partial));
    CHECK (transport->count() == 0);
   #if ! defined(_WIN32)
    SECTION ("Repository work reacquires ownership after its directory is replaced")
    {
        fs::rename (cache, tree.root / "moved-cache");
        REQUIRE (fs::create_directory (cache));
        transport->append ({ listing });
        REQUIRE (restarted.browse ("musical-artifacts"));
        REQUIRE (settled (restarted).state == LibraryState::Complete);
        DirectoryLease maintenance;
        std::string error;
        CHECK_FALSE (maintenance.acquire (cache, DirectoryLease::Access::Exclusive, error));
        CHECK (error.empty());
        CHECK (transport->count() == 1);
    }
   #endif
}

TEST_CASE ("SFZ library preserves saved downloads when its metadata cache is unavailable", "[sfz][service][storage][ipc]")
{
    Tree tree;
    const auto configuration = tree.root / "configuration";
    const auto library = tree.root / "library";
    REQUIRE (fs::create_directory (configuration));
    REQUIRE (fs::create_directory (library));
    auto settings = configuration;
    SECTION ("The configured data directory uses a plain parent") {}
    SECTION ("The configured data directory may use a symbolic parent")
    {
        const auto alias = tree.root / "configuration-alias";
        std::error_code error;
        fs::create_directory_symlink (tree.root, alias, error);
        if (error) SKIP ("The filesystem cannot create a symbolic-directory fixture");
        settings = alias / "configuration";
    }
    const ScopedConfigDirectory isolated (settings);
    REQUIRE (changeLibraryLocation ({}, library, false, {}, libraryLocationsFile()));
    REQUIRE (fs::create_directory (library / ".downloads"));
    const auto bytes = archiveBytes();
    const auto pack = packFor (bytes);
    const auto part = library / ".downloads" / (hash (providerPackToJson (pack)) + ".part");
    ScriptedTransport interrupted;
    Reply first { bytes.substr (0, 64), 200,
        { { "content-length", std::to_string (bytes.size()) }, { "etag", "\"version1\"" } } };
    first.endStatus = OperationStatus::Failed;
    interrupted.append (std::move (first));
    const auto prefix = downloadFile (interrupted, { pack.downloadUrl, part, pack.expectedSha256, ArchiveLimits{}.maxArchiveBytes });
    REQUIRE_FALSE (prefix);
    REQUIRE (prefix.preservePart);
    REQUIRE (ownedInstallFiles (tree) == 2);
    const auto cache = configuration / "sfz-cache" / "repository";
    REQUIRE (fs::create_directory (cache.parent_path()));
    REQUIRE (dusk::fs::writeStringToFile (cache, "blocked"));
    const auto transport = std::make_shared<ScriptedTransport>();
    {
        LibraryService service;
        REQUIRE (settled (service).state == LibraryState::Failed);
        REQUIRE (service.setTransportForTest (transport));
        REQUIRE (service.install (pack));
        REQUIRE (settled (service).state == LibraryState::Failed);
        CHECK (transport->count() == 0);
        REQUIRE (ownedInstallFiles (tree) == 2);
    }
    REQUIRE (fs::remove (cache));
    transport->append ({ bytes.substr (64), 206,
        { { "content-length", std::to_string (bytes.size() - 64) },
          { "content-range", "bytes 64-" + std::to_string (bytes.size() - 1) + "/" + std::to_string (bytes.size()) },
          { "etag", "\"version1\"" } } });
    LibraryService restored;
    REQUIRE (settled (restored).state == LibraryState::Idle);
    REQUIRE (fs::is_directory (cache));
    REQUIRE (restored.setTransportForTest (transport));
    REQUIRE (restored.install (pack));
    const auto result = settled (restored);
    INFO (result.error);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.installed.size() == 1);
    REQUIRE (transport->count() == 1);
    CHECK (transport->request (0).headers.at ("range") == "bytes=64-");
    CHECK (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ service changes storage without replacing its worker or portable identities", "[sfz][service][storage][ipc]")
{
    Tree source, destination;
    const auto bytes = archiveBytes();
    const auto provider = packFor (bytes);
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ bytes });
    LibraryService service (source.root, transport);
    settled (service);
    REQUIRE (service.install (provider));
    const auto installed = settled (service);
    REQUIRE (installed.installed.size() == 1);
    const auto original = installed.installed.front();
    const auto locator = original.locatorFor (original.instruments.front());
    bool move = true;
    SECTION ("Verified move") {}
    SECTION ("Future downloads retain the old pack") { move = false; }
    SECTION ("Paused move blocks loads until explicit cancellation")
    {
        service.setHeavyWork (true);
        REQUIRE (service.changeLocation (destination.root, true));
        CHECK (service.isMovingPacks());
        REQUIRE (settled (service).state == LibraryState::Paused);
        CHECK (service.isMovingPacks());
        CHECK_FALSE (service.requestInstalled());
        service.cancel();
        REQUIRE (settled (service).state == LibraryState::Cancelled);
        CHECK_FALSE (service.isMovingPacks());
        CHECK (fs::exists (original.directory));
        service.setHeavyWork (false);
    }
    REQUIRE (service.changeLocation (destination.root, move));
    auto changed = settled (service);
    INFO (changed.error);
    REQUIRE (changed.state == LibraryState::Complete);
    CHECK_FALSE (service.isMovingPacks());
    CHECK (changed.locations.active.root == destination.root);
    REQUIRE (changed.installed.size() == 1);
    CHECK (changed.installed.front().locatorFor (changed.installed.front().instruments.front()) == locator);
    CHECK (fs::exists (original.directory) == ! move);
    CHECK (transport->count() == 1);
    REQUIRE (service.requestInstalled());
    CHECK (settled (service).installed.size() == 1);
    REQUIRE (service.remove (changed.installed.front(), {}));
    CHECK (settled (service).installed.empty());
    CHECK (transport->count() == 1);
}

TEST_CASE ("SFZ library installs a downloaded archive and publishes its portable locator", "[sfz][service][ipc]")
{
    Tree tree;
    auto root = tree.root;
    SECTION ("Ordinary library path") {}
    SECTION ("Deep library path")
    {
        for (int index = 0; index < 4; ++index) root /= std::string (72, char ('a' + index));
    }
    const auto bytes = archiveBytes();
    auto pack = packFor (bytes);
    pack.presentation.artworkUrl = "https://fixtures.example/piano.png";
    pack.presentation.categoryHint = "piano";
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ bytes, 200, { { "content-length", std::to_string (bytes.size()) } } });
    LibraryService service (root, transport);
    settled (service);
    REQUIRE (service.install (pack));
    const auto result = settled (service);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.installed.size() == 1);
    CHECK (result.installed[0].provider.presentation.artworkUrl == pack.presentation.artworkUrl);
    CHECK (result.installed[0].provider.presentation.categoryHint == pack.presentation.categoryHint);
    REQUIRE (result.installed[0].archiveSha256 == hash (bytes));
    REQUIRE (transport->count() == 1);
    const auto locator = result.installed[0].locatorFor (result.installed[0].instruments.front());
    PackStore offline (root);
    REQUIRE (offline.resolve (locator));
    REQUIRE (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ library pauses before HTTP during heavy work and requires explicit resume", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ bytes, 200, {} });
    LibraryService service (tree.root, transport);
    settled (service);
    service.setHeavyWork (true);
    REQUIRE (service.install (packFor (bytes)));
    const auto paused = settled (service);
    REQUIRE (paused.state == LibraryState::Paused);
    REQUIRE (transport->count() == 0);
    REQUIRE_FALSE (service.resume());
    REQUIRE_FALSE (service.browse ("musical-artifacts"));
    service.setHeavyWork (false);
    REQUIRE (service.snapshot().state == LibraryState::Paused);
    REQUIRE (transport->count() == 0);
    REQUIRE (service.resume());
    const auto completed = settled (service);
    REQUIRE (completed.state == LibraryState::Complete);
    REQUIRE (completed.generation > paused.generation);
    REQUIRE (completed.installed.size() == 1);
    REQUIRE (transport->count() == 1);
}

TEST_CASE ("SFZ library preserves interrupted downloads and resumes a validated HTTP range", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    settled (service);
    Reply first { bytes, 200, { { "content-length", std::to_string (bytes.size()) }, { "etag", "\"version1\"" } }, 64 };
    first.afterChunk = [&] (std::size_t) { service.setHeavyWork (true); };
    transport->append (std::move (first));
    REQUIRE (service.install (packFor (bytes)));
    const auto paused = settled (service);
    REQUIRE (paused.state == LibraryState::Paused);
    REQUIRE (paused.installed.empty());
    REQUIRE (ownedInstallFiles (tree) == 2);
    transport->append ({ bytes.substr (64), 206,
        { { "content-length", std::to_string (bytes.size() - 64) },
          { "content-range", "bytes 64-" + std::to_string (bytes.size() - 1) + "/" + std::to_string (bytes.size()) },
          { "etag", "\"version1\"" } } });
    service.setHeavyWork (false);
    REQUIRE (service.resume());
    REQUIRE (settled (service).state == LibraryState::Complete);
    REQUIRE (transport->count() == 2);
    REQUIRE (transport->request (1).headers.at ("range") == "bytes=64-");
    REQUIRE (transport->request (1).headers.at ("if-range") == "\"version1\"");
    REQUIRE (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ library cancellation removes interrupted state and accepts a new generation", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    settled (service);
    Reply first { bytes, 200, { { "etag", "\"version1\"" } }, 64 };
    bool overLimit = false;
    SECTION ("A small archive can be cancelled") {}
    SECTION ("A multi-gigabyte collection can be cancelled after its first chunk")
    { first.headers["content-length"] = std::to_string (3ULL * 1024 * 1024 * 1024); }
    SECTION ("An archive beyond the extraction limit is rejected before its first chunk")
    {
        first.headers["content-length"] = std::to_string (ArchiveLimits{}.maxArchiveBytes + 1);
        overLimit = true;
    }
    first.afterChunk = [&] (std::size_t) { service.cancel(); };
    transport->append (std::move (first));
    REQUIRE (service.install (packFor (bytes)));
    const auto cancelled = settled (service);
    REQUIRE (cancelled.state == (overLimit ? LibraryState::Failed : LibraryState::Cancelled));
    if (overLimit) CHECK (cancelled.error.find ("size limit") != std::string::npos);
    REQUIRE (cancelled.installed.empty());
    REQUIRE (ownedInstallFiles (tree) == 0);
    transport->append ({ bytes, 200, {} });
    REQUIRE (service.install (packFor (bytes)));
    const auto completed = settled (service);
    REQUIRE (completed.state == LibraryState::Complete);
    REQUIRE (completed.generation > cancelled.generation);
    REQUIRE (completed.installed.size() == 1);
}

TEST_CASE ("SFZ library serializes requests and cancels a paused job without resuming HTTP", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    settled (service);
    SECTION ("Busy requests are rejected")
    {
        Reply reply { listing };
        reply.hold = true;
        transport->append (std::move (reply));
        REQUIRE (service.browse ("musical-artifacts"));
        REQUIRE (transport->waitEntered (1));
        REQUIRE_FALSE (service.requestInstalled());
        REQUIRE_FALSE (service.resolve (summary()));
        service.cancel();
        transport->release();
        REQUIRE (settled (service).state == LibraryState::Cancelled);
        REQUIRE (transport->count() == 1);
    }
    SECTION ("Paused cancellation does not fetch")
    {
        service.setHeavyWork (true);
        REQUIRE (service.install (packFor (archiveBytes())));
        REQUIRE (settled (service).state == LibraryState::Paused);
        service.cancel();
        REQUIRE (settled (service).state == LibraryState::Cancelled);
        REQUIRE_FALSE (service.resume());
        REQUIRE (transport->count() == 0);
        REQUIRE (ownedInstallFiles (tree) == 0);
    }
}

TEST_CASE ("SFZ repository cache retains the last valid listing and supports explicit offline browsing", "[sfz][service][ipc]")
{
    Tree tree;
    auto root = tree.root;
    SECTION ("Ordinary cache path") {}
    SECTION ("Deep cache path")
    {
        for (int index = 0; index < 4; ++index) root /= std::string (72, char ('a' + index));
    }
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ listing, 200, { { "content-type", "application/json" } } });
    transport->append ({ "{broken JSON", 200, { { "content-type", "application/json" } } });
    {
        LibraryService service (root, transport);
        settled (service);
        REQUIRE (service.browse ("musical-artifacts"));
        REQUIRE (settled (service).page.items.size() == 1);
        REQUIRE (service.browse ("musical-artifacts"));
        REQUIRE (settled (service).state == LibraryState::Failed);
        REQUIRE (service.browse ("musical-artifacts", 1, {}, true));
        const auto cached = settled (service);
        REQUIRE (cached.state == LibraryState::Complete);
        REQUIRE (cached.cachedResponse);
        REQUIRE (cached.page.items.size() == 1);
        REQUIRE (transport->count() == 2);
    }
    const auto offline = std::make_shared<ScriptedTransport>();
    LibraryService restored (root, offline);
    settled (restored);
    REQUIRE (offline->count() == 0);
    REQUIRE (restored.browse ("musical-artifacts", 1, {}, true));
    REQUIRE (settled (restored).cachedResponse);
    REQUIRE (restored.browse ("musical-artifacts", 1, "uncached", true));
    REQUIRE (settled (restored).state == LibraryState::Failed);
    REQUIRE (offline->count() == 0);
}

TEST_CASE ("SFZ GitHub metadata freshness avoids repeated requests and repairs invalid caches", "[sfz][service][ipc]")
{
    Tree tree;
    bool corrupt = false, expired = false;
    SECTION ("Recent metadata is reused without a request") {}
    SECTION ("A corrupt recent response is fetched again") { corrupt = true; }
    SECTION ("Expired metadata is fetched again") { expired = true; }
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ githubListing ("FreePats-Piano", 42, "freepats") });
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.browse ("freepats"));
    REQUIRE (settled (service).page.items.size() == 1);
    if (corrupt)
        for (const auto& entry : fs::directory_iterator (tree.root / ".repository-cache"))
        {
            auto json = dusk::json::Json::parse (dusk::fs::loadFileAsString (entry.path()));
            json["body"] = "[]";
            REQUIRE (dusk::fs::writeStringToFile (entry.path(), json.dump()));
        }
    if (expired) expireApiCache (tree);
    if (corrupt || expired) transport->append ({ githubListing ("FreePats-Flute", 44, "freepats") });
    REQUIRE (service.browse ("freepats"));
    const auto result = settled (service);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.page.items.size() == 1);
    CHECK (result.page.items.front().packId == (corrupt || expired ? "44" : "42"));
    CHECK (transport->count() == (corrupt || expired ? 2 : 1));
}

TEST_CASE ("SFZ repository revalidation sends saved ETags and retains the validated body", "[sfz][service][ipc]")
{
    Tree tree;
    bool corrupt = false, updated = false;
    SECTION ("The server confirms the saved representation") {}
    SECTION ("A corrupt cached body triggers an unconditional repair") { corrupt = true; }
    SECTION ("A not-modified response refreshes its validator and rate-limit metadata") { updated = true; }
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ listing, 200, { { "ETag", "\"catalog-v1\"" }, { "X-RateLimit-Remaining", "59" } } });
    transport->append ({ corrupt ? listing : std::string {}, corrupt ? 200 : 304,
        { { "ETag", updated ? "\"catalog-v2\"" : "\"catalog-v1\"" }, { "X-RateLimit-Remaining", "58" } } });
    {
        LibraryService service (tree.root, transport);
        settled (service);
        REQUIRE (service.browse ("musical-artifacts"));
        REQUIRE (settled (service).page.items.size() == 1);
    }
    if (corrupt)
        for (const auto& entry : fs::directory_iterator (tree.root / ".repository-cache"))
            if (entry.path().extension() == ".json")
            {
                auto json = dusk::json::Json::parse (dusk::fs::loadFileAsString (entry.path()));
                json["body"] = "{broken JSON";
                REQUIRE (dusk::fs::writeStringToFile (entry.path(), json.dump()));
            }
    LibraryService restored (tree.root, transport);
    settled (restored);
    REQUIRE (restored.browse ("musical-artifacts"));
    const auto result = settled (restored);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.page.items.size() == 1);
    CHECK (result.page.rateLimit.remaining == "58");
    CHECK (transport->request (0).headers.count ("if-none-match") == 0);
    if (corrupt) CHECK (transport->request (1).headers.count ("if-none-match") == 0);
    else CHECK (transport->request (1).headers.at ("if-none-match") == "\"catalog-v1\"");
    REQUIRE (restored.browse ("musical-artifacts", 1, {}, true));
    const auto cached = settled (restored);
    REQUIRE (cached.page.items.size() == 1);
    CHECK (cached.page.rateLimit.remaining == "58");
    CHECK (transport->count() == 2);
    if (updated)
    {
        transport->append ({ {}, 304, {} });
        REQUIRE (restored.browse ("musical-artifacts"));
        REQUIRE (settled (restored).state == LibraryState::Complete);
        CHECK (transport->request (2).headers.at ("if-none-match") == "\"catalog-v2\"");
    }
}

TEST_CASE ("SFZ repository does not accept an unsolicited not-modified response", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ {}, 304, {} });
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.browse ("musical-artifacts"));
    CHECK (settled (service).state == LibraryState::Failed);
    REQUIRE (service.browse ("musical-artifacts", 1, {}, true));
    CHECK (settled (service).state == LibraryState::Failed);
    CHECK (transport->count() == 1);
}

TEST_CASE ("SFZ repository cache evicts old entries within count and byte limits", "[sfz][service][ipc]")
{
    Tree tree;
    const auto cache = tree.root / ".repository-cache";
    REQUIRE (fs::create_directory (cache));
    std::size_t count = 257;
    std::uintmax_t bytes = 1;
    bool expired = false;
    SECTION ("Entry count") {}
    SECTION ("Total bytes") { count = 8; bytes = 9 * 1024 * 1024; }
    SECTION ("Entries older than thirty days") { count = 1; expired = true; }
    const auto now = fs::file_time_type::clock::now();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto file = cache / (hash (std::to_string (i)) + ".json");
        REQUIRE (dusk::fs::writeStringToFile (file, "x"));
        fs::resize_file (file, bytes);
        fs::last_write_time (file, now - std::chrono::hours (expired ? 24 * 31 : 1)
                                      - std::chrono::seconds (count - i));
    }
    REQUIRE (dusk::fs::writeStringToFile (cache / "unrelated.json", "keep"));
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ listing, 200, {} });
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.browse ("musical-artifacts"));
    REQUIRE (settled (service).state == LibraryState::Complete);
    CHECK_FALSE (fs::exists (cache / (hash ("0") + ".json")));
    CHECK (dusk::fs::loadFileAsString (cache / "unrelated.json") == "keep");
    std::size_t remaining = 0;
    std::uintmax_t total = 0;
    for (const auto& entry : fs::directory_iterator (cache))
        if (entry.path().extension() == ".json" && isSha256 (entry.path().stem().u8string()))
        { ++remaining; total += entry.file_size(); }
    CHECK (remaining <= 256);
    CHECK (total <= 64 * 1024 * 1024);
}

TEST_CASE ("SFZ repository detail resolution can use validated cached metadata offline", "[sfz][service][ipc]")
{
    using Json = dusk::json::Json;
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    bool vcsl = false;
    SECTION ("Musical Artifacts metadata survives restart") {}
    SECTION ("The VCSL README license survives restart without a cached failed request") { vcsl = true; }
    auto selected = summary();
    auto expectedDigest = hash (bytes);
    const unsigned expectedRequests = vcsl ? 5 : 2;
    const auto detail = Json {
        { "id", 42 }, { "name", "Service piano" }, { "author", "Fixture author" },
        { "license", "cc-zero" }, { "formats", { "sfz" } },
        { "file", "https://musical-artifacts.com/artifacts/42/piano.zip" },
        { "file_hash", hash (bytes) }, { "file_list", { "Piano.sfz", "Samples/tone.wav" } }
    }.dump();
    if (vcsl)
    {
        selected.sourceId = "versilian";
        selected.locator = "VCSL";
        expectedDigest.clear();
        const std::string revision (40, 'a'), treeSha (40, 'b');
        const std::string grant = "An open CC0 general-purpose sample library.\nThis collection is under a Creative Commons 0 license.\n";
        std::string encoded (sodium_base64_encoded_len (grant.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
        REQUIRE (sodium_bin2base64 (encoded.data(), encoded.size(),
            reinterpret_cast<const unsigned char*> (grant.data()), grant.size(), sodium_base64_VARIANT_ORIGINAL) != nullptr);
        encoded.resize (encoded.find ('\0'));
        transport->append ({ Json::parse (githubListing ("VCSL", 42, "sgossner"))[0].dump() });
        transport->append ({ Json { { "tag_name", "sfz-v1" }, { "draft", false }, { "prerelease", false } }.dump() });
        transport->append ({ Json { { "sha", revision }, { "commit", { { "tree", { { "sha", treeSha } } } } } }.dump() });
        transport->append ({ Json { { "truncated", false }, { "tree", Json::array ({
            { { "path", "README.md" }, { "type", "blob" }, { "mode", "100644" } },
            { { "path", "Piano.sfz" }, { "type", "blob" }, { "mode", "100644" } }
        }) } }.dump() });
        transport->append ({ Json { { "path", "README.md" }, { "encoding", "base64" }, { "content", encoded } }.dump() });
    }
    else
    {
        transport->append ({ detail });
        transport->append ({ R"([{"short_name":"cc-zero","name":"Creative Commons CC0 1.0 Universal","license_type":"cc"}])" });
    }
    {
        LibraryService service (tree.root, transport);
        settled (service);
        REQUIRE (service.resolve (selected));
        const auto result = settled (service);
        REQUIRE (result.state == LibraryState::Complete);
        REQUIRE (result.pack);
        REQUIRE (result.pack->expectedSha256 == expectedDigest);
        REQUIRE (providerPackFromJson (providerPackToJson (*result.pack)));
        REQUIRE (transport->count() == expectedRequests);
    }
    const auto offline = std::make_shared<ScriptedTransport>();
    LibraryService restored (tree.root, offline);
    settled (restored);
    REQUIRE (restored.resolve (selected, true));
    const auto cached = settled (restored);
    REQUIRE (cached.state == LibraryState::Complete);
    REQUIRE (cached.cachedResponse);
    REQUIRE (cached.pack);
    REQUIRE (cached.pack->expectedSha256 == expectedDigest);
    REQUIRE (offline->count() == 0);
}

TEST_CASE ("SFZ library rejects bad download content without publishing or retaining an install tree", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    auto expected = archiveBytes();
    std::string body = expected;
    SECTION ("Upstream digest mismatch") { body = "different body"; }
    SECTION ("Digest valid but ZIP malformed") { expected = body = "this is not a zip archive"; }
    transport->append ({ body });
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.install (packFor (expected)));
    const auto result = settled (service);
    REQUIRE (result.state == LibraryState::Failed);
    REQUIRE_FALSE (result.error.empty());
    CHECK (result.failureKind == FailureKind::Integrity);
    REQUIRE (result.installed.empty());
    REQUIRE (ownedInstallFiles (tree) == 0);
    REQUIRE (service.requestInstalled());
    const auto refreshed = settled (service);
    CHECK (refreshed.state == LibraryState::Complete);
    CHECK (refreshed.failureKind == FailureKind::None);
}

TEST_CASE ("SFZ library resumes extraction from a completed locally verified part without HTTP", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto normalized = providerPackFromJson (providerPackToJson (packFor (bytes)));
    REQUIRE (normalized);
    REQUIRE (normalized.pack);
    const auto pack = *normalized.pack;
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    settled (service);
    const auto part = tree.root / ".repository-cache" / (hash (providerPackToJson (pack)) + ".part");
    ScriptedTransport priorDownload;
    priorDownload.append ({ bytes });
    REQUIRE (downloadFile (priorDownload, { pack.downloadUrl, part, pack.expectedSha256 }));
    service.setHeavyWork (true);
    REQUIRE (service.install (pack));
    REQUIRE (settled (service).state == LibraryState::Paused);
    service.setHeavyWork (false);
    REQUIRE (service.resume());
    const auto result = settled (service);
    INFO (result.error);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.installed.size() == 1);
    REQUIRE (transport->count() == 0);
    REQUIRE (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ library refuses unlicensed descriptors before creating a download job", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    const auto initial = settled (service);
    auto pack = packFor (archiveBytes());
    pack.license.id = "free";
    REQUIRE_FALSE (service.install (pack));
    REQUIRE (service.snapshot().generation == initial.generation);
    REQUIRE (transport->count() == 0);
    REQUIRE (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ library removal forwards session usage and refreshes installed state", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ bytes });
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.install (packFor (bytes)));
    const auto installed = settled (service);
    REQUIRE (installed.installed.size() == 1);
    const auto pack = installed.installed.front();
    const auto locator = pack.locatorFor (pack.instruments.front());
    REQUIRE (service.remove (pack, { locator }));
    const auto refused = settled (service);
    REQUIRE (refused.state == LibraryState::Failed);
    REQUIRE (refused.installed.size() == 1);
    REQUIRE (service.remove (pack, {}));
    const auto removed = settled (service);
    REQUIRE (removed.state == LibraryState::Complete);
    REQUIRE (removed.installed.empty());
    REQUIRE (transport->count() == 1);
}

TEST_CASE ("SFZ library shutdown preserves paused downloads unless explicitly cancelled", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    bool cancelBeforeShutdown = false;
    SECTION ("Quit retains the validated prefix") {}
    SECTION ("Cancel followed immediately by quit removes it") { cancelBeforeShutdown = true; }
    {
        LibraryService service (tree.root, transport);
        settled (service);
        Reply reply { bytes, 200, { { "etag", "\"version1\"" } }, 64 };
        reply.afterChunk = [&] (std::size_t) { service.setHeavyWork (true); };
        transport->append (std::move (reply));
        REQUIRE (service.install (packFor (bytes)));
        REQUIRE (settled (service).state == LibraryState::Paused);
        REQUIRE (ownedInstallFiles (tree) == 2);
        if (cancelBeforeShutdown) service.cancel();
    }
    REQUIRE (ownedInstallFiles (tree) == (cancelBeforeShutdown ? 0 : 2));
    REQUIRE (transport->count() == 1);
    const auto resumed = std::make_shared<ScriptedTransport>();
    if (cancelBeforeShutdown) resumed->append ({ bytes, 200, {} });
    else resumed->append ({ bytes.substr (64), 206,
        { { "content-length", std::to_string (bytes.size() - 64) },
          { "content-range", "bytes 64-" + std::to_string (bytes.size() - 1) + "/" + std::to_string (bytes.size()) },
          { "etag", "\"version1\"" } } });
    LibraryService restored (tree.root, resumed);
    settled (restored);
    REQUIRE (restored.install (packFor (bytes)));
    const auto result = settled (restored);
    INFO (result.error);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.installed.size() == 1);
    REQUIRE (resumed->count() == 1);
    if (cancelBeforeShutdown) CHECK (resumed->request (0).headers.count ("range") == 0);
    else
    {
        CHECK (resumed->request (0).headers.at ("range") == "bytes=64-");
        CHECK (resumed->request (0).headers.at ("if-range") == "\"version1\"");
    }
    CHECK (dusk::fs::loadFileAsString (result.installed.front().directory / "Samples/tone.wav") == std::string (4096, 'x'));
    CHECK (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ library retries failed downloads using their saved validators", "[sfz][service][ipc]")
{
    Tree tree;
    const auto bytes = archiveBytes();
    const auto transport = std::make_shared<ScriptedTransport>();
    Reply first { bytes.substr (0, 64), 200,
        { { "content-length", std::to_string (bytes.size()) }, { "etag", "\"version1\"" } } };
    first.endStatus = OperationStatus::Failed;
    transport->append (std::move (first));
    auto service = std::make_unique<LibraryService> (tree.root, transport);
    settled (*service);
    REQUIRE (service->install (packFor (bytes)));
    REQUIRE (settled (*service).state == LibraryState::Failed);
    REQUIRE (ownedInstallFiles (tree) == 2);
    bool restartFresh = false;
    SECTION ("Retry in the same service") {}
    SECTION ("Retry after shutdown")
    {
        service.reset();
        REQUIRE (ownedInstallFiles (tree) == 2);
        service = std::make_unique<LibraryService> (tree.root, transport);
        settled (*service);
    }
    SECTION ("Quit a paused retry before HTTP preserves the existing prefix")
    {
        service->setHeavyWork (true);
        REQUIRE (service->install (packFor (bytes)));
        REQUIRE (settled (*service).state == LibraryState::Paused);
        REQUIRE (transport->count() == 1);
        service.reset();
        REQUIRE (ownedInstallFiles (tree) == 2);
        service = std::make_unique<LibraryService> (tree.root, transport);
        settled (*service);
    }
    SECTION ("Cancel a paused retry before HTTP and quit")
    {
        service->setHeavyWork (true);
        REQUIRE (service->install (packFor (bytes)));
        REQUIRE (settled (*service).state == LibraryState::Paused);
        REQUIRE (transport->count() == 1);
        service->cancel();
        service.reset();
        REQUIRE (ownedInstallFiles (tree) == 0);
        service = std::make_unique<LibraryService> (tree.root, transport);
        settled (*service);
        restartFresh = true;
    }
    SECTION ("A mismatched resume validator discards the saved pair")
    {
        transport->append ({ bytes.substr (64), 206,
            { { "content-length", std::to_string (bytes.size() - 64) },
              { "content-range", "bytes 64-" + std::to_string (bytes.size() - 1) + "/" + std::to_string (bytes.size()) },
              { "etag", "\"version2\"" } } });
        REQUIRE (service->install (packFor (bytes)));
        const auto rejected = settled (*service);
        REQUIRE (rejected.state == LibraryState::Failed);
        CHECK (rejected.error == "Download entity changed; restart the download");
        REQUIRE (transport->count() == 2);
        CHECK (transport->request (1).headers.at ("range") == "bytes=64-");
        REQUIRE (ownedInstallFiles (tree) == 0);
        restartFresh = true;
    }
    const auto retryRequest = transport->count();
    if (restartFresh) transport->append ({ bytes, 200, {} });
    else transport->append ({ bytes.substr (64), 206,
        { { "content-length", std::to_string (bytes.size() - 64) },
          { "content-range", "bytes 64-" + std::to_string (bytes.size() - 1) + "/" + std::to_string (bytes.size()) },
          { "etag", "\"version1\"" } } });
    REQUIRE (service->install (packFor (bytes)));
    const auto result = settled (*service);
    INFO (result.error);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.installed.size() == 1);
    if (restartFresh) CHECK (transport->request (retryRequest).headers.count ("range") == 0);
    else
    {
        CHECK (transport->request (retryRequest).headers.at ("range") == "bytes=64-");
        CHECK (transport->request (retryRequest).headers.at ("if-range") == "\"version1\"");
    }
    CHECK (ownedInstallFiles (tree) == 0);
}

TEST_CASE ("SFZ library shutdown joins an idle worker after acknowledged jobs", "[sfz][service][ipc][sfz-shutdown]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    bool refresh = false;
    SECTION ("Initialization has just finished") {}
    SECTION ("An installed scan has just finished") { refresh = true; }
    for (int iteration = 0; iteration < 64; ++iteration)
    {
        LibraryService service (tree.root, transport);
        REQUIRE (settled (service).state == LibraryState::Idle);
        if (refresh)
        {
            REQUIRE (service.requestInstalled());
            REQUIRE (settled (service).state == LibraryState::Complete);
        }
    }
    REQUIRE (transport->count() == 0);
}

TEST_CASE ("SFZ all repositories searches every source and retains resolvable identities", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ githubListing() });
    appendCatalogReply (transport);
    transport->append ({ listing });
    appendAdditionalListings (transport, true);
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.browse ("all", 1, "Piano"));
    const auto result = settled (service);
    REQUIRE (result.state == LibraryState::Complete);
    REQUIRE (result.error.empty());
    REQUIRE (result.page.items.size() == 4);
    CHECK (result.page.items[0].sourceId == "sfzinstruments");
    CHECK (result.page.items[1].sourceId == "musical-artifacts");
    CHECK (result.page.items[2].sourceId == "freepats");
    CHECK (result.page.items[3].sourceId == "versilian");
    CHECK (result.page.items[0].packId == result.page.items[1].packId);
    CHECK (result.page.items[0].locator == "Grand-Piano");
    CHECK (result.page.items[1].locator == "42");
    REQUIRE (transport->count() == 5);
    CHECK (transport->request (0).url.find ("api.github.com/orgs/sfzinstruments/") != std::string::npos);
    CHECK (transport->request (1).url == "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml");
    CHECK (transport->request (2).url.find ("q=Piano") != std::string::npos);
    CHECK (transport->request (3).url.find ("users/freepats/") != std::string::npos);
    CHECK (transport->request (4).url.find ("users/sgossner/") != std::string::npos);

    transport->append ({ dusk::json::Json {
        { "id", 42 }, { "name", "Service piano" }, { "author", "Fixture author" },
        { "license", "cc-zero" }, { "formats", { "sfz" } },
        { "file", "https://musical-artifacts.com/artifacts/42/piano.zip" },
        { "file_hash", hash ("archive") }, { "file_list", { "Piano.sfz" } }
    }.dump() });
    transport->append ({ R"([{"short_name":"cc-zero","name":"Creative Commons CC0 1.0 Universal","license_type":"cc"}])" });
    REQUIRE (service.resolve (result.page.items[1]));
    const auto resolved = settled (service);
    REQUIRE (resolved.state == LibraryState::Complete);
    REQUIRE (resolved.pack);
    CHECK (resolved.pack->sourceId == "musical-artifacts");
    CHECK (resolved.pack->packId == "42");
    CHECK (transport->count() == 7);
}

TEST_CASE ("SFZ all repositories keeps independent pagination through sparse search pages", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ githubListing ("Banjo"), 200, { { "x-total-pages", "3" } } });
    appendCatalogReply (transport);
    transport->append ({ githubListing(), 200, { { "x-total-pages", "3" } } });
    transport->append ({ listing });
    appendAdditionalListings (transport);
    transport->append ({ githubListing ("Upright-Piano", 43) });
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.browse ("all", 1, "Piano"));
    const auto first = settled (service);
    REQUIRE (first.state == LibraryState::Complete);
    REQUIRE (first.page.items.size() == 2);
    REQUIRE (first.page.nextPage == 2);
    REQUIRE (transport->count() == 6);
    CHECK_FALSE (first.cachedResponse);
    CHECK (transport->request (2).url.find ("page=2") != std::string::npos);

    REQUIRE (service.browse ("all", first.page.nextPage, "Piano"));
    const auto second = settled (service);
    REQUIRE (second.state == LibraryState::Complete);
    REQUIRE (second.page.items.size() == 1);
    CHECK (second.page.items.front().sourceId == "sfzinstruments");
    CHECK (second.page.items.front().packId == "43");
    CHECK (second.page.nextPage == 0);
    REQUIRE (transport->count() == 7);
    CHECK_FALSE (second.cachedResponse);
    CHECK (transport->request (6).url.find ("page=3") != std::string::npos);

    REQUIRE (service.browse ("all", 1, "Piano", true));
    const auto cachedFirst = settled (service);
    REQUIRE (cachedFirst.state == LibraryState::Complete);
    CHECK (cachedFirst.cachedResponse);
    CHECK (cachedFirst.page.items.size() == 2);
    REQUIRE (service.browse ("all", cachedFirst.page.nextPage, "Piano", true));
    const auto cachedSecond = settled (service);
    REQUIRE (cachedSecond.state == LibraryState::Complete);
    CHECK (cachedSecond.page.items.size() == 1);
    REQUIRE (service.browse ("all", 1, "Piano", true));
    CHECK (settled (service).page.items.size() == 2);
    CHECK (transport->count() == 7);
}

TEST_CASE ("SFZ source searches skip sparse pages and restore catalog descriptions offline", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    auto swirly = dusk::json::Json::parse (githubListing ("karoryfer.swirly-drums", 77));
    swirly[0]["description"] = nullptr;
    const std::string description = "Punk/indie drums and unusual percussion played with brushes.";
    transport->append ({ githubListing ("Banjo"), 200, { { "x-total-pages", "3" } } });
    transport->append ({ "- name: Drums\n  page: drums\n  instruments:\n"
        "  - name: Swirly Drums\n  # page: swirly_drums\n"
        "    url: https://karoryfer-samples.myshopify.com/pages/free-swirly-drums\n"
        "    short_description:\n      \"" + description + "\"\n" });
    transport->append ({ githubListing ("Flute", 43), 200, { { "x-total-pages", "3" } } });
    transport->append ({ swirly.dump() });
    {
        LibraryService service (tree.root, transport);
        settled (service);
        REQUIRE (service.browse ("sfzinstruments", 1, "BrUsHeS"));
        const auto result = settled (service);
        REQUIRE (result.state == LibraryState::Complete);
        CHECK (result.error.empty());
        CHECK_FALSE (result.cachedResponse);
        REQUIRE (result.page.items.size() == 1);
        CHECK (result.page.items.front().locator == "karoryfer.swirly-drums");
        CHECK (result.page.items.front().description == description);
        CHECK (result.page.items.front().presentation.categoryHint == description);
        CHECK (result.page.nextPage == 0);
        REQUIRE (transport->count() == 4);
        CHECK (transport->request (0).url.find ("page=1") != std::string::npos);
        CHECK (transport->request (1).url == "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml");
        CHECK (transport->request (2).url.find ("page=2") != std::string::npos);
        CHECK (transport->request (3).url.find ("page=3") != std::string::npos);
    }
    const auto offline = std::make_shared<ScriptedTransport>();
    LibraryService restored (tree.root, offline);
    settled (restored);
    REQUIRE (restored.browse ("sfzinstruments", 1, "BRUSHES", true));
    const auto cached = settled (restored);
    REQUIRE (cached.state == LibraryState::Complete);
    CHECK (cached.error.empty());
    CHECK (cached.cachedResponse);
    REQUIRE (cached.page.items.size() == 1);
    CHECK (cached.page.items.front().packId == "77");
    CHECK (cached.page.items.front().description == description);
    CHECK (cached.page.items.front().presentation.categoryHint == description);
    CHECK (cached.page.nextPage == 0);
    CHECK (offline->count() == 0);
}

TEST_CASE ("SFZ all repositories retains successful sources and reports source failures", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    settled (service);
    bool githubFails = true, bothFail = false;
    SECTION ("First source is unavailable") {}
    SECTION ("Second source is unavailable") { githubFails = false; }
    SECTION ("Both sources fail without replacing the last results") { bothFail = true; }
    if (bothFail)
    {
        transport->append ({ githubListing() });
        appendCatalogReply (transport);
        transport->append ({ listing });
        appendAdditionalListings (transport);
        REQUIRE (service.browse ("all", 1, "Piano"));
        REQUIRE (settled (service).page.items.size() == 2);
        expireApiCache (tree);
    }
    transport->append ({ githubListing(), githubFails ? 429 : 200, { { "retry-after", "25" } } });
    if (! githubFails) appendCatalogReply (transport);
    transport->append ({ listing, ! githubFails || bothFail ? 429 : 200, { { "retry-after", "25" } } });
    appendAdditionalListings (transport, false, bothFail ? 429 : 200);
    REQUIRE (service.browse ("all", 1, "Piano"));
    const auto result = settled (service);
    REQUIRE (result.state == (bothFail ? LibraryState::Failed : LibraryState::Complete));
    REQUIRE (result.error.find (githubFails ? "SFZ Instruments" : "Musical Artifacts") != std::string::npos);
    CHECK (result.error.find ("25") != std::string::npos);
    if (bothFail)
    {
        CHECK (result.error.find ("Musical Artifacts") != std::string::npos);
        CHECK (result.error.find ("FreePats") != std::string::npos);
        CHECK (result.error.find ("Versilian Studios") != std::string::npos);
        REQUIRE (result.page.items.size() == 2);
        CHECK (result.page.items.front().packId == "42");
        CHECK (transport->count() == 9);
        return;
    }
    REQUIRE (result.page.items.size() == 1);
    CHECK (result.page.items.front().sourceId == (githubFails ? "musical-artifacts" : "sfzinstruments"));
    REQUIRE (result.page.nextPage == 2);
    REQUIRE (transport->count() == (githubFails ? 4 : 5));
    const auto retryRequest = transport->count();
    transport->append ({ githubFails ? githubListing() : listing });
    if (githubFails) appendCatalogReply (transport);
    REQUIRE (service.browse ("all", result.page.nextPage, "Piano"));
    const auto retried = settled (service);
    REQUIRE (retried.state == LibraryState::Complete);
    CHECK (retried.error.empty());
    REQUIRE (retried.page.items.size() == 1);
    CHECK (retried.page.items.front().sourceId == (githubFails ? "sfzinstruments" : "musical-artifacts"));
    CHECK (retried.page.nextPage == 0);
    CHECK (transport->count() == 6);
    CHECK (transport->request (retryRequest).url.find ("page=1") != std::string::npos);
}

TEST_CASE ("SFZ all repositories keeps validated caches separate from failed sources", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    LibraryService service (tree.root, transport);
    settled (service);
    SECTION ("A malformed source cannot overwrite its last good cache")
    {
        transport->append ({ githubListing() });
        appendCatalogReply (transport);
        transport->append ({ listing });
        appendAdditionalListings (transport);
        REQUIRE (service.browse ("all", 1, "Piano"));
        REQUIRE (settled (service).page.items.size() == 2);
        expireApiCache (tree);
        transport->append ({ "{broken JSON" });
        transport->append ({ listing });
        appendAdditionalListings (transport);
        REQUIRE (service.browse ("all", 1, "Piano"));
        const auto partial = settled (service);
        REQUIRE (partial.state == LibraryState::Complete);
        CHECK_FALSE (partial.error.empty());
        REQUIRE (partial.page.items.size() == 1);
        REQUIRE (service.browse ("all", 1, "Piano", true));
        const auto cached = settled (service);
        REQUIRE (cached.state == LibraryState::Complete);
        CHECK (cached.cachedResponse);
        CHECK (cached.error.empty());
        CHECK (cached.page.items.size() == 2);
        CHECK (transport->count() == 9);
    }
    SECTION ("Missing source cache is explicit and never starts HTTP")
    {
        transport->append ({ listing });
        REQUIRE (service.browse ("musical-artifacts", 1, "Piano"));
        REQUIRE (settled (service).page.items.size() == 1);
        REQUIRE (service.browse ("all", 1, "Piano", true));
        const auto cached = settled (service);
        REQUIRE (cached.state == LibraryState::Complete);
        CHECK (cached.cachedResponse);
        CHECK (cached.error.find ("SFZ Instruments") != std::string::npos);
        CHECK (cached.error.find ("FreePats") != std::string::npos);
        CHECK (cached.error.find ("Versilian Studios") != std::string::npos);
        REQUIRE (cached.page.items.size() == 1);
        CHECK (cached.page.items.front().sourceId == "musical-artifacts");
        CHECK (transport->count() == 1);
    }
}

TEST_CASE ("SFZ all repositories interruption preserves the last complete page", "[sfz][service][ipc]")
{
    Tree tree;
    const auto transport = std::make_shared<ScriptedTransport>();
    transport->append ({ githubListing() });
    appendCatalogReply (transport);
    transport->append ({ listing });
    appendAdditionalListings (transport);
    LibraryService service (tree.root, transport);
    settled (service);
    REQUIRE (service.browse ("all", 1, "Piano"));
    REQUIRE (settled (service).page.items.size() == 2);
    bool pause = false, holdSecond = false;
    SECTION ("Cancel during the first source") {}
    SECTION ("Pause during the first source and resume") { pause = true; }
    SECTION ("Cancel while the second source is pending") { holdSecond = true; }
    Reply reply { githubListing ("New-Piano", 99) };
    if (! holdSecond)
        reply.afterChunk = [&] (std::size_t) { if (pause) service.setHeavyWork (true); else service.cancel(); };
    transport->append (std::move (reply));
    if (holdSecond)
    {
        Reply held { listing };
        held.hold = true;
        transport->append (std::move (held));
    }
    REQUIRE (service.browse ("all", 1, "New"));
    if (holdSecond)
    {
        REQUIRE (transport->waitEntered (7));
        service.cancel();
        transport->release();
    }
    const auto stopped = settled (service);
    REQUIRE (stopped.state == (pause ? LibraryState::Paused : LibraryState::Cancelled));
    REQUIRE (stopped.page.items.size() == 2);
    CHECK (stopped.page.items.front().packId == "42");
    CHECK (transport->count() == (holdSecond ? 7 : 6));
    if (pause)
    {
        transport->append ({ githubListing ("New-Piano", 99) });
        transport->append ({ listing });
        appendAdditionalListings (transport);
        service.setHeavyWork (false);
        REQUIRE (service.resume());
        const auto resumed = settled (service);
        REQUIRE (resumed.state == LibraryState::Complete);
        REQUIRE (resumed.page.items.size() == 2);
        CHECK (resumed.page.items.front().packId == "99");
        CHECK (transport->count() == 10);
    }
}
