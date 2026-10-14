#pragma once

#include "SfzArchive.h"
#include "SfzPackStore.h"

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
enum class LibraryState
{
    Idle, Browsing, Resolving, Downloading, Extracting, Paused, Complete, Failed, Cancelled
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
};

class LibraryService
{
public:
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
    bool setTransportForTest (std::shared_ptr<HttpTransport>);
    void setHeavyWork (bool active) noexcept { heavyWork.store (active, std::memory_order_release); }
    LibrarySnapshot snapshot() const;

private:
    enum class JobKind { Browse, Resolve, Install, Installed, Remove, Cleanup };
    struct Job
    {
        JobKind kind = JobKind::Installed;
        std::uint64_t generation = 0;
        std::string sourceId, query;
        unsigned page = 1;
        bool offlineOnly = false;
        ProviderSummary summary;
        ProviderPack pack;
        InstalledPack installed;
        std::vector<ManagedSfzLocator> inUse;
        std::filesystem::path part, staging;
    };

    bool enqueue (Job);
    void run();
    OperationResult execute (Job&, const OperationControl&);
    void setState (std::uint64_t generation, LibraryState);
    void cleanInstall (Job&) noexcept;

    PackStore store;
    std::shared_ptr<HttpTransport> transport;
    std::filesystem::path cacheRoot;
    mutable std::mutex mutex;
    std::condition_variable wake;
    LibrarySnapshot current;
    std::optional<Job> pending;
    std::optional<Job> paused;
    std::atomic<bool> stopping { false }, cancelRequested { false }, heavyWork { false };
    std::thread worker;
};
} // namespace duskstudio::sfz
