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

    // Audio thread. Deinterleaved src, numCh pointers of >= numFrames.
    // Returns false (block dropped) if the ring lacks room.
    bool push (const float* const* src, int numCh, int64_t numFrames) noexcept;

private:
    void run();
    int64_t used (int64_t w, int64_t r) const noexcept { return (w - r + capacityFrames) % capacityFrames; }

    std::unique_ptr<FileWriter> writer;
    const int                   channels;
    const int64_t               capacityFrames;   // one frame kept free to tell full from empty
    std::vector<float>          ring;              // interleaved, capacityFrames * channels

    std::atomic<int64_t> writePos { 0 };
    std::atomic<int64_t> readPos  { 0 };
    std::atomic<bool>    stop     { false };
    std::thread          worker;
};
} // namespace dusk::audio
