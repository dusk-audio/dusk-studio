#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "IFileWriteSink.h"

namespace dusk::audio
{
// Real-time-safe front end for a file write sink. The audio thread push()es
// blocks into a lock-free interleaved ring; the ring drains to disk either from
// an owned worker thread (Drain::Internal) or from an external WriterDrainPool
// that calls drainOnce() (Drain::External). push() never allocates or blocks;
// on overflow it drops the block and returns false, matching the
// disk-can't-keep-up contract of the recorder.
class ThreadedFileWriter
{
public:
    enum class Drain
    {
        Internal,   // owned worker thread drains the ring
        External    // no thread; a WriterDrainPool calls drainOnce()
    };

    ThreadedFileWriter (std::unique_ptr<IFileWriteSink> writer, int fifoFrames,
                        Drain drain = Drain::Internal);
    ~ThreadedFileWriter();

    ThreadedFileWriter (const ThreadedFileWriter&)            = delete;
    ThreadedFileWriter& operator= (const ThreadedFileWriter&) = delete;

    // False once construction got a null sink (a failed create) or the disk
    // write has failed - callers must check this before trusting push().
    bool isValid() const noexcept { return writer != nullptr && ! writeFailed.load (std::memory_order_acquire); }

    // Audio thread. Deinterleaved src, numCh pointers of >= numFrames. Returns
    // false when the block is dropped: the ring lacks room, the writer is null,
    // or a prior disk write failed. A false must not be read as "persisted".
    bool push (const float* const* src, int numCh, int64_t numFrames) noexcept;

    // Disk/pool thread only. Writes the one contiguous ring run currently ready
    // and returns the frames written; 0 means the ring was empty or the sink
    // has failed. Both the internal worker and the pool drive the file through
    // this. NOT called by the audio thread.
    int64_t drainOnce() noexcept;

    // Pool thread only, after draining to empty: flush the sink if it hasn't
    // failed. Drain::Internal flushes from its own worker instead.
    void finalizeExternal();

private:
    void run();
    int64_t used (int64_t w, int64_t r) const noexcept { return (w - r + capacityFrames) % capacityFrames; }

    std::unique_ptr<IFileWriteSink> writer;
    const int                       channels;
    const int64_t                   capacityFrames;   // one frame kept free to tell full from empty
    std::vector<float>              ring;              // interleaved, capacityFrames * channels

    std::atomic<int64_t> writePos    { 0 };
    std::atomic<int64_t> readPos     { 0 };
    std::atomic<bool>    stop        { false };
    std::atomic<bool>    writeFailed { false };
    std::thread          worker;
};
} // namespace dusk::audio
