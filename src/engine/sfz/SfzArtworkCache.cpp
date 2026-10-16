#include "SfzArtworkCache.h"
#include "SfzDirectoryLease.h"
#include "SfzProvider.h"
#include "../../foundation/Fs.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif defined (__APPLE__)
 #include <pthread.h>
#elif defined (__linux__)
 #include <sys/resource.h>
#endif

namespace duskstudio::sfz
{
namespace
{
namespace fs = std::filesystem;
constexpr std::size_t kMaxImageBytes = 4 * 1024 * 1024;
constexpr std::uintmax_t kMaxDiskBytes = 64 * 1024 * 1024;

std::string imageKey (const std::string& url)
{
    if (sodium_init() < 0) return {};
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash {};
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex {};
    crypto_hash_sha256 (hash.data(), reinterpret_cast<const unsigned char*> (url.data()), url.size());
    sodium_bin2hex (hex.data(), hex.size(), hash.data(), hash.size());
    return hex.data();
}

bool validPixels (const std::shared_ptr<const ArtworkPixels>& pixels)
{
    return pixels && pixels->width > 0 && pixels->width <= 512 && pixels->height > 0 && pixels->height <= 512
        && pixels->rgba.size() == static_cast<std::size_t> (pixels->width) * static_cast<std::size_t> (pixels->height) * 4;
}

bool temporaryImageName (std::string_view name)
{
    if (name.size() < 71 || name.size() > 80 || name[64] != '.' || name.substr (name.size() - 5) != ".part"
        || ! std::all_of (name.begin(), name.begin() + 64, [] (char c)
            { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return false;
    const auto token = name.substr (65, name.size() - 70);
    std::uint32_t value = 0;
    const auto parsed = std::from_chars (token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc {} && parsed.ptr == token.data() + token.size()
        && (token.size() == 1 || token.front() != '0');
}

void sweepImageTemporaries (const fs::path& root) noexcept
{
    try
    {
        std::error_code error;
        for (fs::directory_iterator it (root, error), end; ! error && it != end; it.increment (error))
        {
            if (! temporaryImageName (it->path().filename().u8string())) continue;
            std::error_code entryError;
            if (! fs::is_regular_file (it->symlink_status (entryError)) || entryError
                || fs::hard_link_count (it->path(), entryError) != 1 || entryError) continue;
            fs::remove (it->path(), entryError);
        }
    }
    catch (...) {}
}

void trimDiskCache (const fs::path& root)
{
    std::error_code error;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::uintmax_t bytes = 0;
    for (fs::directory_iterator it (root, error), end; ! error && it != end; it.increment (error))
    {
        const auto& entry = *it;
        const auto name = entry.path().filename().u8string();
        if (name.size() != 70 || name.substr (64) != ".image"
            || ! std::all_of (name.begin(), name.begin() + 64, [] (char c)
                { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) continue;
        if (! fs::is_regular_file (entry.symlink_status (error)) || error
            || fs::hard_link_count (entry.path(), error) != 1 || error) continue;
        const auto size = entry.file_size (error);
        if (error) break;
        const auto time = entry.last_write_time (error);
        if (error) break;
        bytes += size;
        files.emplace_back (time, entry.path());
    }
    std::sort (files.begin(), files.end());
    auto count = files.size();
    for (const auto& entry : files)
    {
        if (bytes <= kMaxDiskBytes && count <= 256) break;
        const auto size = fs::file_size (entry.second, error);
        if (! error && fs::remove (entry.second, error)) { bytes -= size; --count; }
    }
}
} // namespace

ArtworkCache::ArtworkCache (fs::path directory, std::shared_ptr<HttpTransport> http)
    : root (std::move (directory)), transport (std::move (http)), worker ([this] { run(); }) {}

ArtworkCache::~ArtworkCache()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping.store (true, std::memory_order_release);
        generation.fetch_add (1, std::memory_order_acq_rel);
    }
    wake.notify_one();
    if (worker.joinable()) worker.join();
}

void ArtworkCache::request (std::string key, std::string url, std::string_view fallback,
                            ArtworkDecoder decode, bool allowNetwork, std::string sourceId)
{
    const std::lock_guard<std::mutex> lock (mutex);
    if (stopping.load (std::memory_order_acquire)) return;
    const auto next = generation.fetch_add (1, std::memory_order_acq_rel) + 1;
    current = { next, key, {}, false, true };
    pending = Request { next, std::move (key), std::move (url), std::move (sourceId), fallback, decode, allowNetwork };
    wake.notify_one();
}

void ArtworkCache::cancel()
{
    const std::lock_guard<std::mutex> lock (mutex);
    generation.fetch_add (1, std::memory_order_acq_rel);
    pending.reset();
    current.busy = false;
}

void ArtworkCache::setTransport (std::shared_ptr<HttpTransport> http)
{
    const std::lock_guard<std::mutex> lock (mutex);
    generation.fetch_add (1, std::memory_order_acq_rel);
    pending.reset();
    current.busy = false;
    transport = std::move (http);
}

ArtworkSnapshot ArtworkCache::snapshot() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return current;
}

void ArtworkCache::publish (const Request& request, std::shared_ptr<const ArtworkPixels> pixels, bool sourceImage)
{
    if (! validPixels (pixels)) return;
    const std::lock_guard<std::mutex> lock (mutex);
    if (generation.load (std::memory_order_acquire) == request.generation)
        current = { request.generation, request.key, std::move (pixels), sourceImage, true };
}

void ArtworkCache::run()
{
#if defined (_WIN32)
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined (__APPLE__)
    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
#elif defined (__linux__)
    setpriority (PRIO_PROCESS, 0, 10);
#endif
    DirectoryLease cacheLease;
    const auto acquireCache = [&] (bool sweep)
    {
        std::error_code error;
        fs::create_directories (root, error);
        if (error || ! fs::is_directory (fs::symlink_status (root, error)) || error) return false;
        const auto canonical = fs::canonical (root, error);
        if (error) return false;
        std::string ownershipError;
        if (sweep)
        {
            DirectoryLease maintenance;
            if (maintenance.acquire (canonical, DirectoryLease::Access::Exclusive, ownershipError))
                sweepImageTemporaries (canonical);
        }
        DirectoryLease next;
        if (! next.acquire (canonical, DirectoryLease::Access::Shared, ownershipError)) return false;
        cacheLease = std::move (next);
        root = canonical;
        return true;
    };
    try { acquireCache (true); } catch (...) {}
    struct CachedImage { std::shared_ptr<const ArtworkPixels> pixels; std::uint64_t used; };
    std::map<std::string, CachedImage> decoded;
    std::uint64_t useCounter = 0;
    for (;;)
    {
        Request request;
        std::shared_ptr<HttpTransport> http;
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait (lock, [&] { return stopping.load (std::memory_order_acquire) || pending.has_value(); });
            if (stopping.load (std::memory_order_acquire)) return;
            request = std::move (*pending);
            pending.reset();
            http = transport;
        }
        try
        {
            const auto finish = [&] (void*)
            {
                const std::lock_guard<std::mutex> lock (mutex);
                if (generation.load (std::memory_order_acquire) == request.generation) current.busy = false;
            };
            const std::unique_ptr<void, decltype (finish)> completed (this, finish);
            if (request.decode == nullptr) continue;
            const auto decode = [&] (const std::string& key, std::string_view bytes)
            {
                const auto found = decoded.find (key);
                if (found != decoded.end()) { found->second.used = ++useCounter; return found->second.pixels; }
                auto pixels = request.decode (bytes);
                if (validPixels (pixels))
                {
                    if (decoded.size() >= 24)
                        decoded.erase (std::min_element (decoded.begin(), decoded.end(), [] (const auto& a, const auto& b)
                        { return a.second.used < b.second.used; }));
                    decoded[key] = { pixels, ++useCounter };
                }
                return pixels;
            };
            const auto fallbackKey = "fallback:" + std::to_string (reinterpret_cast<std::uintptr_t> (request.fallback.data()));
            publish (request, decode (fallbackKey, request.fallback), false);
            const auto hosts = artworkHostsForSource (request.sourceId);
            if (request.url.size() > 2048 || ! httpsUrlOnHosts (request.url, hosts)) continue;
            const auto key = imageKey (request.url);
            if (key.empty()) continue;
            const auto memory = decoded.find (key);
            if (memory != decoded.end())
            {
                memory->second.used = ++useCounter;
                publish (request, memory->second.pixels, true);
                continue;
            }
            const OperationControl control {
                [&] { return stopping.load (std::memory_order_acquire)
                    || generation.load (std::memory_order_acquire) != request.generation; },
                [&] { return heavyWork.load (std::memory_order_acquire); }, {} };
            if (control.status() != OperationStatus::Complete) continue;
            std::error_code error;
            const bool cacheReady = acquireCache (false);
            const auto path = root / (key + ".image");
            if (cacheReady && fs::is_regular_file (fs::symlink_status (path, error)) && ! error)
            {
                const auto bytes = fs::file_size (path, error) <= kMaxImageBytes && ! error
                    ? dusk::fs::loadFileAsString (path) : std::string {};
                auto pixels = decode (key, bytes);
                if (validPixels (pixels)) { publish (request, std::move (pixels), true); continue; }
                fs::remove (path, error);
            }
            if (! request.allowNetwork || ! http || control.status() != OperationStatus::Complete) continue;
            HttpRequest transfer;
            transfer.url = request.url;
            transfer.allowedHosts = hosts;
            transfer.connectTimeoutSeconds = 2; transfer.timeoutSeconds = 5; transfer.lowSpeedSeconds = 2;
            const auto response = fetchHttpText (*http, transfer, kMaxImageBytes, control);
            if (! response || response.response.statusCode != 200 || control.status() != OperationStatus::Complete) continue;
            auto pixels = decode (key, response.body);
            if (! validPixels (pixels)) continue;
            publish (request, std::move (pixels), true);
            if (cacheReady)
            {
                const auto temporary = root / (key + "." + std::to_string (randombytes_random()) + ".part");
                const auto status = fs::symlink_status (temporary, error);
                if (error == std::errc::no_such_file_or_directory) error.clear();
                if (! error && ! fs::exists (status))
                {
                    const auto removeTemporary = [&temporary] (void*)
                    { std::error_code ignored; fs::remove (temporary, ignored); };
                    const std::unique_ptr<void, decltype (removeTemporary)> cleanup (this, removeTemporary);
                    if (dusk::fs::writeStringToFile (temporary, response.body))
                    {
                        fs::rename (temporary, path, error);
                        if (! error) trimDiskCache (root);
                    }
                }
            }
        }
        catch (...) {}
    }
}
} // namespace duskstudio::sfz
