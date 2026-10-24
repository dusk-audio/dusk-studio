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
constexpr auto kRetryDelay = std::chrono::milliseconds (100);

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
        worker = std::thread ([this] { run(); });
}

BufferedFileReader::~BufferedFileReader()
{
    stop.store (true, std::memory_order_release);
    wake.signal();
    if (worker.joinable()) worker.join();
    releaseResidency();
}

void BufferedFileReader::setFillMode (Fill next)
{
    if (fillMode == next) return;
    stop.store (true, std::memory_order_release);
    wake.signal();
    if (worker.joinable()) worker.join();
    fillMode = next;
    stop.store (false, std::memory_order_release);
    if (next == Fill::Background && channels > 0 && fileInfo.numFrames > 0)
        worker = std::thread ([this] { run(); });
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
    const bool held = windowHolds (windowForRange (window, from, end), from, end);
    releaseWindow();
    return held;
}

std::int64_t BufferedFileReader::filledFrames (const Window* window) noexcept
{
    return window == nullptr ? 0 : window->filled.load (std::memory_order_acquire);
}

bool BufferedFileReader::windowHolds (const Window* window, std::int64_t from, std::int64_t end) noexcept
{
    return window != nullptr && window->startFrame <= from && window->startFrame + filledFrames (window) >= end;
}

bool BufferedFileReader::grows (const Window* window, std::int64_t from, std::int64_t end) noexcept
{
    if (window == nullptr) return false;
    const auto filled = window->filled.load (std::memory_order_relaxed);
    return filled < window->planned && window->startFrame <= from
        && from <= window->startFrame + filled && end <= window->startFrame + window->planned;
}

const BufferedFileReader::Window* BufferedFileReader::windowForRange (
    const Window* current, std::int64_t from, std::int64_t end) const noexcept
{
    if (windowHolds (current, from, end)) return current;
    const auto* seek = publishedSeekWindow.load (std::memory_order_seq_cst);
    // Publishing a new seek may first retain the old seek in the current slot.
    const auto* latestCurrent = publishedWindow.load (std::memory_order_seq_cst);
    const auto overlap = [from, end] (const Window* window)
    {
        return window == nullptr ? std::int64_t { 0 }
            : std::max<std::int64_t> (0, std::min (end, window->startFrame + filledFrames (window))
                                          - std::max (from, window->startFrame));
    };
    const auto* best = overlap (seek) > overlap (current) ? seek : current;
    return overlap (latestCurrent) > overlap (best) ? latestCurrent : best;
}

bool BufferedFileReader::holdsSeek (std::int64_t startFrame, std::int64_t numFrames) const noexcept
{
    if (channels <= 0 || numFrames <= 0) return true;
    const auto from = std::max<std::int64_t> (0, startFrame);
    const auto end = std::min (spanEnd (startFrame, numFrames), fileInfo.numFrames);
    if (from >= end) return true;
    acquireWindow();
    const bool held = windowHolds (publishedSeekWindow.load (std::memory_order_seq_cst), from, end);
    releaseWindow();
    return held;
}

bool BufferedFileReader::needsSeekFill (std::int64_t startFrame, std::int64_t numFrames) const noexcept
{
    return ! holdsSeek (startFrame, numFrames);
}

bool BufferedFileReader::needsFillAt (const Window* window, std::int64_t h) const noexcept
{
    if (channels <= 0 || h >= fileInfo.numFrames)
        return false;
    const auto end = window != nullptr ? window->startFrame + filledFrames (window) : 0;
    if (window == nullptr || h < window->startFrame || h >= end)
        return true;
    return end < fileInfo.numFrames
        && end - h <= (capacity - 1) / 2;
}

bool BufferedFileReader::needsFill() const noexcept
{
    const auto* window = acquireWindow();
    const auto at = hint();
    const bool needed = needsFillAt (window, at)
        && needsFillAt (publishedSeekWindow.load (std::memory_order_seq_cst), at);
    releaseWindow();
    return needed;
}

bool BufferedFileReader::hasResidency() const noexcept
{
    return publishedWindow.load (std::memory_order_acquire) != nullptr
        || publishedSeekWindow.load (std::memory_order_acquire) != nullptr;
}

bool BufferedFileReader::fillFailing() const noexcept
{
    return failing.load (std::memory_order_relaxed);
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
    if (seekWindow != nullptr) bytes += seekWindow->data.capacity() * sizeof (float);
    if (stagingWindow != nullptr) bytes += stagingWindow->data.capacity() * sizeof (float);
    if (source != nullptr) bytes += source->readScratchBytes();
    allocated.store (bytes, std::memory_order_relaxed);
}

std::unique_ptr<BufferedFileReader::Window> BufferedFileReader::makeWindow (std::int64_t startFrame)
{
    auto next = std::make_unique<Window>();
    next->startFrame = startFrame;
    next->planned = std::min (capacity - 1, fileInfo.numFrames - startFrame);
    next->stride = next->planned;
    if ((std::uint64_t) next->stride > std::numeric_limits<std::size_t>::max()
                                               / sizeof (float) / (std::size_t) channels)
        throw std::length_error ("audio read window is too large");
    next->data.resize ((std::size_t) next->stride * (std::size_t) channels);
    fillDest.resize ((std::size_t) channels);
    return next;
}

bool BufferedFileReader::readChunk (Window& window)
{
    const auto filled = window.filled.load (std::memory_order_relaxed);
    const auto chunk = std::min (kFillChunkFrames, window.planned - filled);
    if (chunk <= 0) return false;
    for (int c = 0; c < channels; ++c)
        fillDest[(std::size_t) c] = window.data.data()
                               + (std::size_t) c * (std::size_t) window.stride
                               + (std::size_t) filled;
    const auto got = std::clamp<std::int64_t> (
        source->read (fillDest.data(), channels, window.startFrame + filled, chunk), 0, chunk);
    if (got > 0) window.filled.store (filled + got, std::memory_order_release);
    if (got < chunk)
    {
        nextFillAttempt = std::chrono::steady_clock::now() + kRetryDelay;
        failing.store (true, std::memory_order_relaxed);
        return false;
    }
    failing.store (false, std::memory_order_relaxed);
    return true;
}

void BufferedFileReader::publish (std::unique_ptr<Window>& slot, std::atomic<const Window*>& published,
                                  std::unique_ptr<Window> next)
{
    auto retired = std::move (slot);
    slot = std::move (next);
    published.exchange (slot.get(), std::memory_order_seq_cst);
    while (activeReads.load (std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    retired.reset();
    updateAllocatedBytes();
}

void BufferedFileReader::retainSeekForPlayback (std::int64_t frame)
{
    const auto playing = [this] (std::int64_t at)
    {
        return at >= 0 && windowHolds (seekWindow.get(), at, spanEnd (at, 1))
            && ! windowHolds (currentWindow.get(), at, seekWindow->startFrame + filledFrames (seekWindow.get()));
    };
    // The audio thread's own reads move the hint, so it catches playback that
    // committed into this seek after the caller sampled its position.
    if (! playing (frame) && ! playing (hint())) return;
    // Playback runs in the seek window: it becomes the current window and keeps
    // growing there. Nothing is freed when the seek slot empties, and no
    // readiness rests on it: a new seek window starts only for a span it lacks.
    auto retained = std::move (seekWindow);
    publish (currentWindow, publishedWindow, std::move (retained));
    publishedSeekWindow.store (nullptr, std::memory_order_seq_cst);
}

bool BufferedFileReader::fillOnce (std::int64_t startFrame)
{
    return fillAt (startFrame >= 0 ? startFrame : hint());
}

bool BufferedFileReader::fillSeekOnce (std::int64_t target, std::int64_t currentFrame,
                                      std::int64_t numFrames)
{
    if (! needsSeekFill (target, numFrames)) return false;
    return fillSeekAt (target, currentFrame, numFrames);
}

bool BufferedFileReader::fillSeekAt (std::int64_t target, std::int64_t currentFrame, std::int64_t numFrames)
{
    if (std::chrono::steady_clock::now() < nextFillAttempt) return false;
    try
    {
        if (channels <= 0 || source == nullptr) return false;
        const auto h = std::clamp (target, (std::int64_t) 0, fileInfo.numFrames);
        if (h >= fileInfo.numFrames) return false;
        const auto end = std::min ({ spanEnd (target, numFrames), fileInfo.numFrames, spanEnd (h, capacity - 1) });
        if (grows (seekWindow.get(), h, end))
        {
            const bool full = readChunk (*seekWindow);
            updateAllocatedBytes();
            return full;
        }
        // A complete window from the target already holds all a window can.
        if (seekWindow != nullptr && seekWindow->startFrame == h
            && filledFrames (seekWindow.get()) == seekWindow->planned)
            return false;
        auto next = makeWindow (h);
        const bool full = readChunk (*next);
        if (filledFrames (next.get()) == 0)
        {
            updateAllocatedBytes();
            return false;
        }
        retainSeekForPlayback (currentFrame);
        publish (seekWindow, publishedSeekWindow, std::move (next));
        return full;
    }
    catch (...)
    {
        nextFillAttempt = std::chrono::steady_clock::now() + kRetryDelay;
        failing.store (true, std::memory_order_relaxed);
        updateAllocatedBytes();
        throw;
    }
}

bool BufferedFileReader::fillAt (std::int64_t startFrame)
{
    if (std::chrono::steady_clock::now() < nextFillAttempt) return false;
    try
    {
        if (channels <= 0 || source == nullptr) return false;
        const auto h = std::clamp (startFrame, (std::int64_t) 0, fileInfo.numFrames);
        if (h >= fileInfo.numFrames)
        {
            stagingWindow.reset();
            updateAllocatedBytes();
            return false;
        }
        // A window still growing toward h carries on, whichever slot it serves:
        // a seek committed before it was warm keeps everything it has read.
        for (auto* slot : { &seekWindow, &currentWindow })
            if (grows (slot->get(), h, h))
            {
                const bool full = readChunk (**slot);
                updateAllocatedBytes();
                return full;
            }
        if (stagingWindow != nullptr
            && (h < stagingWindow->startFrame || h >= stagingWindow->startFrame + stagingWindow->planned))
        {
            stagingWindow.reset();
            updateAllocatedBytes();
        }
        if (! windowHolds (currentWindow.get(), h, h + 1) && ! windowHolds (seekWindow.get(), h, h + 1))
        {
            // Nothing published plays h, so whatever is read for it is heard at once.
            auto next = stagingWindow != nullptr
                        && h <= stagingWindow->startFrame + filledFrames (stagingWindow.get())
                      ? std::move (stagingWindow) : makeWindow (h);
            stagingWindow.reset();
            const bool full = readChunk (*next);
            if (filledFrames (next.get()) > 0)
                publish (currentWindow, publishedWindow, std::move (next));
            updateAllocatedBytes();
            return full;
        }
        if (stagingWindow == nullptr)
        {
            if (! needsFillAt (currentWindow.get(), h) || ! needsFillAt (seekWindow.get(), h)) return false;
            stagingWindow = makeWindow (h);
        }
        const bool full = readChunk (*stagingWindow);
        const auto stagedEnd = stagingWindow->startFrame + filledFrames (stagingWindow.get());
        if (filledFrames (stagingWindow.get()) == stagingWindow->planned)
            publish (currentWindow, publishedWindow, std::move (stagingWindow));
        else if (! full)
        {
            // A short refill replaces the current window only where it reaches further.
            const auto reach = [h] (const Window* window)
            {
                return windowHolds (window, h, h + 1) ? window->startFrame + filledFrames (window) : h;
            };
            if (stagedEnd > std::max (reach (currentWindow.get()), reach (seekWindow.get())))
                publish (currentWindow, publishedWindow, std::move (stagingWindow));
            stagingWindow.reset();
        }
        updateAllocatedBytes();
        return full;
    }
    catch (...)
    {
        nextFillAttempt = std::chrono::steady_clock::now() + kRetryDelay;
        failing.store (true, std::memory_order_relaxed);
        stagingWindow.reset();
        updateAllocatedBytes();
        throw;
    }
}

void BufferedFileReader::fillNow()
{
    while (fillOnce()) {}
}

void BufferedFileReader::releaseResidency() noexcept
{
    publishedWindow.exchange (nullptr, std::memory_order_seq_cst);
    publishedSeekWindow.exchange (nullptr, std::memory_order_seq_cst);
    while (activeReads.load (std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    currentWindow.reset();
    seekWindow.reset();
    stagingWindow.reset();
    std::vector<float*>().swap (fillDest);
    if (source != nullptr) source->releaseReadScratch();
    nextFillAttempt = {};
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
            wake.wait (kIdleWaitMs);
            continue;
        }
        const auto h = hint();
        const bool moving = h != lastHint;
        lastHint = h;
        wake.wait (moving ? kActiveWaitMs : kIdleWaitMs);
    }
}

bool BufferedFileReader::readRt (float* const* dest, int numDestCh,
                                 std::int64_t startFrame, std::int64_t numFrames,
                                 ReadCoverage* coverage) noexcept
{
    if (coverage != nullptr) *coverage = {};
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

    const auto* current = acquireWindow();
    const auto* window = windowForRange (current, wantedStart, wantedEnd);
    bool complete = false;
    if (window != nullptr)
    {
        const auto from = std::max (wantedStart, window->startFrame);
        const auto to = std::min (wantedEnd, window->startFrame + filledFrames (window));
        if (to > from)
        {
            if (coverage != nullptr) *coverage = { from, to };
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
                                    + (std::size_t) c * (std::size_t) window->stride
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
