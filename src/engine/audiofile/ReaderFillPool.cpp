#include "ReaderFillPool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace dusk::audio
{
namespace
{
std::int64_t addFrames (std::int64_t value, std::int64_t frames) noexcept
{
    return value > std::numeric_limits<std::int64_t>::max() - frames
         ? std::numeric_limits<std::int64_t>::max() : value + frames;
}

std::int64_t horizon (double rate, double seconds) noexcept
{
    if (! std::isfinite (rate) || rate <= 0.0 || ! std::isfinite (seconds) || seconds <= 0.0)
        return 0;
    const auto frames = (long double) rate * (long double) seconds;
    return frames >= (long double) std::numeric_limits<std::int64_t>::max()
         ? std::numeric_limits<std::int64_t>::max() : (std::int64_t) frames;
}
} // namespace

ReaderFillPool::ReaderFillPool (unsigned count, bool startWorkers)
{
    if (! startWorkers) return;
    count = std::clamp (count, 2u, 4u);
    workers.reserve (count);
    try
    {
        for (unsigned i = 0; i < count; ++i)
            workers.emplace_back ([this] { run(); });
    }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            stop.store (true, std::memory_order_release);
        }
        changed.notify_all();
        for (auto& worker : workers) worker.join();
        throw;
    }
}

ReaderFillPool::~ReaderFillPool()
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        stop.store (true, std::memory_order_release);
    }
    changed.notify_all();
    for (auto& worker : workers) worker.join();
    reset();
}

void ReaderFillPool::configure (double sampleRate, double aheadSeconds, double behindSeconds)
{
    aheadFrames.store (horizon (sampleRate, aheadSeconds), std::memory_order_release);
    behindFrames.store (horizon (sampleRate, behindSeconds), std::memory_order_release);
    changed.notify_all();
}

void ReaderFillPool::add (BufferedFileReader& reader, std::int64_t start,
                           std::int64_t end, std::int64_t offset)
{
    if (! reader.isManual()) throw std::invalid_argument ("fill pool requires a Manual reader");
    std::lock_guard<std::mutex> control (controlMutex);
    {
        std::lock_guard<std::mutex> lock (mutex);
        seekReadyGeneration.store (~std::uint64_t { 0 }, std::memory_order_release);
        for (auto& entry : entries)
            if (entry->reader == &reader)
                throw std::invalid_argument ("reader is already registered with fill pool");
        auto entry = std::make_unique<Entry>();
        entry->reader = &reader;
        entry->timelineStart = std::max<std::int64_t> (0, start);
        entry->timelineEnd = std::max (entry->timelineStart, end);
        entry->sourceOffset = std::max<std::int64_t> (0, offset);
        entries.push_back (std::move (entry));
        registeredReaders.store (entries.size(), std::memory_order_release);
    }
    changed.notify_all();
}

void ReaderFillPool::remove (BufferedFileReader& reader) noexcept
{
    std::lock_guard<std::mutex> control (controlMutex);
    std::unique_lock<std::mutex> lock (mutex);
    const auto found = std::find_if (entries.begin(), entries.end(),
                                    [&] (const auto& e) { return e->reader == &reader; });
    if (found == entries.end()) return;
    seekReadyGeneration.store (~std::uint64_t { 0 }, std::memory_order_release);
    auto* entry = found->get();
    entry->removed = true;
    changed.wait (lock, [&] { return ! entry->claimed; });
    entries.erase (found);
    registeredReaders.store (entries.size(), std::memory_order_release);
    lock.unlock();
    reader.releaseResidency();
}

void ReaderFillPool::reset() noexcept
{
    std::lock_guard<std::mutex> control (controlMutex);
    std::unique_lock<std::mutex> lock (mutex);
    resetting = true;
    clearSeekTargetRt();
    seekReadyGeneration.store (~std::uint64_t { 0 }, std::memory_order_release);
    changed.wait (lock, [&]
    {
        return std::none_of (entries.begin(), entries.end(),
                             [] (const auto& e) { return e->claimed; });
    });
    std::vector<std::unique_ptr<Entry>> retired;
    retired.swap (entries);
    registeredReaders.store (0, std::memory_order_release);
    lock.unlock();
    for (auto& entry : retired) entry->reader->releaseResidency();
    lock.lock();
    resetting = false;
    lock.unlock();
    changed.notify_all();
}

void ReaderFillPool::setPositionRt (std::int64_t frame) noexcept
{
    position.store (std::max<std::int64_t> (0, frame), std::memory_order_release);
}

void ReaderFillPool::publishSeekRt (std::int64_t target, int warmFrames) noexcept
{
    if (seekTarget.load (std::memory_order_relaxed) == target
        && seekWarmFrames.load (std::memory_order_relaxed) == warmFrames) return;
    seekGeneration.fetch_add (1, std::memory_order_acq_rel);
    std::atomic_thread_fence (std::memory_order_release);
    seekTarget.store (target, std::memory_order_relaxed);
    seekWarmFrames.store (warmFrames, std::memory_order_relaxed);
    seekGeneration.fetch_add (1, std::memory_order_release);
}

void ReaderFillPool::setSeekTargetRt (std::int64_t target, int warmFrames) noexcept
{
    publishSeekRt (std::max<std::int64_t> (0, target), std::max (1, warmFrames));
}

void ReaderFillPool::clearSeekTargetRt() noexcept { publishSeekRt (-1, 0); }

ReaderFillPool::Seek ReaderFillPool::seekSnapshot() const noexcept
{
    const auto generation = seekGeneration.load (std::memory_order_acquire);
    if ((generation & 1) != 0) return { -1, 0, generation, false };
    const auto target = seekTarget.load (std::memory_order_relaxed);
    const auto frames = seekWarmFrames.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    return { target, frames, generation, seekGeneration.load (std::memory_order_relaxed) == generation };
}

bool ReaderFillPool::seekReadyRt (std::int64_t target, int warmFrames) const noexcept
{
    const auto seek = seekSnapshot();
    return seek.coherent && seek.target >= 0 && seek.target == std::max<std::int64_t> (0, target)
        && seek.warmFrames >= std::max (1, warmFrames) && (registeredReaders.load (std::memory_order_acquire) == 0
            || seekReadyGeneration.load (std::memory_order_acquire) == seek.generation);
}

void ReaderFillPool::updateSeekReady()
{
    if (resetting || priming) return;
    const auto seek = seekSnapshot();
    if (! seek.coherent || seek.target < 0 || seekReadyGeneration.load (std::memory_order_relaxed) == seek.generation)
        return;
    const auto end = addFrames (seek.target, seek.warmFrames);
    for (const auto& entry : entries)
    {
        if (entry->removed) continue;
        const auto from = std::max (seek.target, entry->timelineStart);
        const auto until = std::min (end, entry->timelineEnd);
        if (until <= from) continue;
        // Fills toward this seek or for playback only add to the seek window, so
        // they cannot undo what it holds; claims on other readers do not matter.
        if (entry->claimed && (entry->claimEvicts
                               || (entry->claimSeekGeneration != 0 && entry->claimSeekGeneration != seek.generation)))
            return;
        if (! entry->reader->holdsSeek (addFrames (entry->sourceOffset, from - entry->timelineStart), until - from))
            return;
    }
    seekReadyGeneration.store (seek.generation, std::memory_order_release);
}

void ReaderFillPool::setLoopRt (bool enabled, std::int64_t start, std::int64_t end) noexcept
{
    enabled = enabled && end > start;
    start = std::max<std::int64_t> (0, start);
    if (loopEnabled.load (std::memory_order_relaxed) == enabled
        && loopStart.load (std::memory_order_relaxed) == start
        && loopEnd.load (std::memory_order_relaxed) == end)
        return;
    loopGeneration.fetch_add (1, std::memory_order_acq_rel);
    std::atomic_thread_fence (std::memory_order_release);
    loopStart.store (start, std::memory_order_relaxed);
    loopEnd.store (end, std::memory_order_relaxed);
    loopEnabled.store (enabled, std::memory_order_relaxed);
    loopGeneration.fetch_add (1, std::memory_order_release);
}

ReaderFillPool::Loop ReaderFillPool::loopSnapshot() const noexcept
{
    const auto generation = loopGeneration.load (std::memory_order_acquire);
    if ((generation & 1) != 0) return { false, 0, 0, false };
    Loop loop;
    loop.enabled = loopEnabled.load (std::memory_order_relaxed);
    loop.start = loopStart.load (std::memory_order_relaxed);
    loop.end = loopEnd.load (std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_acquire);
    if (loopGeneration.load (std::memory_order_relaxed) != generation)
        return { false, 0, 0, false };
    return loop;
}

bool ReaderFillPool::eligible (const Entry& entry, std::int64_t at, const Loop& loop,
                               std::int64_t& sourceFrame, bool* pastEnd) const noexcept
{
    if (entry.timelineEnd <= entry.timelineStart) return false;
    const auto ahead = aheadFrames.load (std::memory_order_acquire);
    const auto behind = behindFrames.load (std::memory_order_acquire);
    const auto near = [&] (std::int64_t target)
    {
        return entry.timelineStart <= addFrames (target, ahead)
            && entry.timelineEnd > std::max<std::int64_t> (0, target - behind);
    };
    auto target = at;
    if (! near (target))
    {
        if (! loop.enabled || at < loop.start || at >= loop.end
            || loop.end - at > ahead || ! near (loop.start))
            return false;
        target = loop.start;
    }
    const auto timelineFrame = std::clamp (target, entry.timelineStart, entry.timelineEnd - 1);
    sourceFrame = addFrames (entry.sourceOffset, timelineFrame - entry.timelineStart);
    if (pastEnd != nullptr) *pastEnd = target >= entry.timelineEnd;
    return true;
}

ReaderFillPool::Claim ReaderFillPool::claimNext()
{
    std::lock_guard<std::mutex> lock (mutex);
    if (resetting || priming || stop.load (std::memory_order_acquire)) return {};
    updateSeekReady();
    const auto loop = loopSnapshot();
    const auto seek = seekSnapshot();
    if (! loop.coherent || ! seek.coherent) return {};
    // Clearing the seek follows publication of its committed position on the audio owner.
    const auto at = position.load (std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    const auto behind = behindFrames.load (std::memory_order_acquire);
    Claim chosen;
    using Priority = std::tuple<int, std::int64_t, int, std::uint64_t>;
    Priority best { 3, 0, 0, 0 };
    for (auto& owned : entries)
    {
        auto& entry = *owned;
        if (entry.claimed || entry.removed) continue;
        std::int64_t sourceFrame = 0;
        const bool near = eligible (entry, at, loop, sourceFrame);
        std::int64_t seekSource = 0;
        std::int64_t seekFrames = 0;
        bool behindSeek = false;
        const auto seekFrom = std::max (seek.target, entry.timelineStart);
        const auto seekUntil = std::min (addFrames (seek.target, seek.warmFrames), entry.timelineEnd);
        const bool required = seek.target >= 0 && seekUntil > seekFrom;
        const bool nearSeek = required
            || (seek.target >= 0 && eligible (entry, seek.target, loop, seekSource, &behindSeek));
        if (required)
        {
            seekSource = addFrames (entry.sourceOffset, seekFrom - entry.timelineStart);
            seekFrames = seekUntil - seekFrom;
        }
        else if (nearSeek)
        {
            const auto timelineFrame = std::clamp (seek.target, entry.timelineStart, entry.timelineEnd - 1);
            seekFrames = std::min<std::int64_t> (seek.warmFrames, entry.timelineEnd - timelineFrame);
        }
        bool fillCurrent = false;
        if (near && ! (at >= entry.timelineEnd && entry.timelineEnd > std::max<std::int64_t> (0, at - behind)))
        {
            entry.reader->hintRt (sourceFrame);
            fillCurrent = entry.reader->needsFill();
        }
        // Playback runs forward from the target, so a region that ends before it
        // stays resident but is not filled for it, as for a committed position.
        const bool fillSeek = nearSeek && ! behindSeek && entry.reader->needsSeekFill (seekSource, seekFrames);
        const bool evict = ! near && ! nearSeek && entry.reader->allocatedBytes() != 0;
        if ((! fillCurrent && ! fillSeek && ! evict) || (! evict && now < entry.retryAfter)) continue;
        const auto target = fillCurrent || evict ? at : seek.target;
        const auto distance = target < entry.timelineStart ? entry.timelineStart - target
                            : target >= entry.timelineEnd ? target - entry.timelineEnd : 0;
        const Priority priority { fillCurrent ? 0 : fillSeek ? 1 : 2, distance,
                                  entry.reader->hasResidency() ? 1 : 0, entry.lastService };
        if (priority < best)
        {
            best = priority;
            const auto currentSource = near ? sourceFrame : std::int64_t { -1 };
            chosen = { &entry, fillCurrent ? sourceFrame : seekSource, evict,
                       ! fillCurrent && fillSeek, currentSource, seekFrames };
        }
    }
    if (chosen.entry != nullptr)
    {
        chosen.entry->claimed = true;
        chosen.entry->claimEvicts = chosen.evict;
        chosen.entry->claimSeekGeneration = chosen.seeking ? seek.generation : 0;
        chosen.entry->lastService = ++serviceSequence;
    }
    return chosen;
}

void ReaderFillPool::finishClaim (Entry& entry, bool failed) noexcept
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        entry.retryAfter = failed ? std::chrono::steady_clock::now() + std::chrono::milliseconds (100)
                                  : std::chrono::steady_clock::time_point {};
        entry.claimed = false;
        entry.claimEvicts = false;
        entry.claimSeekGeneration = 0;
        updateSeekReady();
    }
    changed.notify_all();
}

bool ReaderFillPool::pumpOnce()
{
    const auto claim = claimNext();
    if (claim.entry == nullptr) return false;
    bool failed = false;
    try
    {
        if (claim.evict) claim.entry->reader->releaseResidency();
        else
        {
            const auto filled = claim.seeking
                ? claim.entry->reader->fillSeekOnce (claim.sourceFrame, claim.currentSourceFrame, claim.numFrames)
                : claim.entry->reader->fillOnce (claim.sourceFrame);
            if (filled)
                fillPasses.fetch_add (1, std::memory_order_relaxed);
            else
                failed = claim.seeking ? claim.entry->reader->needsSeekFill (claim.sourceFrame, claim.numFrames)
                                      : claim.entry->reader->needsFill();
        }
    }
    catch (...)
    {
        failed = true;
        fillErrors.fetch_add (1, std::memory_order_relaxed);
    }
    finishClaim (*claim.entry, failed);
    return true;
}

void ReaderFillPool::settleForTest()
{
    for (;;)
    {
        if (pumpOnce()) continue;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (std::none_of (entries.begin(), entries.end(), [] (const auto& e) { return e->claimed; }))
                return;
        }
        std::this_thread::yield();
    }
}

void ReaderFillPool::primeAt (std::int64_t frame)
{
    std::lock_guard<std::mutex> control (controlMutex);
    const auto at = std::max<std::int64_t> (0, frame);
    const auto loop = loopSnapshot();
    std::vector<Claim> claims;
    std::unique_lock<std::mutex> lock (mutex);
    claims.reserve (entries.size());
    priming = true;
    clearSeekTargetRt();
    seekReadyGeneration.store (~std::uint64_t { 0 }, std::memory_order_release);
    changed.wait (lock, [&]
    {
        return std::none_of (entries.begin(), entries.end(),
                             [] (const auto& e) { return e->claimed; });
    });
    for (auto& entry : entries)
    {
        std::int64_t sourceFrame = 0;
        const bool near = eligible (*entry, at, loop, sourceFrame);
        if (! entry->removed && (near || entry->reader->allocatedBytes() != 0))
        {
            claims.push_back ({ entry.get(), sourceFrame, ! near });
            entry->claimed = true;
            entry->claimEvicts = ! near;
        }
    }
    lock.unlock();
    for (const auto& claim : claims)
    {
        bool failed = false;
        try
        {
            if (claim.evict) claim.entry->reader->releaseResidency();
            else
            {
                claim.entry->reader->hintRt (claim.sourceFrame);
                while (claim.entry->reader->fillOnce())
                    fillPasses.fetch_add (1, std::memory_order_relaxed);
            }
        }
        catch (...)
        {
            failed = true;
            fillErrors.fetch_add (1, std::memory_order_relaxed);
        }
        finishClaim (*claim.entry, failed);
    }
    lock.lock();
    position.store (at, std::memory_order_release);
    priming = false;
    lock.unlock();
    changed.notify_all();
}

ReaderFillPool::Stats ReaderFillPool::stats() const
{
    Stats result;
    std::lock_guard<std::mutex> lock (mutex);
    result.workers = workers.size();
    result.readers = entries.size();
    for (const auto& entry : entries)
    {
        result.residentReaders += entry->reader->hasResidency() ? 1u : 0u;
        result.allocatedBytes += entry->reader->allocatedBytes();
        result.readMisses += entry->reader->readMisses();
    }
    result.fillPasses = fillPasses.load (std::memory_order_relaxed);
    result.fillErrors = fillErrors.load (std::memory_order_relaxed);
    result.workerWakeups = workerWakeups.load (std::memory_order_relaxed);
    return result;
}

void ReaderFillPool::run()
{
    while (! stop.load (std::memory_order_acquire))
    {
        if (pumpOnce()) continue;
        std::unique_lock<std::mutex> lock (mutex);
        if (entries.empty())
            changed.wait (lock, [&] { return stop.load (std::memory_order_acquire) || ! entries.empty(); });
        else
            changed.wait_for (lock, std::chrono::milliseconds (10));
        workerWakeups.fetch_add (1, std::memory_order_relaxed);
    }
}
} // namespace dusk::audio
