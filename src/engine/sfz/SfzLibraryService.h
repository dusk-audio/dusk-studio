#pragma once

#include "SfzArchive.h"
#include "SfzArtworkCache.h"
#include "SfzPackStore.h"
#include "SfzLibraryLocations.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace duskstudio::sfz
{
inline constexpr const char* kAllRepositoriesSourceId = "all";

enum class LibraryState
{
    Idle, Browsing, Resolving, Downloading, Extracting, ChangingLocation, Paused, Complete, Failed, Cancelled
};

struct LibrarySnapshot
{
    LibraryState state = LibraryState::Idle;
    std::uint64_t generation = 0;
    bool busy = true;
    bool cachedResponse = false;
    OperationProgress progress;
    std::string error;
    ProviderPageResult page;
    std::optional<ProviderPack> pack;
    std::vector<InstalledPack> installed;
    LibraryLocations locations;
};

class LibraryService
{
public:
    LibraryService();
    explicit LibraryService (std::filesystem::path libraryRoot,
                             std::shared_ptr<HttpTransport> transport = {});
    ~LibraryService();
    LibraryService (const LibraryService&) = delete;
    LibraryService& operator= (const LibraryService&) = delete;

    bool browse (std::string sourceId, unsigned page = 1, std::string query = {},
                 bool offlineOnly = false);
    bool resolve (ProviderSummary, bool offlineOnly = false);
    bool install (ProviderPack);
    bool resume();
    void cancel();
    bool requestInstalled();
    bool remove (InstalledPack, std::vector<ManagedSfzLocator> inUse);
    bool changeLocation (std::filesystem::path destination, bool moveExisting);
    bool isMovingPacks() const noexcept { return movingPacks.load (std::memory_order_acquire); }
    bool setTransportForTest (std::shared_ptr<HttpTransport>);
    void setHeavyWork (bool active) noexcept
    {
        heavyWork.store (active, std::memory_order_release);
        artworkCache.setHeavyWork (active);
    }
    ArtworkCache& artwork() noexcept { return artworkCache; }
    LibrarySnapshot snapshot() const;

private:
    LibraryService (LibraryLocations, std::filesystem::path configurationFile, std::shared_ptr<HttpTransport>);
    enum class JobKind { Browse, Resolve, Install, Installed, Remove, Cleanup, ChangeLocation };
    struct Job
    {
        JobKind kind = JobKind::Installed;
        std::uint64_t generation = 0;
        std::string sourceId, query;
        unsigned page = 1;
        bool offlineOnly = false;
        bool moveExisting = false;
        ProviderSummary summary;
        ProviderPack pack;
        InstalledPack installed;
        std::vector<ManagedSfzLocator> inUse;
        std::filesystem::path part, staging, destination;
    };

    struct BrowseSession
    {
        std::string query;
        bool offlineOnly = false;
        std::vector<std::vector<unsigned>> pages;
    };

    bool enqueue (Job);
    void run();
    OperationResult execute (Job&, const OperationControl&);
    OperationResult browseAll (const Job&, const OperationControl&);
    void setState (std::uint64_t generation, LibraryState);
    void cleanInstall (Job&) noexcept;

    PackStore store;
    std::shared_ptr<HttpTransport> transport;
    LibraryLocations locations;
    std::filesystem::path configurationFile;
    std::filesystem::path cacheRoot;
    ArtworkCache artworkCache;
    mutable std::mutex mutex;
    std::condition_variable wake;
    LibrarySnapshot current;
    std::optional<Job> pending;
    std::optional<Job> paused;
    std::optional<BrowseSession> allBrowse;
    std::atomic<bool> stopping { false }, cancelRequested { false }, heavyWork { false };
    std::atomic<bool> movingPacks { false };
    std::thread worker;
};
} // namespace duskstudio::sfz
