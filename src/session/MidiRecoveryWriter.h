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

// Keeps each pass of a MIDI take being recorded in a playable checkpoint,
// midi/recording/<take>_pass<N>.recording.mid, until its final file is on the
// disk. One worker thread decodes what the capture drains hand it and writes
// the checkpoints. Everything but the destructor runs on the message thread,
// the capture's only consumer.
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
    };

    explicit Journal (Writer::Report report = {},
                      std::size_t capacity = 4 * MidiCaptureStream::kDefaultCapacity);
    ~Journal();

    // Zero means the worker could not be started; no disk fallback.
    Handle begin (Config config) noexcept;
    // Bounded, allocation-free copy of one drain. Never waits on the worker.
    // A drain that only moves the capture position is kept back, as its newest
    // position, until a drain with more in it or the next checkpoint request
    // takes it along. False when the drain did not fit: its events are left out
    // of the checkpoints, held notes end where the last drain that fit left
    // them, and the next drain that fits reports the gap.
    bool append (const Handle& handle, const MidiCaptureStream::DrainView& drain) noexcept;
    // Once the worker has decoded every drain appended before this, it writes
    // each current pass whose notes or controllers changed since its checkpoint
    // was written and retries the passes still unwritten. A journal whose
    // writes keep failing sits out a growing number of requests. The recorder
    // asks about once a second.
    void checkpoint() noexcept;
    bool finish (const Handle& handle, int passOrdinal, std::int64_t endSample) noexcept;
    // The worker retires this pass's checkpoint only after a durable final write.
    bool commit (const PendingFile& file, std::uint64_t id, int passOrdinal) noexcept;
    // Waits for everything handed over before it, then for a checkpoint round
    // that retries every journal. True when no take is still recording into
    // the selected folder; empty selects all. A pass whose checkpoint could
    // not be written does not count: it goes on retrying in its folder. The
    // session being saved holds such a pass only while a region keeps it; a
    // loop pass older than its region's previous takes is held nowhere else.
    bool flush (const std::filesystem::path& folder = {});

    // Runs on the worker each time it has found nothing to do, just before it
    // sleeps. Set before the first begin.
    void setIdleHookForTest (std::function<void()> hook) { idleHook = std::move (hook); }

private:
    struct Impl;
    Writer::Report report;
    std::size_t capacity;
    std::function<void()> idleHook;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::midirecovery
