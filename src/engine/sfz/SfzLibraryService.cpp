#include "SfzLibraryService.h"
#include "SfzCatalogJson.h"
#include "../../foundation/Fs.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <fstream>
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

std::string digest (std::string_view text)
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, crypto_hash_sha256_BYTES> bytes {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256 (bytes.data(), reinterpret_cast<const unsigned char*> (text.data()), text.size());
    sodium_bin2hex (hex.data(), hex.size(), bytes.data(), bytes.size());
    return hex.data();
}

bool plainDirectory (const fs::path& path)
{
    std::error_code error;
    fs::create_directory (path, error);
    error.clear();
    return fs::is_directory (fs::symlink_status (path, error)) && ! error;
}

bool atomicCacheWrite (const fs::path& target, const std::string& text)
{
    if (sodium_init() < 0) return false;
    std::array<unsigned char, 16> random {};
    std::array<char, 33> name {};
    randombytes_buf (random.data(), random.size());
    sodium_bin2hex (name.data(), name.size(), random.data(), random.size());
    const auto temporary = target.parent_path() / (std::string (".cache-") + name.data());
    if (! dusk::fs::writeStringToFile (temporary, text))
    {
        std::error_code ignored;
        fs::remove (temporary, ignored);
        return false;
    }
    std::error_code error;
#if defined(_WIN32)
    const bool replaced = MoveFileExW (temporary.c_str(), target.c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    fs::rename (temporary, target, error);
    const bool replaced = ! error;
#endif
    if (! replaced) fs::remove (temporary, error);
    return replaced;
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
        const auto path = cache / (key + ".json");
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
            if (dusk::json::getInt (json, "schema", 0) != 1
                || dusk::json::getString (json, "request_url") != request.url
                || responseHeaders == json.end() || ! responseHeaders->is_object()
                || content == json.end() || ! content->is_string()
                || content->get_ref<const std::string&>().size() > kMaxCacheBody)
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
        HttpResponse response;
        std::string content;
        const auto result = upstream.perform (request,
            [&] (const HttpResponse& value) { response = value; return headers (value); },
            [&] (std::string_view bytes)
            {
                if (bytes.size() > kMaxCacheBody - content.size()) return false;
                content.append (bytes.data(), bytes.size());
                return body (bytes);
            }, control);
        if (result && response.statusCode == 200)
        {
            Json entry { { "schema", 1 }, { "request_url", request.url },
                         { "response_url", response.url }, { "headers", response.headers },
                         { "body", std::move (content) } };
            updates.emplace_back (path, entry.dump());
        }
        return result;
    }

    void commit()
    {
        for (const auto& update : updates) atomicCacheWrite (update.first, update.second);
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

LibraryService::LibraryService (fs::path root, std::shared_ptr<HttpTransport> supplied)
    : store (std::move (root)), transport (supplied ? std::move (supplied) : std::make_shared<CurlTransport>()),
      worker ([this] { run(); }) {}

LibraryService::~LibraryService()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping.store (true, std::memory_order_release);
        cancelRequested.store (true, std::memory_order_release);
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
    return true;
}

bool LibraryService::enqueue (Job job)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (current.busy || paused || stopping.load (std::memory_order_acquire)) return false;
        job.generation = ++current.generation;
        current.busy = true;
        current.error.clear();
        current.cachedResponse = false;
        current.progress = {};
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

void LibraryService::cleanInstall (Job& job) noexcept
{
    std::error_code error;
    if (! job.staging.empty()) fs::remove_all (job.staging, error);
    job.staging.clear();
    if (! job.part.empty())
    {
        fs::remove (job.part, error);
        auto sidecar = job.part;
        sidecar += ".resume.json";
        fs::remove (sidecar, error);
    }
    job.part.clear();
}

OperationResult LibraryService::execute (Job& job, const OperationControl& control)
{
    if (job.kind == JobKind::Cleanup)
    {
        cleanInstall (job);
        return { OperationStatus::Cancelled, {} };
    }
    if (control.status() != OperationStatus::Complete) return interrupted (control);
    if (job.kind == JobKind::Browse || job.kind == JobKind::Resolve)
    {
        CachedTransport cached (*transport, cacheRoot, job.offlineOnly);
        if (job.kind == JobKind::Browse)
        {
            setState (job.generation, LibraryState::Browsing);
            auto result = browseProvider (cached, job.sourceId, job.page, job.query, control);
            if (result) cached.commit();
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
        setState (job.generation, LibraryState::Downloading);
        if (job.part.empty())
        {
            const auto key = digest (providerPackToJson (job.pack));
            if (key.empty()) return { OperationStatus::Failed, "Cannot identify SFZ download" };
            job.part = cacheRoot / (key + ".part");
        }
        const auto downloaded = downloadFile (*transport,
            { job.pack.downloadUrl, job.part, job.pack.expectedSha256 }, control);
        if (! downloaded) return { downloaded.status, downloaded.error };
        if (control.status() != OperationStatus::Complete) return interrupted (control);
        setState (job.generation, LibraryState::Extracting);
        std::string error;
        job.staging = store.createStaging (error);
        if (job.staging.empty()) return { OperationStatus::Failed, error };
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
            std::error_code ignored;
            fs::remove_all (job.staging, ignored);
            job.staging.clear();
            return { extracted.status, extracted.error };
        }
        if (control.status() != OperationStatus::Complete) return interrupted (control);
        const auto published = store.publish (job.pack, downloaded.sha256, job.staging, extracted.sfzPaths);
        if (! published) return { published.status, published.error };
        cleanInstall (job);
        const auto installed = store.installed();
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
        const auto removed = store.remove (job.installed, job.inUse);
        if (! removed) return removed;
    }
    const auto installed = store.installed();
    const std::lock_guard<std::mutex> lock (mutex);
    if (current.generation == job.generation) current.installed = installed;
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
    std::string initializationError;
    const bool initialized = store.initialize (initializationError);
    bool cacheReady = false;
    if (initialized)
    {
        cacheRoot = store.root() / ".repository-cache";
        cacheReady = plainDirectory (cacheRoot);
        if (! cacheReady) initializationError = "Cannot open the SFZ repository cache directory";
    }
    const auto installed = initialized ? store.installed() : std::vector<InstalledPack> {};
    {
        const std::lock_guard<std::mutex> lock (mutex);
        current.installed = installed;
        current.busy = false;
        if (! initialized || ! cacheReady)
        {
            current.state = LibraryState::Failed;
            current.error = initializationError;
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
                cleanInstall (job);
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
            result = initialized && cacheReady
                ? execute (job, control)
                : OperationResult { OperationStatus::Failed, initializationError };
        }
        catch (const std::exception& exception)
        {
            result = { OperationStatus::Failed, std::string ("SFZ library operation failed: ") + exception.what() };
        }
        if (result.status == OperationStatus::Paused)
        {
            std::error_code ignored;
            if (! job.staging.empty()) fs::remove_all (job.staging, ignored);
            job.staging.clear();
        }
        else if (result.status != OperationStatus::Complete) cleanInstall (job);
        {
            std::unique_lock<std::mutex> lock (mutex);
            if (result.status == OperationStatus::Paused
                && (cancelRequested.load (std::memory_order_acquire)
                    || stopping.load (std::memory_order_acquire)))
            {
                result = { OperationStatus::Cancelled, {} };
                lock.unlock();
                cleanInstall (job);
                lock.lock();
            }
            if (current.generation == job.generation)
            {
                current.state = stateFor (result.status);
                current.error = result.error;
                current.busy = false;
                if (result.status == OperationStatus::Paused) paused = std::move (job);
            }
        }
    }
}
} // namespace duskstudio::sfz
