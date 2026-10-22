#pragma once

#include "BufferedFileReader.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dusk::audio
{
class ReaderFillPool
{
public:
    struct Stats
    {
        std::size_t workers = 0;
        std::size_t readers = 0;
        std::size_t residentReaders = 0;
        std::size_t allocatedBytes = 0;
        std::uint64_t readMisses = 0;
        std::uint64_t fillPasses = 0;
        std::uint64_t fillErrors = 0;
        std::uint64_t workerWakeups = 0;
    };

    explicit ReaderFillPool (unsigned workers = 3, bool startWorkers = true);
    ~ReaderFillPool();
    ReaderFillPool (const ReaderFillPool&) = delete;
    ReaderFillPool& operator= (const ReaderFillPool&) = delete;

    // Non-RT. Readers must use Manual fill; remove fences any in-flight fill.
    void configure (double sampleRate, double aheadSeconds = 3.0,
                     double behindSeconds = 2.0);
    void add (BufferedFileReader&, std::int64_t timelineStart,
              std::int64_t timelineEnd, std::int64_t sourceOffset);
    void remove (BufferedFileReader&) noexcept;
    void reset() noexcept;
    // Updates the pool position after priming; transport writers must be quiescent.
    void primeAt (std::int64_t timelineFrame);
    Stats stats() const;

    // RT: the workers poll these hints; publishing them never signals an event.
    void setPositionRt (std::int64_t timelineFrame) noexcept;
    void setSeekTargetRt (std::int64_t timelineFrame, int warmFrames) noexcept;
    void clearSeekTargetRt() noexcept;
    bool seekReadyRt (std::int64_t timelineFrame, int warmFrames) const noexcept;
    // One serialized producer, normally the audio callback.
    void setLoopRt (bool enabled, std::int64_t start, std::int64_t end) noexcept;

    // One bounded pass, also used by deterministic no-thread tests.
    bool pumpOnce();

private:
    struct Entry
    {
        BufferedFileReader* reader = nullptr;
        std::int64_t timelineStart = 0;
        std::int64_t timelineEnd = 0;
        std::int64_t sourceOffset = 0;
        bool claimed = false;
        bool removed = false;
        std::chrono::steady_clock::time_point retryAfter {};
        std::uint64_t lastService = 0;
    };

    struct Loop
    {
        bool enabled = false;
        std::int64_t start = 0;
        std::int64_t end = 0;
        bool coherent = true;
    };

    struct Claim
    {
        Entry* entry = nullptr;
        std::int64_t sourceFrame = 0;
        bool evict = false;
        bool seeking = false;
        std::int64_t currentSourceFrame = -1;
    };

    struct Seek
    {
        std::int64_t target = -1;
        int warmFrames = 0;
        std::uint64_t generation = 0;
        bool coherent = true;
    };

    Loop loopSnapshot() const noexcept;
    Seek seekSnapshot() const noexcept;
    void publishSeekRt (std::int64_t, int) noexcept;
    void updateSeekReady();
    bool eligible (const Entry&, std::int64_t position, const Loop&,
                   std::int64_t& sourceFrame) const noexcept;
    Claim claimNext();
    void finishClaim (Entry&, bool failed = false) noexcept;
    void run();

    std::mutex controlMutex;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<std::thread> workers;
    bool resetting = false;
    bool priming = false;
    std::uint64_t serviceSequence = 0;

    std::atomic<bool> stop { false };
    std::atomic<std::int64_t> position { 0 };
    std::atomic<std::size_t> registeredReaders { 0 };
    std::atomic<std::uint64_t> seekGeneration { 0 }, seekReadyGeneration { ~std::uint64_t { 0 } };
    std::atomic<std::int64_t> seekTarget { -1 };
    std::atomic<int> seekWarmFrames { 0 };
    std::atomic<std::int64_t> aheadFrames { 144000 };
    std::atomic<std::int64_t> behindFrames { 96000 };
    std::atomic<std::uint64_t> loopGeneration { 0 };
    std::atomic<bool> loopEnabled { false };
    std::atomic<std::int64_t> loopStart { 0 };
    std::atomic<std::int64_t> loopEnd { 0 };
    std::atomic<std::uint64_t> fillPasses { 0 };
    std::atomic<std::uint64_t> fillErrors { 0 };
    std::atomic<std::uint64_t> workerWakeups { 0 };
};
} // namespace dusk::audio
