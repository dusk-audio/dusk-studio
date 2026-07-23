#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "../../foundation/AutoResetEvent.h"

namespace dusk::audio
{
class ThreadedFileWriter;

// One disk thread draining many externally-drained ThreadedFileWriter rings, so
// a 24-track record or an N-stem bounce costs one thread instead of N. The audio
// thread never touches the pool - it only pushes to each writer's SPSC ring. The
// pool thread and the message-thread add()/remove() calls share `m`; the audio
// thread does not, so holding `m` across a drain pass never blocks the RT path.
class WriterDrainPool
{
public:
    explicit WriterDrainPool (int maxWritersIn);
    ~WriterDrainPool();

    WriterDrainPool (const WriterDrainPool&)            = delete;
    WriterDrainPool& operator= (const WriterDrainPool&) = delete;

    // Message thread. Registers a writer built in ThreadedFileWriter::Drain::
    // External mode. False if the fixed registry is full. The pool does not own
    // the writer.
    bool add (ThreadedFileWriter* w);

    // Message thread. Drains the writer to empty, flushes it, then drops it from
    // the pool. PRECONDITION: the audio thread has stopped pushing to this
    // writer, else the drain never reaches empty. The writer may be destroyed
    // once this returns.
    void remove (ThreadedFileWriter* w);

private:
    void run();

    const std::size_t             maxWriters;
    std::mutex                    m;
    std::vector<ThreadedFileWriter*> writers;   // guarded by m
    std::atomic<bool>             stop { false };
    AutoResetEvent                wake;
    std::thread                   worker;
};
} // namespace dusk::audio
