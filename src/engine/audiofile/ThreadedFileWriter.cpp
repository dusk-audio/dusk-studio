#include "ThreadedFileWriter.h"

#include <algorithm>
#include <chrono>

namespace dusk::audio
{
ThreadedFileWriter::ThreadedFileWriter (std::unique_ptr<FileWriter> w, int numChannels, int fifoFrames)
    : writer (std::move (w)),
      channels (numChannels),
      capacityFrames ((int64_t) std::max (2, fifoFrames) + 1)
{
    ring.resize ((size_t) capacityFrames * (size_t) channels, 0.0f);
    worker = std::thread ([this] { run(); });
}

ThreadedFileWriter::~ThreadedFileWriter()
{
    stop.store (true, std::memory_order_release);
    if (worker.joinable())
        worker.join();
    if (writer != nullptr)
        writer->flush();
}

bool ThreadedFileWriter::push (const float* const* src, int numCh, int64_t numFrames) noexcept
{
    if (numFrames <= 0)
        return true;

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

void ThreadedFileWriter::run()
{
    using namespace std::chrono_literals;
    for (;;)
    {
        const int64_t r = readPos.load (std::memory_order_relaxed);
        const int64_t w = writePos.load (std::memory_order_acquire);
        const int64_t avail = used (w, r);

        if (avail == 0)
        {
            if (stop.load (std::memory_order_acquire))
                break;
            std::this_thread::sleep_for (2ms);
            continue;
        }

        // Write the contiguous run up to the ring wrap; the next pass takes the rest.
        const int64_t chunk = std::min (avail, capacityFrames - r);
        writer->writeInterleaved (&ring[(size_t) r * (size_t) channels], chunk);
        readPos.store ((r + chunk) % capacityFrames, std::memory_order_release);
    }
    writer->flush();
}
} // namespace dusk::audio
