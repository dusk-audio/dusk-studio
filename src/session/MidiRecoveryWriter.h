#pragma once

#include "MidiRecovery.h"
#include "../engine/MidiCaptureStream.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>

namespace duskstudio::midirecovery
{
// Writes MIDI recovery files on a thread of its own, one at a time and in the
// order they were queued, so a commit, a save or an autosave does not wait for
// the disk. Only flush() waits: Save As calls it before copying midi/, and
// quitting before plug-ins are torn down. Each atomic write syncs the disk,
// which can take milliseconds a file, and a long loop take queues one file a
// pass. The thread starts with the first file queued, so a writer that is
// never given one costs no thread.
class Writer
{
public:
    // Called with each file once it is written or has failed, on the thread
    // that wrote it. What it throws is dropped. It must not call flush(): that
    // waits for the file being reported.
    using Report = std::function<void (const PendingFile&, const WriteResult&)>;

    explicit Writer (Report report = {});
    // Writes every file still queued, then stops the thread.
    ~Writer();

    Writer (const Writer&)            = delete;
    Writer& operator= (const Writer&) = delete;

    // Any thread. When the thread cannot be started, the file is written on
    // the calling thread before this returns.
    void queue (PendingFile file) noexcept;

    // Any thread but the report's. Returns once every file queued before the
    // call is written or has failed, and at once when there is none.
    void flush();

private:
    void run() noexcept;
    void write (const PendingFile& file) noexcept;

    const Report            report;
    std::mutex              mutex;
    std::condition_variable wake;               // the thread waits here for a file or the stop
    std::condition_variable progress;           // flush waits here for the files before it
    std::deque<PendingFile> files;              // guarded by mutex
    std::uint64_t           queued   = 0;       // guarded by mutex
    std::uint64_t           finished = 0;       // guarded by mutex
    bool                    stopping = false;   // guarded by mutex
    std::thread             worker;             // started under mutex
    // Held across each write, so a file written on a caller's thread never
    // races the thread for a name.
    std::mutex              writing;
};

class Journal
{
public:
    struct Config
    {
        std::filesystem::path sessionDir;
        std::chrono::system_clock::time_point made;
        int trackNumber = 0;
        std::string name;
        Timing timing;
        std::int64_t timelineStart = 0;
        std::int64_t loopLength = 0;
    };
    struct Handle
    {
        std::uint64_t id = 0;
        MidiCaptureStream::DrainView latest;
        std::uint64_t dropped = 0;
    };

    explicit Journal (Writer::Report report = {},
                      std::size_t capacity = 4 * MidiCaptureStream::kDefaultCapacity);
    ~Journal();

    // Message thread. Zero means the worker could not be started; no disk fallback.
    Handle begin (Config config) noexcept;
    // Bounded, allocation-free copy from the sole capture consumer. Never waits.
    bool append (Handle& handle, const MidiCaptureStream::DrainView& drain) noexcept;
    bool finish (const Handle& handle, int passOrdinal, std::int64_t endSample) noexcept;
    // The worker retires this pass's checkpoint only after a durable final write.
    bool commit (const PendingFile& file, std::uint64_t id, int passOrdinal) noexcept;
    // Message thread. Forces checkpoints and waits for preceding work. True
    // means the selected folder has no active decoder or unwritten pass and
    // stays unchanged until another message-thread submission. Empty selects all.
    bool flush (const std::filesystem::path& folder = {});

private:
    struct Impl;
    Writer::Report report;
    std::size_t capacity;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::midirecovery
