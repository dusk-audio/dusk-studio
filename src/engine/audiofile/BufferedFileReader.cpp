#include "BufferedFileReader.h"

#include <algorithm>
#include <cstring>

namespace dusk::audio
{
namespace
{
// Frames pulled from disk per pass. Large enough that a full window is a
// handful of reads, small enough that the destructor's stop flag is seen
// between them.
constexpr std::int64_t kFillChunkFrames = 8192;

// How long the worker sleeps once the window is full. Short while the read
// position is moving, because an offline render drains the window far faster
// than real time; long once it has stopped, because a session holds a reader
// per region and most of them are never read.
constexpr int kActiveWaitMs = 5;
constexpr int kIdleWaitMs   = 100;
} // namespace

BufferedFileReader::BufferedFileReader (std::unique_ptr<FileReader> src,
                                        std::int64_t windowFrames,
                                        Fill fill)
    : source (std::move (src)),
      fileInfo (source != nullptr ? source->info() : FileInfo {}),
      channels (fileInfo.numChannels),
      capacity (std::max<std::int64_t> (2, windowFrames))
{
    if (channels <= 0) return;   // unreadable file: every read stays a miss

    ring.resize ((size_t) capacity * (size_t) channels, 0.0f);
    fillDest.resize ((size_t) channels, nullptr);

    if (fill == Fill::Background)
        worker = std::thread ([this] { run(); });
}

BufferedFileReader::~BufferedFileReader()
{
    stop.store (true, std::memory_order_release);
    wake.signal();
    if (worker.joinable())
        worker.join();
}

void BufferedFileReader::prefetch (std::int64_t startFrame) noexcept
{
    hintFrame.store (std::max<std::int64_t> (0, startFrame), std::memory_order_release);
    wake.signal();
}

void BufferedFileReader::fillNow()
{
    while (fillStep()) {}
}

void BufferedFileReader::run()
{
    std::int64_t lastHint = -1;
    while (! stop.load (std::memory_order_acquire))
    {
        if (fillStep()) continue;

        const std::int64_t h = hintFrame.load (std::memory_order_acquire);
        const bool moving = (h != lastHint);
        lastHint = h;
        wake.wait (moving ? kActiveWaitMs : kIdleWaitMs);
    }
}

bool BufferedFileReader::fillStep()
{
    if (channels <= 0) return false;

    const std::int64_t h = std::clamp (hintFrame.load (std::memory_order_acquire),
                                       (std::int64_t) 0, fileInfo.numFrames);
    std::int64_t s = residentStart.load (std::memory_order_relaxed);
    std::int64_t e = residentEnd.load (std::memory_order_relaxed);

    if (h < s || h > e)
    {
        // Seek out of the window: the frames it holds are worthless and their
        // slots are about to be refilled. The odd generation covers the gap
        // where a reader has already latched the old bounds.
        generation.fetch_add (1, std::memory_order_acq_rel);
        residentStart.store (h, std::memory_order_release);
        residentEnd.store (h, std::memory_order_release);
        generation.fetch_add (1, std::memory_order_acq_rel);
        s = e = h;
    }
    else if (h > s)
    {
        // Retire played frames before the append below reuses their slots:
        // the append only writes slots holding frames below residentStart, so
        // publishing it first is what keeps the write head off anything a
        // concurrent readRt is copying.
        residentStart.store (h, std::memory_order_release);
        s = h;
    }

    const std::int64_t room = std::min (capacity - 1 - (e - s), fileInfo.numFrames - e);
    if (room <= 0) return false;

    const std::int64_t slot  = e % capacity;
    const std::int64_t chunk = std::min ({ kFillChunkFrames, room, capacity - slot });

    for (int c = 0; c < channels; ++c)
        fillDest[(size_t) c] = ring.data() + (size_t) c * (size_t) capacity + (size_t) slot;

    const std::int64_t got = source->read (fillDest.data(), channels, e, chunk);
    if (got <= 0) return false;   // read error: keep the window as it stands

    residentEnd.store (e + got, std::memory_order_release);
    return true;
}

bool BufferedFileReader::readRt (float* const* dest, int numDestCh,
                                 std::int64_t startFrame, std::int64_t numFrames) noexcept
{
    if (dest == nullptr || numDestCh <= 0 || numFrames <= 0) return true;

    hintFrame.store (std::max<std::int64_t> (0, startFrame), std::memory_order_release);

    const auto clearAll = [&]
    {
        for (int c = 0; c < numDestCh; ++c)
            std::fill (dest[c], dest[c] + numFrames, 0.0f);
    };

    const std::uint64_t gen = generation.load (std::memory_order_acquire);
    const std::int64_t  s   = residentStart.load (std::memory_order_acquire);
    const std::int64_t  e   = residentEnd.load (std::memory_order_acquire);

    const std::int64_t wantEnd = std::min (startFrame + numFrames, fileInfo.numFrames);
    const std::int64_t from    = std::max (startFrame, s);
    const std::int64_t to      = std::min (wantEnd, e);

    if ((gen & 1) != 0 || to <= from)
    {
        clearAll();
        return (gen & 1) == 0 && startFrame >= wantEnd;
    }

    const std::int64_t have   = to - from;
    const std::int64_t dstOff = from - startFrame;
    const int          copyCh = std::min (numDestCh, channels);

    for (int c = 0; c < numDestCh; ++c)
    {
        if (c >= copyCh)
        {
            std::fill (dest[c], dest[c] + numFrames, 0.0f);
            continue;
        }
        std::fill (dest[c], dest[c] + dstOff, 0.0f);
        std::fill (dest[c] + dstOff + have, dest[c] + numFrames, 0.0f);
    }

    for (std::int64_t done = 0; done < have; )
    {
        const std::int64_t slot = (from + done) % capacity;
        const std::int64_t run  = std::min (have - done, capacity - slot);
        for (int c = 0; c < copyCh; ++c)
            std::memcpy (dest[c] + dstOff + done,
                         ring.data() + (size_t) c * (size_t) capacity + (size_t) slot,
                         sizeof (float) * (size_t) run);
        done += run;
    }

    // The copy raced the fill thread if the window was thrown away or the span
    // was retired under it. The retire case needs an earlier read in the same
    // block to have published a higher position (the loop-wrap block reads
    // pre-wrap, then wrapped); dest then holds the wrong material, not a gap.
    std::atomic_thread_fence (std::memory_order_acquire);
    if (generation.load (std::memory_order_relaxed) != gen
        || residentStart.load (std::memory_order_relaxed) > from)
    {
        clearAll();
        return false;
    }

    return from == startFrame && to == wantEnd;
}
} // namespace dusk::audio
