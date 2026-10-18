#pragma once

#include "SfzTransfer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace duskstudio::sfz
{
struct ArtworkPixels
{
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

using ArtworkDecoder = std::shared_ptr<const ArtworkPixels> (*) (std::string_view);

struct ArtworkSnapshot
{
    std::uint64_t generation = 0;
    std::string key;
    std::shared_ptr<const ArtworkPixels> pixels;
    bool sourceImage = false;
    bool busy = false;
};

class ArtworkCache
{
public:
    ArtworkCache (std::filesystem::path root, std::shared_ptr<HttpTransport>);
    ~ArtworkCache();
    // Fallback bytes are immutable application resources and outlive this cache.
    void request (std::string key, std::string url, std::string_view fallback,
                  ArtworkDecoder, bool allowNetwork, std::string sourceId);
    ArtworkSnapshot snapshot() const;
    void cancel();
    void setTransport (std::shared_ptr<HttpTransport>);
    void setHeavyWork (bool active) noexcept { heavyWork.store (active, std::memory_order_release); }

private:
    struct Request
    {
        std::uint64_t generation;
        std::string key, url, sourceId;
        std::string_view fallback;
        ArtworkDecoder decode;
        bool allowNetwork;
    };
    void run();
    void publish (const Request&, std::shared_ptr<const ArtworkPixels>, bool sourceImage);
    std::filesystem::path root;
    std::shared_ptr<HttpTransport> transport;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    ArtworkSnapshot current;
    std::atomic<std::uint64_t> generation { 0 };
    std::atomic<bool> stopping { false }, heavyWork { false };
    std::thread worker;
};
} // namespace duskstudio::sfz
