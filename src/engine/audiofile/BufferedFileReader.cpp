#include "BufferedFileReader.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dusk::audio
{
namespace
{
constexpr int kActiveWaitMs = 5;
constexpr int kIdleWaitMs = 100;

std::int64_t spanEnd (std::int64_t start, std::int64_t count) noexcept
{
    return start > std::numeric_limits<std::int64_t>::max() - count
         ? std::numeric_limits<std::int64_t>::max() : start + count;
}
} // namespace

BufferedFileReader::BufferedFileReader (std::unique_ptr<FileReader> src,
                                        std::int64_t windowFrames, Fill fill)
    : source (std::move (src)),
      fileInfo (source != nullptr ? source->info() : FileInfo {}),
      channels (fileInfo.numChannels),
      capacity (std::max<std::int64_t> (2, windowFrames)),
      fillMode (fill)
{
    if (fillMode == Fill::Background && channels > 0 && fileInfo.numFrames > 0)
    {
        beginFill (0);
        worker = std::thread ([this] { run(); });
    }
}

BufferedFileReader::~BufferedFileReader()
{
    stop.store (true, std::memory_order_release);
    wake.signal();
    if (worker.joinable()) worker.join();
    releaseResidency();
}

const BufferedFileReader::Window* BufferedFileReader::acquireWindow() const noexcept
{
    // A lease precedes the pointer load in the same total order as retirement.
    // Therefore any reader that can see the retired pointer is counted before
    // the worker's post-exchange drain; later leases can see only its successor.
    activeReads.fetch_add (1, std::memory_order_seq_cst);
    return publishedWindow.load (std::memory_order_seq_cst);
}

void BufferedFileReader::releaseWindow() const noexcept
{
    activeReads.fetch_sub (1, std::memory_order_release);
}

void BufferedFileReader::hintRt (std::int64_t startFrame) noexcept
{
    hintFrame.store (std::max<std::int64_t> (0, startFrame), std::memory_order_release);
}

std::int64_t BufferedFileReader::hint() const noexcept
{
    return hintFrame.load (std::memory_order_acquire);
}

void BufferedFileReader::prefetch (std::int64_t startFrame) noexcept
{
    hintRt (startFrame);
    if (fillMode == Fill::Background) wake.signal();
}

bool BufferedFileReader::holds (std::int64_t startFrame, std::int64_t numFrames) const noexcept
{
    if (channels <= 0 || numFrames <= 0) return true;
    const auto from = std::max<std::int64_t> (0, startFrame);
    const auto end = std::min (spanEnd (startFrame, numFrames), fileInfo.numFrames);
    if (from >= end) return true;
    const auto* window = acquireWindow();
    const bool held = window != nullptr && window->startFrame <= from
                    && window->startFrame + window->numFrames >= end;
    releaseWindow();
    return held;
}

bool BufferedFileReader::needsFillAt (const Window* window, std::int64_t h) const noexcept
{
    if (channels <= 0 || h >= fileInfo.numFrames
        || failedHint.load (std::memory_order_acquire) == h)
        return false;
    if (window == nullptr || h < window->startFrame
        || h >= window->startFrame + window->numFrames)
        return true;
    const auto end = window->startFrame + window->numFrames;
    return end < fileInfo.numFrames
        && end - h <= (capacity - 1) / 2;
}

bool BufferedFileReader::needsFill() const noexcept
{
    const auto* window = acquireWindow();
    const bool needed = needsFillAt (window, hint());
    releaseWindow();
    return needed;
}

bool BufferedFileReader::hasResidency() const noexcept
{
    return publishedWindow.load (std::memory_order_acquire) != nullptr;
}

std::size_t BufferedFileReader::allocatedBytes() const noexcept
{
    return allocated.load (std::memory_order_relaxed);
}

std::uint64_t BufferedFileReader::readMisses() const noexcept
{
    return misses.load (std::memory_order_relaxed);
}

void BufferedFileReader::updateAllocatedBytes() noexcept
{
    std::size_t bytes = fillDest.capacity() * sizeof (float*);
    if (currentWindow != nullptr) bytes += currentWindow->data.capacity() * sizeof (float);
    if (stagingWindow != nullptr) bytes += stagingWindow->data.capacity() * sizeof (float);
    if (source != nullptr) bytes += source->readScratchBytes();
    allocated.store (bytes, std::memory_order_relaxed);
}

void BufferedFileReader::beginFill (std::int64_t startFrame)
{
    auto next = std::make_unique<Window>();
    next->startFrame = startFrame;
    next->numFrames = std::min (capacity - 1, fileInfo.numFrames - startFrame);
    if ((std::uint64_t) next->numFrames > std::numeric_limits<std::size_t>::max()
                                                  / sizeof (float) / (std::size_t) channels)
        throw std::length_error ("audio read window is too large");
    next->data.resize ((std::size_t) next->numFrames * (std::size_t) channels);
    fillDest.resize ((std::size_t) channels);
    stagingWindow = std::move (next);
    stagingFilled = 0;
    updateAllocatedBytes();
}

void BufferedFileReader::publishStaging()
{
    auto retired = std::move (currentWindow);
    currentWindow = std::move (stagingWindow);
    publishedWindow.exchange (currentWindow.get(), std::memory_order_seq_cst);
    while (activeReads.load (std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    retired.reset();
    stagingFilled = 0;
    updateAllocatedBytes();
}

bool BufferedFileReader::fillOnce()
{
    if (channels <= 0 || source == nullptr) return false;
    const auto h = std::clamp (hint(), (std::int64_t) 0, fileInfo.numFrames);
    if (h >= fileInfo.numFrames)
    {
        stagingWindow.reset();
        updateAllocatedBytes();
        return false;
    }
    if (stagingWindow != nullptr
        && (h < stagingWindow->startFrame
            || h >= stagingWindow->startFrame + stagingWindow->numFrames))
    {
        stagingWindow.reset();
        stagingFilled = 0;
        updateAllocatedBytes();
    }
    if (stagingWindow == nullptr)
    {
        if (! needsFillAt (currentWindow.get(), h)) return false;
        beginFill (h);
    }

    const auto stride = stagingWindow->numFrames;
    const auto chunk = std::min (kFillChunkFrames, stride - stagingFilled);
    for (int c = 0; c < channels; ++c)
        fillDest[(std::size_t) c] = stagingWindow->data.data()
                               + (std::size_t) c * (std::size_t) stride
                               + (std::size_t) stagingFilled;
    const auto got = source->read (fillDest.data(), channels,
                                   stagingWindow->startFrame + stagingFilled, chunk);
    stagingFilled += got;
    updateAllocatedBytes();
    if (got < chunk)
    {
        failedHint.store (h, std::memory_order_release);
        // A short disk read cannot publish the unwritten suffix as resident.
        // Keep the previous complete snapshot instead of changing its contract.
        stagingWindow.reset();
        stagingFilled = 0;
        updateAllocatedBytes();
        return false;
    }
    failedHint.store (-1, std::memory_order_release);
    if (stagingFilled == stride) publishStaging();
    return true;
}

void BufferedFileReader::fillNow()
{
    while (fillOnce()) {}
}

void BufferedFileReader::releaseResidency() noexcept
{
    publishedWindow.exchange (nullptr, std::memory_order_seq_cst);
    while (activeReads.load (std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    currentWindow.reset();
    stagingWindow.reset();
    stagingFilled = 0;
    std::vector<float*>().swap (fillDest);
    if (source != nullptr) source->releaseReadScratch();
    failedHint.store (-1, std::memory_order_release);
    updateAllocatedBytes();
}

void BufferedFileReader::run()
{
    std::int64_t lastHint = -1;
    while (! stop.load (std::memory_order_acquire))
    {
        try
        {
            if (fillOnce()) continue;
        }
        catch (...)
        {
            return;
        }
        const auto h = hint();
        const bool moving = h != lastHint;
        lastHint = h;
        wake.wait (moving ? kActiveWaitMs : kIdleWaitMs);
    }
}

bool BufferedFileReader::readRt (float* const* dest, int numDestCh,
                                 std::int64_t startFrame, std::int64_t numFrames) noexcept
{
    if (dest == nullptr || numDestCh <= 0 || numFrames <= 0) return true;
    hintRt (startFrame);
    const auto clearAll = [&]
    {
        for (int c = 0; c < numDestCh; ++c)
            std::fill (dest[c], dest[c] + numFrames, 0.0f);
    };

    const auto wantedStart = std::max<std::int64_t> (0, startFrame);
    const auto wantedEnd = std::min (spanEnd (startFrame, numFrames), fileInfo.numFrames);
    if (wantedStart >= wantedEnd)
    {
        clearAll();
        return true;
    }

    const auto* window = acquireWindow();
    bool complete = false;
    if (window != nullptr)
    {
        const auto from = std::max (wantedStart, window->startFrame);
        const auto to = std::min (wantedEnd, window->startFrame + window->numFrames);
        if (to > from)
        {
            const auto destinationOffset = from - startFrame;
            const auto sourceOffset = from - window->startFrame;
            const auto have = to - from;
            for (int c = 0; c < numDestCh; ++c)
            {
                if (c < channels)
                {
                    std::fill (dest[c], dest[c] + destinationOffset, 0.0f);
                    std::memcpy (dest[c] + destinationOffset,
                                 window->data.data()
                                    + (std::size_t) c * (std::size_t) window->numFrames
                                    + (std::size_t) sourceOffset,
                                 (std::size_t) have * sizeof (float));
                    std::fill (dest[c] + destinationOffset + have, dest[c] + numFrames, 0.0f);
                }
                else
                    std::fill (dest[c], dest[c] + numFrames, 0.0f);
            }
            complete = from == wantedStart && to == wantedEnd;
        }
        else
            clearAll();
    }
    else
        clearAll();
    releaseWindow();
    if (! complete) misses.fetch_add (1, std::memory_order_relaxed);
    return complete;
}
} // namespace dusk::audio
