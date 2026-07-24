#include "ThreadedFileWriter.h"

#include <algorithm>
#include <chrono>

namespace dusk::audio
{
ThreadedFileWriter::ThreadedFileWriter (std::unique_ptr<IFileWriteSink> w, int fifoFrames,
                                        Drain drain)
    : writer (std::move (w)),
      channels (writer != nullptr ? writer->numChannels() : 0),
      capacityFrames ((int64_t) std::max (2, fifoFrames) + 1)
{
    ring.resize ((size_t) capacityFrames * (size_t) channels, 0.0f);
    if (writer != nullptr && drain == Drain::Internal)
        worker = std::thread ([this] { run(); });
}

ThreadedFileWriter::~ThreadedFileWriter()
{
    stop.store (true, std::memory_order_release);
    if (worker.joinable())
    {
        worker.join();
        // run() flushes once on normal drain-to-empty (and skips flush on
        // failure); join() guarantees it has finished, so no flush here.
        return;
    }
    // Drain::External with no thread: the owning pool normally drains and
    // flushes via remove() first, leaving this a no-op. If it did not, drain
    // the remainder single-threaded here so a dropped remove() never loses a
    // clean take. Precondition either way: no producer or pool is still
    // touching this writer.
    while (drainOnce() > 0) {}
    finalizeExternal();
}

bool ThreadedFileWriter::push (const float* const* src, int numCh, int64_t numFrames) noexcept
{
    if (numFrames <= 0)
        return true;
    if (! isValid())
        return false;   // null writer or a failed disk write - never report success

    const int64_t w = writePos.load (std::memory_order_relaxed);
    const int64_t r = readPos.load (std::memory_order_acquire);
    const int64_t freeFrames = capacityFrames - 1 - used (w, r);
    if (numFrames > freeFrames)
        return false;

    for (int64_t f = 0; f < numFrames; ++f)
    {
        const size_t base = (size_t) ((w + f) % capacityFrames) * (size_t) channels;
        for (int c = 0; c < channels; ++c)
            ring[base + (size_t) c] = (c < numCh) ? src[c][f] : 0.0f;
    }

    writePos.store ((w + numFrames) % capacityFrames, std::memory_order_release);
    return true;
}

int64_t ThreadedFileWriter::drainOnce() noexcept
{
    if (writer == nullptr || writeFailed.load (std::memory_order_acquire))
        return 0;

    const int64_t r = readPos.load (std::memory_order_relaxed);
    const int64_t w = writePos.load (std::memory_order_acquire);
    const int64_t avail = used (w, r);
    if (avail == 0)
        return 0;

    // Write the contiguous run up to the ring wrap; the next pass takes the rest.
    const int64_t chunk = std::min (avail, capacityFrames - r);
    if (! writer->writeInterleaved (&ring[(size_t) r * (size_t) channels], chunk))
    {
        // Disk write failed/short: leave readPos so nothing is treated as
        // persisted and flag the failure so push() starts rejecting.
        writeFailed.store (true, std::memory_order_release);
        return 0;
    }
    readPos.store ((r + chunk) % capacityFrames, std::memory_order_release);
    return chunk;
}

void ThreadedFileWriter::finalizeExternal()
{
    if (writer == nullptr || writeFailed.load (std::memory_order_acquire))
        return;
    if (! writer->flush())
        writeFailed.store (true, std::memory_order_release);
}

void ThreadedFileWriter::run()
{
    using namespace std::chrono_literals;
    for (;;)
    {
        if (drainOnce() > 0)
            continue;   // more may be ready; keep draining before sleeping
        if (writeFailed.load (std::memory_order_acquire))
            break;
        if (stop.load (std::memory_order_acquire))
        {
            // Stop wins only once the ring is empty, so a block pushed just
            // before stop is still written.
            const int64_t r = readPos.load (std::memory_order_relaxed);
            const int64_t w = writePos.load (std::memory_order_acquire);
            if (used (w, r) == 0)
                break;
            continue;
        }
        std::this_thread::sleep_for (2ms);
    }
    if (! writeFailed.load (std::memory_order_acquire))
        writer->flush();
}
} // namespace dusk::audio
