#include "SfzLibraryService.h"
#include "SfzCatalogJson.h"
#include "../../foundation/AppConfigDir.h"
#include "../../foundation/Fs.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>

#if defined(__APPLE__)
 #include <pthread.h>
#elif defined(__linux__)
 #include <sys/resource.h>
#endif

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
using Json = dusk::json::Json;
constexpr std::uint64_t kMaxCacheBody = 8 * 1024 * 1024;
constexpr std::uint64_t kMaxCacheBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxCacheEntries = 256;

std::string digest (std::string_view text)
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (text.data()), text.size());
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

FailureKind storageFailureKind (const std::error_code& error)
{
    return error == std::errc::no_space_on_device ? FailureKind::DiskFull : FailureKind::None;
}

bool plainDirectory (const fs::path& path, std::error_code& error)
{
    const auto ioPath = dusk::fs::nativeIOPath (path);
    fs::create_directory (ioPath, error);
    if (error) return false;
    return fs::is_directory (fs::symlink_status (ioPath, error)) && ! error;
}

bool lowerHex (std::string_view name)
{
    return std::all_of (name.begin(), name.end(), [] (char c)
    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void removePlainFile (const fs::path& path)
{
    std::error_code error;
    const auto ioPath = dusk::fs::nativeIOPath (path);
    if (! fs::is_regular_file (fs::symlink_status (ioPath, error)) || error) return;
    if (fs::hard_link_count (ioPath, error) != 1 || error) return;
    fs::remove (ioPath, error);
}

void sweepWorkDirectory (const fs::path& directory) noexcept
{
    try
    {
        DirectoryLease maintenance;
        std::string ownershipError;
        if (! maintenance.acquire (directory, DirectoryLease::Access::Exclusive, ownershipError)) return;
        std::error_code error;
        for (fs::directory_iterator it (dusk::fs::nativeIOPath (directory), error), end; ! error && it != end; it.increment (error))
        {
            const auto name = it->path().filename().u8string();
            if (name.size() == 39 && name.compare (0, 7, ".cache-") == 0 && lowerHex (std::string_view (name).substr (7)))
            {
                removePlainFile (it->path());
                continue;
            }
            const bool part = name.size() == 69 && name.compare (64, 5, ".part") == 0;
            const bool sidecar = name.size() == 81 && name.compare (64, 17, ".part.resume.json") == 0;
            if ((! part && ! sidecar) || ! lowerHex (std::string_view (name).substr (0, 64))) continue;
            const auto path = directory / fs::u8path (name.substr (0, 69));
            if (inspectDownloadPart (path, ArchiveLimits{}.maxArchiveBytes) != DownloadPartState::Invalid) continue;
            removePlainFile (path);
            auto metadata = path;
            metadata += ".resume.json";
            removePlainFile (metadata);
        }
    }
    catch (...) {}
}

void sweepLibraryStaging (const LibraryLocation& location) noexcept
{
    try
    {
        if (! libraryLocationAvailable (location)) return;
        DirectoryLease maintenance;
        std::string ownershipError;
        if (! maintenance.acquire (location.root, DirectoryLease::Access::Exclusive, ownershipError)
            || ! libraryLocationAvailable (location)) return;
        std::error_code error;
        for (fs::directory_iterator it (dusk::fs::nativeIOPath (location.root), error), end; ! error && it != end; it.increment (error))
        {
            const auto name = it->path().filename().u8string();
            if (name.size() != 41 || name.compare (0, 9, ".install-") != 0
                || ! lowerHex (std::string_view (name).substr (9))) continue;
            std::error_code entryError;
            if (fs::is_directory (it->symlink_status (entryError)) && ! entryError)
                fs::remove_all (it->path(), entryError);
        }
    }
    catch (...) {}
}

bool atomicCacheWrite (const fs::path& target, const std::string& text)
{
    if (sodium_init() < 0) return false;
    std::array<unsigned char, 16> random {};
    std::array<char, 33> name {};
    randombytes_buf (random.data(), random.size());
    sodium_bin2hex (name.data(), name.size(), random.data(), random.size());
    const auto temporary = dusk::fs::nativeIOPath (target.parent_path() / (std::string (".cache-") + name.data()));
    if (! dusk::fs::writeStringToFile (temporary, text))
    {
        std::error_code ignored;
        fs::remove (temporary, ignored);
        return false;
    }
    std::error_code error;
#if defined(_WIN32)
    const bool replaced = MoveFileExW (temporary.c_str(), dusk::fs::nativeIOPath (target).c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    fs::rename (temporary, target, error);
    const bool replaced = ! error;
#endif
    if (! replaced) fs::remove (temporary, error);
    return replaced;
}

void trimRepositoryCache (const fs::path& root)
{
    struct Entry { fs::path path; fs::file_time_type modified; std::uintmax_t bytes; };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    std::error_code error;
    for (auto it = fs::directory_iterator (dusk::fs::nativeIOPath (root), error); ! error && it != fs::directory_iterator(); it.increment (error))
    {
        if (it->path().extension() != ".json" || ! isSha256 (it->path().stem().u8string())) continue;
        std::error_code entryError;
        if (! fs::is_regular_file (it->symlink_status (entryError)) || entryError) continue;
        const auto size = it->file_size (entryError);
        if (entryError) continue;
        const auto modified = it->last_write_time (entryError);
        if (entryError) continue;
        if (size > kMaxCacheBytes)
        {
            fs::remove (it->path(), entryError);
            continue;
        }
        entries.push_back ({ it->path(), modified, size });
        total += size;
    }
    std::sort (entries.begin(), entries.end(), [] (const auto& a, const auto& b) { return a.modified < b.modified; });
    std::size_t count = entries.size();
    const auto oldest = fs::file_time_type::clock::now() - std::chrono::hours (24 * 30);
    for (const auto& entry : entries)
    {
        if (entry.modified >= oldest && count <= kMaxCacheEntries && total <= kMaxCacheBytes) break;
        std::error_code removeError;
        if (fs::remove (entry.path, removeError)) { --count; total -= entry.bytes; }
    }
}

OperationResult interrupted (const OperationControl& control)
{
    const auto status = control.status();
    return { status, status == OperationStatus::Paused ? "SFZ library work is paused" : "SFZ library work was cancelled" };
}

class CachedTransport final : public HttpTransport
{
public:
    CachedTransport (HttpTransport& next, fs::path root, bool offline)
        : upstream (next), cache (std::move (root)), offlineOnly (offline) {}

    OperationResult perform (const HttpRequest& request, const HttpHeadersCallback& headers,
                             const HttpBodyCallback& body, const OperationControl& control) override
    {
        const auto key = digest (Json { { "url", request.url }, { "headers", request.headers } }.dump());
        if (key.empty()) return { OperationStatus::Failed, "Cannot identify repository cache entry" };
        const auto path = dusk::fs::nativeIOPath (cache / (key + ".json"));
        std::error_code cacheError;
        const auto modified = fs::last_write_time (path, cacheError);
        const auto freshness = request.url == "https://raw.githubusercontent.com/sfzinstruments/sfzinstruments.github.io/master/data/sfz/instruments.yml"
            ? std::chrono::minutes (360) : request.url.rfind ("https://api.github.com/", 0) == 0
                ? std::chrono::minutes (5) : std::chrono::minutes (0);
        const bool fresh = ! cacheError
            && modified <= fs::file_time_type::clock::now()
            && fs::file_time_type::clock::now() - modified < freshness;
        if (offlineOnly)
        {
            std::error_code error;
            if (! fs::is_regular_file (fs::symlink_status (path, error)) || error
                || dusk::fs::fileSize (path) > static_cast<std::int64_t> (kMaxCacheBody * 3))
                return { OperationStatus::Failed, "No cached response is available for this repository request" };
            const auto parsed = detail::parseJsonRejectingDuplicateKeys (dusk::fs::loadFileAsString (path), "Repository cache");
            if (! parsed) return { OperationStatus::Failed, parsed.error };
            const auto& json = *parsed.root;
            const auto responseHeaders = json.find ("headers");
            const auto content = json.find ("body");
            if (dusk::json::getInt (json, "schema", 0) != 2
                || dusk::json::getString (json, "request_url") != request.url
                || responseHeaders == json.end() || ! responseHeaders->is_object()
                || content == json.end() || ! content->is_string()
                || content->get_ref<const std::string&>().size() > kMaxCacheBody
                || digest (content->get_ref<const std::string&>()) != dusk::json::getString (json, "body_sha256"))
                return { OperationStatus::Failed, "Repository cache entry is invalid" };
            HttpResponse response { 200, dusk::json::getString (json, "response_url"), {} };
            std::size_t headerBytes = 0;
            for (auto it = responseHeaders->begin(); it != responseHeaders->end(); ++it)
            {
                if (! it->is_string()) return { OperationStatus::Failed, "Repository cache headers are invalid" };
                const auto& value = it->get_ref<const std::string&>();
                headerBytes += it.key().size() + value.size();
                if (headerBytes > 65536) return { OperationStatus::Failed, "Repository cache headers are too large" };
                response.headers[it.key()] = value;
            }
            if (control.status() != OperationStatus::Complete) return interrupted (control);
            if (! headers (response)) return { OperationStatus::Failed, "Cached repository response was rejected" };
            const auto& text = content->get_ref<const std::string&>();
            for (std::size_t offset = 0; offset < text.size(); offset += 32768)
            {
                if (control.status() != OperationStatus::Complete) return interrupted (control);
                if (! body (std::string_view (text).substr (offset, 32768)))
                    return { OperationStatus::Failed, "Cached repository response was rejected" };
            }
            usedCache = true;
            return { OperationStatus::Complete, {} };
        }
        HttpResponse cachedResponse;
        std::string cachedContent, etag;
        CachedTransport saved (upstream, cache, true);
        const auto cached = saved.perform (request, [&] (const HttpResponse& value)
        {
            cachedResponse = value;
            const auto found = value.headers.find ("etag");
            if (found != value.headers.end() && found->second.size() <= 1024
                && found->second.find_first_of ("\r\n") == std::string::npos
                && found->second.find ('\0') == std::string::npos) etag = found->second;
            return true;
        }, [&] (std::string_view bytes)
        {
            cachedContent.append (bytes.data(), bytes.size());
            return true;
        }, control);
        const auto replay = [&] () -> OperationResult
        {
            if (control.status() != OperationStatus::Complete) return interrupted (control);
            if (! headers (cachedResponse)) return { OperationStatus::Failed, "Revalidated repository response was rejected" };
            for (std::size_t offset = 0; offset < cachedContent.size(); offset += 32768)
            {
                if (control.status() != OperationStatus::Complete) return interrupted (control);
                if (! body (std::string_view (cachedContent).substr (offset, 32768)))
                    return { OperationStatus::Failed, "Revalidated repository response was rejected" };
            }
            return { OperationStatus::Complete, {} };
        };
        if (cached && fresh) return replay();
        auto conditional = request;
        if (cached && ! etag.empty()) conditional.headers["if-none-match"] = etag;
        HttpResponse response;
        std::string content;
        const auto result = upstream.perform (conditional,
            [&] (const HttpResponse& value)
            {
                response = value;
                HttpHeaders normalized;
                for (const auto& item : value.headers)
                    if (! normalized.emplace (dusk::text::toLowerCase (item.first), item.second).second) return false;
                response.headers = std::move (normalized);
                return response.statusCode == 304 || headers (response);
            },
            [&] (std::string_view bytes)
            {
                if (response.statusCode == 304) return bytes.empty();
                if (bytes.size() > kMaxCacheBody - content.size()) return false;
                content.append (bytes.data(), bytes.size());
                return body (bytes);
            }, control);
        if (result && response.statusCode == 304)
        {
            if (! cached || etag.empty()) return { OperationStatus::Failed, "Repository returned 304 without a validated cached response" };
            for (const auto* name : { "etag", "last-modified", "link", "x-ratelimit-remaining", "x-ratelimit-reset", "retry-after" })
            {
                const auto found = response.headers.find (name);
                if (found != response.headers.end()) cachedResponse.headers[name] = found->second;
            }
            const auto replayed = replay();
            if (! replayed) return replayed;
            response = std::move (cachedResponse);
            content = std::move (cachedContent);
        }
        if (result && response.statusCode == 200)
        {
            const auto contentHash = digest (content);
            Json entry { { "schema", 2 }, { "request_url", request.url },
                         { "response_url", response.url }, { "headers", response.headers },
                         { "body_sha256", contentHash }, { "body", std::move (content) } };
            updates.emplace_back (path, entry.dump());
        }
        return result;
    }

    void commit()
    {
        for (const auto& update : updates) atomicCacheWrite (update.first, update.second);
        updates.clear();
        trimRepositoryCache (cache);
    }

    bool usedCache = false;

private:
    HttpTransport& upstream;
    fs::path cache;
    bool offlineOnly;
    std::vector<std::pair<fs::path, std::string>> updates;
};

LibraryState stateFor (OperationStatus status)
{
    switch (status)
    {
        case OperationStatus::Complete: return LibraryState::Complete;
        case OperationStatus::Paused: return LibraryState::Paused;
        case OperationStatus::Cancelled: return LibraryState::Cancelled;
        case OperationStatus::Failed: return LibraryState::Failed;
    }
    return LibraryState::Failed;
}

std::string rateLimitError (std::string error, const ProviderRateLimit& limit)
{
    const auto append = [&error] (const char* label, const std::string& value)
    {
        if (value.empty()) return;
        std::string safe;
        for (const char character : value.substr (0, 128))
        {
            const auto c = static_cast<unsigned char> (character);
            if (c >= 32 && c < 127) safe += character;
        }
        if (! safe.empty()) error += std::string ("; ") + label + safe;
    };
    append ("Retry-After: ", limit.retryAfter);
    append ("Rate limit resets at Unix time ", limit.reset);
    return error;
}
} // namespace

LibraryService::LibraryService()
    : LibraryService (readLibraryLocations(), libraryLocationsFile(), {}, dusk::fs::appDataDir()) {}

LibraryService::LibraryService (fs::path root, std::shared_ptr<HttpTransport> supplied)
    : LibraryService (LibraryLocations { { std::move (root), {} }, {}, {} }, {}, std::move (supplied), {}) {}

LibraryService::LibraryService (LibraryLocations configured, fs::path file, std::shared_ptr<HttpTransport> supplied,
                                fs::path cacheDirectory)
    : store (configured.active.root, configured.active.marker),
      transport (supplied ? std::move (supplied) : std::make_shared<CurlTransport>()),
      locations (std::move (configured)), configurationFile (std::move (file)),
      cacheRoot (cacheDirectory.empty() ? store.root() / ".repository-cache"
                                        : cacheDirectory / "sfz-cache" / "repository"),
      artworkCache (cacheDirectory.empty() ? store.root() / ".artwork-cache"
                                            : cacheDirectory / "sfz-cache" / "artwork", transport),
      worker ([this] { run(); }) {}

LibraryService::~LibraryService()
{
    artworkCache.cancel();
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping.store (true, std::memory_order_release);
    }
    wake.notify_one();
    if (worker.joinable()) worker.join();
}

LibrarySnapshot LibraryService::snapshot() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return current;
}

bool LibraryService::setTransportForTest (std::shared_ptr<HttpTransport> replacement)
{
    const std::lock_guard<std::mutex> lock (mutex);
    if (current.busy || paused || stopping.load (std::memory_order_acquire)) return false;
    transport = replacement ? std::move (replacement) : std::make_shared<CurlTransport>();
    artworkCache.setTransport (transport);
    return true;
}

bool LibraryService::enqueue (Job job)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (current.busy || paused || stopping.load (std::memory_order_acquire)) return false;
        if (job.kind == JobKind::Install)
        {
            const auto key = digest (providerPackToJson (job.pack));
            if (key.empty()) return false;
            const auto downloads = configurationFile.empty() ? cacheRoot : store.root() / ".downloads";
            job.part = downloads / (key + ".part");
            job.preserveDownload = true;
        }
        job.generation = ++current.generation;
        current.busy = true;
        current.error.clear();
        current.failureKind = FailureKind::None;
        current.cachedResponse = false;
        current.progress = {};
        if (job.kind == JobKind::ChangeLocation) movingPacks.store (job.moveExisting, std::memory_order_release);
        if (job.kind == JobKind::Resolve) current.pack.reset();
        if (job.kind == JobKind::Install) current.pack = job.pack;
        cancelRequested.store (false, std::memory_order_release);
        pending = std::move (job);
    }
    wake.notify_one();
    return true;
}

bool LibraryService::browse (std::string sourceId, unsigned page, std::string query, bool offlineOnly)
{
    Job job;
    job.kind = JobKind::Browse;
    job.sourceId = std::move (sourceId);
    job.page = page;
    job.query = std::move (query);
    job.offlineOnly = offlineOnly;
    return enqueue (std::move (job));
}

bool LibraryService::resolve (ProviderSummary summary, bool offlineOnly)
{
    Job job;
    job.kind = JobKind::Resolve;
    job.summary = std::move (summary);
    job.offlineOnly = offlineOnly;
    return enqueue (std::move (job));
}

bool LibraryService::install (ProviderPack pack)
{
    auto validated = providerPackFromJson (providerPackToJson (pack));
    if (! validated || ! validated.pack) return false;
    Job job;
    job.kind = JobKind::Install;
    job.pack = std::move (*validated.pack);
    if (pack.presentation.artworkUrl.size() <= 2048)
        job.pack.presentation.artworkUrl = std::move (pack.presentation.artworkUrl);
    if (pack.presentation.categoryHint.size() <= 8192)
        job.pack.presentation.categoryHint = std::move (pack.presentation.categoryHint);
    return enqueue (std::move (job));
}

bool LibraryService::requestInstalled()
{
    return enqueue (Job {});
}

bool LibraryService::remove (InstalledPack pack, std::vector<ManagedSfzLocator> inUse)
{
    Job job;
    job.kind = JobKind::Remove;
    job.installed = std::move (pack);
    job.inUse = std::move (inUse);
    return enqueue (std::move (job));
}

bool LibraryService::changeLocation (fs::path destination, bool moveExisting)
{
    Job job;
    job.kind = JobKind::ChangeLocation;
    job.destination = std::move (destination);
    job.moveExisting = moveExisting;
    return enqueue (std::move (job));
}

bool LibraryService::resume()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (! paused || current.busy || heavyWork.load (std::memory_order_acquire)
            || stopping.load (std::memory_order_acquire)) return false;
        pending = std::move (paused);
        paused.reset();
        pending->generation = ++current.generation;
        current.busy = true;
        current.error.clear();
        current.failureKind = FailureKind::None;
        cancelRequested.store (false, std::memory_order_release);
    }
    wake.notify_one();
    return true;
}

void LibraryService::cancel()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (paused && ! current.busy)
        {
            pending = std::move (paused);
            paused.reset();
            pending->kind = JobKind::Cleanup;
            pending->generation = ++current.generation;
            current.busy = true;
        }
        else if (! current.busy) return;
        cancelRequested.store (true, std::memory_order_release);
    }
    wake.notify_one();
}

void LibraryService::setState (std::uint64_t generation, LibraryState state)
{
    const std::lock_guard<std::mutex> lock (mutex);
    if (current.generation == generation) current.state = state;
}

void LibraryService::cleanInstall (Job& job, bool removeDownload) noexcept
{
    try
    {
        if ((! job.staging.empty() || ! job.part.empty()) && ! libraryLocationAvailable (locations.active)) return;
        std::error_code error;
        if (! job.staging.empty()) fs::remove_all (dusk::fs::nativeIOPath (job.staging), error);
        job.staging.clear();
        if (removeDownload && ! job.part.empty())
        {
            if (! fs::is_directory (fs::symlink_status (dusk::fs::nativeIOPath (job.part.parent_path()), error)) || error) return;
            DirectoryLease cleanupLease;
            std::string ownershipError;
            if (! cleanupLease.acquire (job.part.parent_path(), DirectoryLease::Access::Shared, ownershipError)) return;
            fs::remove (dusk::fs::nativeIOPath (job.part), error);
            auto sidecar = job.part;
            sidecar += ".resume.json";
            fs::remove (dusk::fs::nativeIOPath (sidecar), error);
            job.part.clear();
        }
    }
    catch (...) {}
}

OperationResult LibraryService::browseAll (const Job& job, const OperationControl& control)
{
    if (job.page == 0 || job.page > 200 || job.query.size() > 256)
        return { OperationStatus::Failed, "Invalid repository browse request" };
    const auto sources = providerSources();
    BrowseSession session;
    if (job.page == 1)
    {
        session.query = job.query;
        session.offlineOnly = job.offlineOnly;
        session.pages.push_back (std::vector<unsigned> (sources.size(), 1));
    }
    else
    {
        if (! allBrowse || allBrowse->query != job.query || allBrowse->offlineOnly != job.offlineOnly
            || job.page > allBrowse->pages.size())
            return { OperationStatus::Failed, "Start this repository search from its first page" };
        session = *allBrowse;
    }
    auto cursors = session.pages[job.page - 1];
    ProviderPageResult combined;
    combined.status = OperationStatus::Complete;
    unsigned completed = 0;
    bool usedCache = false;
    std::string warnings;
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        unsigned page = cursors[i];
        if (page == 0) continue;
        for (;;)
        {
            if (control.status() != OperationStatus::Complete) return interrupted (control);
            CachedTransport cached (*transport, cacheRoot, job.offlineOnly);
            auto result = browseProvider (cached, sources[i].id, page, job.query, control);
            if (control.status() != OperationStatus::Complete) return interrupted (control);
            if (result.status == OperationStatus::Cancelled || result.status == OperationStatus::Paused)
                return { result.status, result.error };
            if (! result)
            {
                if (! warnings.empty()) warnings += '\n';
                warnings += sources[i].name + ": " + rateLimitError (result.error, result.rateLimit);
                cursors[i] = page;
                break;
            }
            cached.commit();
            usedCache = usedCache || cached.usedCache;
            cursors[i] = result.nextPage;
            // GitHub filters its alphabetical listing locally, so an empty
            // matching page need not mean the search has no more instruments.
            if (! job.query.empty() && result.items.empty() && result.nextPage != 0)
            {
                page = result.nextPage;
                continue;
            }
            for (auto& item : result.items) combined.items.push_back (std::move (item));
            ++completed;
            break;
        }
    }
    if (completed == 0)
        return { OperationStatus::Failed, warnings.empty() ? "No repositories could be searched" : warnings };
    if (job.page < 200 && std::any_of (cursors.begin(), cursors.end(), [] (unsigned page) { return page != 0; }))
        combined.nextPage = job.page + 1;
    session.pages.resize (job.page);
    if (combined.nextPage != 0) session.pages.push_back (std::move (cursors));
    const std::lock_guard<std::mutex> lock (mutex);
    if (control.status() != OperationStatus::Complete) return interrupted (control);
    if (current.generation != job.generation) return { OperationStatus::Cancelled, {} };
    allBrowse = std::move (session);
    current.page = std::move (combined);
    current.cachedResponse = usedCache;
    return { OperationStatus::Complete, warnings.empty() ? std::string()
        : "Some repositories could not be searched:\n" + warnings };
}

OperationResult LibraryService::execute (Job& job, const OperationControl& control)
{
    if (job.kind == JobKind::Cleanup)
    {
        cleanInstall (job);
        return { OperationStatus::Cancelled, {} };
    }
    if (control.status() != OperationStatus::Complete) return interrupted (control);
    if (job.kind == JobKind::ChangeLocation)
    {
        setState (job.generation, LibraryState::ChangingLocation);
        auto changed = changeLibraryLocation (locations, job.destination, job.moveExisting, control, configurationFile);
        if (! changed || ! changed.locations) return { changed.status, changed.error, changed.failureKind };
        locations = std::move (*changed.locations);
        store = PackStore (locations.active.root, locations.active.marker);
        const auto installed = installedLibraryPacks (locations);
        const std::lock_guard<std::mutex> lock (mutex);
        if (current.generation == job.generation)
        {
            current.locations = locations;
            current.installed = installed;
        }
        return { OperationStatus::Complete, changed.error };
    }
    if (job.kind == JobKind::Installed && ! configurationFile.empty())
    {
        locations = readLibraryLocations (configurationFile);
        store = PackStore (locations.active.root, locations.active.marker);
        const std::lock_guard<std::mutex> lock (mutex);
        if (current.generation == job.generation) current.locations = locations;
    }
    if (job.kind == JobKind::Browse && job.sourceId == kAllRepositoriesSourceId)
    {
        setState (job.generation, LibraryState::Browsing);
        return browseAll (job, control);
    }
    if (job.kind == JobKind::Browse || job.kind == JobKind::Resolve)
    {
        CachedTransport cached (*transport, cacheRoot, job.offlineOnly);
        if (job.kind == JobKind::Browse)
        {
            setState (job.generation, LibraryState::Browsing);
            auto result = browseProvider (cached, job.sourceId, job.page, job.query, control);
            if (result) cached.commit();
            while (result && ! job.query.empty() && result.items.empty() && result.nextPage != 0)
            {
                if (control.status() != OperationStatus::Complete) return interrupted (control);
                result = browseProvider (cached, job.sourceId, result.nextPage, job.query, control);
                if (result) cached.commit();
            }
            const std::lock_guard<std::mutex> lock (mutex);
            if (current.generation == job.generation)
            {
                if (result) current.page = result;
                current.cachedResponse = cached.usedCache;
            }
            return { result.status, result ? result.error : rateLimitError (result.error, result.rateLimit) };
        }
        setState (job.generation, LibraryState::Resolving);
        auto result = resolveProviderPack (cached, job.summary, control);
        if (result) cached.commit();
        const std::lock_guard<std::mutex> lock (mutex);
        if (current.generation == job.generation)
        {
            if (result) current.pack = std::move (result.pack);
            current.cachedResponse = cached.usedCache;
        }
        return { result.status, result ? result.error : rateLimitError (result.error, result.rateLimit) };
    }
    if (job.kind == JobKind::Install)
    {
        std::string error;
        std::error_code nativeError;
        if (! store.initialize (error, &nativeError))
            return { OperationStatus::Failed, error, storageFailureKind (nativeError) };
        setState (job.generation, LibraryState::Downloading);
        if (! plainDirectory (job.part.parent_path(), nativeError))
            return { OperationStatus::Failed, "Cannot open the SFZ download directory", storageFailureKind (nativeError) };
        DirectoryLease next;
        if (! next.acquire (job.part.parent_path(), DirectoryLease::Access::Shared, error, &nativeError))
            return { OperationStatus::Failed, error, storageFailureKind (nativeError) };
        job.partLease = std::move (next);
        const auto downloaded = downloadFile (*transport,
            { job.pack.downloadUrl, job.part, job.pack.expectedSha256, ArchiveLimits{}.maxArchiveBytes }, control);
        job.preserveDownload = downloaded.preservePart || static_cast<bool> (downloaded);
        if (! downloaded) return { downloaded.status, downloaded.error, downloaded.failureKind };
        if (control.status() != OperationStatus::Complete) return interrupted (control);
        setState (job.generation, LibraryState::Extracting);
        job.staging = store.createStaging (error, &nativeError);
        if (job.staging.empty()) return { OperationStatus::Failed, error, storageFailureKind (nativeError) };
        ArchiveRequest request;
        request.archive = job.part;
        request.stagingDirectory = job.staging;
        request.expectedRoot = job.pack.expectedRoot;
        request.licensePath = job.pack.license.path;
        if (! request.licensePath.empty()) request.licenseSha256 = job.pack.license.sha256;
        for (const auto& instrument : job.pack.instruments)
            request.expectedEntrypoints.push_back (instrument.relativePath);
        const auto extracted = extractZip (request, control);
        if (! extracted)
        {
            if (extracted.status == OperationStatus::Failed && extracted.failureKind != FailureKind::DiskFull)
                job.preserveDownload = false;
            std::error_code ignored;
            fs::remove_all (dusk::fs::nativeIOPath (job.staging), ignored);
            job.staging.clear();
            return { extracted.status, extracted.error, extracted.failureKind };
        }
        if (control.status() != OperationStatus::Complete) return interrupted (control);
        const auto published = store.publish (job.pack, downloaded.sha256, job.staging, extracted.sfzPaths);
        if (! published) return { published.status, published.error, published.failureKind };
        cleanInstall (job);
        const auto installed = installedLibraryPacks (locations);
        const std::lock_guard<std::mutex> lock (mutex);
        if (current.generation == job.generation)
        {
            current.installed = installed;
            current.pack = job.pack;
        }
        return { OperationStatus::Complete, {} };
    }
    if (job.kind == JobKind::Remove)
    {
        if (job.installed.instruments.empty()) return { OperationStatus::Failed, "Invalid installed SFZ pack" };
        const auto key = PackStore::identity (job.installed.locatorFor (job.installed.instruments.front()));
        const auto installed = installedLibraryPacks (locations);
        if (std::none_of (installed.begin(), installed.end(), [&] (const auto& pack) { return pack.directory == job.installed.directory; }))
            return { OperationStatus::Failed, "The selected pack is outside the registered SFZ libraries" };
        auto registered = locations.previous;
        registered.insert (registered.begin(), locations.active);
        for (const auto& location : registered)
        {
            if (! libraryLocationAvailable (location)) continue;
            PackStore owner (location.root, location.marker);
            for (const auto& pack : owner.installed())
                if (! pack.instruments.empty() && PackStore::identity (pack.locatorFor (pack.instruments.front())) == key)
                {
                    const auto removed = owner.remove (pack, job.inUse);
                    if (! removed) return removed;
                }
        }
    }
    const auto installed = installedLibraryPacks (locations);
    const std::lock_guard<std::mutex> lock (mutex);
    if (current.generation == job.generation) current.installed = installed;
    if (! locations.error.empty()) return { OperationStatus::Failed, locations.error };
    if (! libraryLocationAvailable (locations.active))
        return { OperationStatus::Failed, "The SFZ library drive is unavailable. Reconnect it or choose another folder in Settings." };
    return { OperationStatus::Complete, {} };
}

void LibraryService::run()
{
#if defined(_WIN32)
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    setpriority (PRIO_PROCESS, 0, 10);
#endif
    std::string initializationError = locations.error;
    const auto sweepLocation = [] (const LibraryLocation& location)
    {
        sweepLibraryStaging (location);
        if (! libraryLocationAvailable (location)) return;
        sweepWorkDirectory (location.root / ".repository-cache");
        sweepWorkDirectory (location.root / ".downloads");
    };
    if (initializationError.empty())
    {
        sweepLocation (locations.active);
        for (const auto& previous : locations.previous) sweepLocation (previous);
    }
    std::error_code initializationCode;
    const bool initialized = initializationError.empty() && store.initialize (initializationError, &initializationCode);
    auto initializationKind = storageFailureKind (initializationCode);
    bool cacheReady = false;
    DirectoryLease repositoryLease;
    if (initialized || ! configurationFile.empty())
    {
        std::error_code error;
        fs::create_directories (dusk::fs::nativeIOPath (cacheRoot.parent_path()), error);
        cacheReady = ! error && plainDirectory (cacheRoot, error);
        if (cacheReady)
        {
            const auto canonical = fs::canonical (dusk::fs::nativeIOPath (cacheRoot), error);
            cacheReady = ! error;
            if (cacheReady) cacheRoot = canonical;
        }
        if (cacheReady)
        {
            sweepWorkDirectory (cacheRoot);
            std::string ownershipError;
            cacheReady = repositoryLease.acquire (cacheRoot, DirectoryLease::Access::Shared, ownershipError, &error);
        }
        if (! cacheReady)
        {
            initializationError = "Cannot open the SFZ repository cache directory";
            initializationKind = storageFailureKind (error);
        }
    }
    const auto installed = installedLibraryPacks (locations);
    {
        const std::lock_guard<std::mutex> lock (mutex);
        current.installed = installed;
        current.locations = locations;
        current.busy = false;
        if (! initialized || ! cacheReady)
        {
            current.state = LibraryState::Failed;
            current.error = initializationError;
            current.failureKind = initializationKind;
        }
    }
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait (lock, [this] { return pending || stopping.load (std::memory_order_acquire); });
            if (stopping.load (std::memory_order_acquire))
            {
                if (paused) { job = std::move (*paused); paused.reset(); }
                else if (pending) { job = std::move (*pending); pending.reset(); }
                lock.unlock();
                cleanInstall (job, cancelRequested.load (std::memory_order_acquire) || job.kind == JobKind::Cleanup);
                break;
            }
            job = std::move (*pending);
            pending.reset();
        }
        OperationControl control;
        control.cancelled = [this]
        { return stopping.load (std::memory_order_acquire) || cancelRequested.load (std::memory_order_acquire); };
        control.paused = [this] { return heavyWork.load (std::memory_order_acquire); };
        control.progress = [this, generation = job.generation] (const OperationProgress& progress)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (current.generation == generation) current.progress = progress;
        };
        OperationResult result;
        try
        {
            DirectoryLease nextRepository;
            std::string ownershipError;
            std::error_code ownershipCode;
            const bool usesRepository = job.kind == JobKind::Browse || job.kind == JobKind::Resolve;
            if (cacheReady && usesRepository
                && ! nextRepository.acquire (cacheRoot, DirectoryLease::Access::Shared, ownershipError, &ownershipCode))
                result = { OperationStatus::Failed, ownershipError, storageFailureKind (ownershipCode) };
            else
            {
                if (nextRepository) repositoryLease = std::move (nextRepository);
                result = cacheReady || job.kind == JobKind::ChangeLocation
                    ? execute (job, control)
                    : OperationResult { OperationStatus::Failed, initializationError, initializationKind };
            }
        }
        catch (const fs::filesystem_error& exception)
        {
            result = { OperationStatus::Failed, std::string ("SFZ library operation failed: ") + exception.what(),
                       storageFailureKind (exception.code()) };
        }
        catch (const std::exception& exception)
        {
            result = { OperationStatus::Failed, std::string ("SFZ library operation failed: ") + exception.what() };
        }
        if (result.status != OperationStatus::Complete) cleanInstall (job, false);
        {
            std::unique_lock<std::mutex> lock (mutex);
            if (result.status != OperationStatus::Complete)
            {
                const bool cancelled = cancelRequested.load (std::memory_order_acquire);
                const bool shuttingDown = stopping.load (std::memory_order_acquire);
                if (cancelled || (result.status == OperationStatus::Paused && shuttingDown))
                    result = { OperationStatus::Cancelled, {} };
                if (! job.part.empty() && (cancelled || (result.status != OperationStatus::Paused
                                                       && ! shuttingDown && ! job.preserveDownload)))
                {
                    lock.unlock();
                    cleanInstall (job);
                    lock.lock();
                    if (cancelRequested.load (std::memory_order_acquire))
                        result = { OperationStatus::Cancelled, {} };
                }
            }
            if (current.generation == job.generation)
            {
                current.state = stateFor (result.status);
                current.error = result.error;
                current.failureKind = result.failureKind;
                current.busy = false;
                if (result.status != OperationStatus::Paused
                    && (job.kind == JobKind::ChangeLocation || job.kind == JobKind::Cleanup))
                    movingPacks.store (false, std::memory_order_release);
                if (result.status == OperationStatus::Paused) paused = std::move (job);
            }
        }
    }
}
} // namespace duskstudio::sfz
