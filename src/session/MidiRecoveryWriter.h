#pragma once

#include "MidiRecovery.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
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
} // namespace duskstudio::midirecovery
