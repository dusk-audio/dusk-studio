#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "FileWriter.h"

namespace dusk::audio
{
// Real-time-safe front end for FileWriter. The audio thread push()es blocks
// into a lock-free interleaved ring; an owned worker thread drains the ring to
// disk. push() never allocates or blocks; on overflow it drops the block and
// returns false, matching the disk-can't-keep-up contract of the recorder.
class ThreadedFileWriter
{
public:
    ThreadedFileWriter (std::unique_ptr<FileWriter> writer, int fifoFrames);
    ~ThreadedFileWriter();

    ThreadedFileWriter (const ThreadedFileWriter&)            = delete;
    ThreadedFileWriter& operator= (const ThreadedFileWriter&) = delete;

    // False once construction got a null FileWriter (a failed create) or the
    // disk write has failed — callers must check this before trusting push().
    bool isValid() const noexcept { return writer != nullptr && ! writeFailed.load (std::memory_order_acquire); }

    // Audio thread. Deinterleaved src, numCh pointers of >= numFrames. Returns
    // false when the block is dropped: the ring lacks room, the writer is null,
    // or a prior disk write failed. A false must not be read as "persisted".
    bool push (const float* const* src, int numCh, int64_t numFrames) noexcept;

private:
    void run();
    int64_t used (int64_t w, int64_t r) const noexcept { return (w - r + capacityFrames) % capacityFrames; }

    std::unique_ptr<FileWriter> writer;
    const int                   channels;
    const int64_t               capacityFrames;   // one frame kept free to tell full from empty
    std::vector<float>          ring;              // interleaved, capacityFrames * channels

    std::atomic<int64_t> writePos    { 0 };
    std::atomic<int64_t> readPos     { 0 };
    std::atomic<bool>    stop        { false };
    std::atomic<bool>    writeFailed { false };
    std::thread          worker;
};
} // namespace dusk::audio
