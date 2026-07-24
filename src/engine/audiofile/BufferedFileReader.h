#pragma once

#include "FileReader.h"
#include "../../foundation/AutoResetEvent.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace dusk::audio
{
// Audio-thread front end for FileReader. A background thread keeps a rolling
// window of the file resident in memory; readRt() serves the audio thread from
// that window and never blocks, allocates or touches the disk. Anything not
// resident - right after a seek, or when the disk falls behind - is zero-filled
// and reported as a miss, which is the contract the playback path is built on.
//
// One file, one window (sized at construction, never reallocated), one worker.
//
// Concurrency: only the fill thread mutates the window, only readRt reads it,
// and the two are kept apart by publishing every discard before it happens.
// The fill thread retires frames up to the last position readRt published
// BEFORE it reuses their ring slots, and it brackets a seek that throws the
// whole window away in an odd `generation`. readRt re-reads both after copying
// and reports a miss if either moved, which covers the case where an earlier
// read in the same block published a higher position than the one being served
// (the loop-wrap block reads pre-wrap, then wrapped).
class BufferedFileReader
{
public:
    enum class Fill
    {
        Background,   // owned thread keeps the window resident
        Manual        // no thread; the owner drives fillNow()
    };

    static constexpr std::int64_t kDefaultWindowFrames = 96000;

    explicit BufferedFileReader (std::unique_ptr<FileReader> source,
                                 std::int64_t windowFrames = kDefaultWindowFrames,
                                 Fill fill = Fill::Background);
    ~BufferedFileReader();

    BufferedFileReader (const BufferedFileReader&)            = delete;
    BufferedFileReader& operator= (const BufferedFileReader&) = delete;

    const FileInfo& info() const noexcept { return fileInfo; }

    // Audio thread. Copies the resident part of [startFrame, startFrame +
    // numFrames) into dest, zero-filling the rest; destination channels past
    // the file's channel count are zero-filled too. Returns false when a
    // wanted frame was missing - frames past the end of the file are silence
    // by definition and don't count as a miss. Publishes startFrame as the
    // prefetch position, so the window follows playback.
    bool readRt (float* const* dest, int numDestCh,
                 std::int64_t startFrame, std::int64_t numFrames) noexcept;

    // Message thread. Points the window at startFrame and wakes the worker -
    // warms a position before the audio thread reaches it, where readRt's own
    // hint only moves the window after a miss.
    void prefetch (std::int64_t startFrame) noexcept;

    // Fills the window from the current position until it is full or at EOF,
    // on the calling thread. For Fill::Manual owners: it does the disk I/O the
    // worker would otherwise do, which is what makes residency testable.
    void fillNow();

private:
    bool fillStep();
    void run();

    std::unique_ptr<FileReader> source;     // fill thread only, once constructed
    FileInfo                    fileInfo;
    const int                   channels;
    const std::int64_t          capacity;   // frames per channel; one is left
                                            // free so the write head can never
                                            // land on a resident frame
    std::vector<float>          ring;       // channel-major: ch * capacity + frame % capacity
    std::vector<float*>         fillDest;   // FileReader::read target, fill thread only

    // Resident file frames [residentStart, residentEnd); written by the fill
    // thread only, monotonic within a generation.
    std::atomic<std::int64_t>  residentStart { 0 };
    std::atomic<std::int64_t>  residentEnd   { 0 };
    std::atomic<std::int64_t>  hintFrame     { 0 };
    std::atomic<std::uint64_t> generation    { 0 };   // odd while the window is in flux

    std::atomic<bool> stop { false };
    AutoResetEvent    wake;
    std::thread       worker;
};
} // namespace dusk::audio
