#include "WriterDrainPool.h"

#include "ThreadedFileWriter.h"
#include "../../foundation/ThreadName.h"

#include <algorithm>
#include <utility>

namespace dusk::audio
{
WriterDrainPool::WriterDrainPool (int maxWritersIn, std::function<void()> idleHookForTest)
    : maxWriters ((std::size_t) std::max (1, maxWritersIn)), idleHook (std::move (idleHookForTest))
{
    writers.reserve (maxWriters);
    worker = std::thread ([this] { dusk::setCurrentThreadName ("DuskRecDrain"); run(); });
}

WriterDrainPool::~WriterDrainPool()
{
    stop.store (true, std::memory_order_release);
    wake.signal();
    if (worker.joinable())
        worker.join();
}

bool WriterDrainPool::add (ThreadedFileWriter* w)
{
    if (w == nullptr)
        return false;

    std::lock_guard<std::mutex> lk (m);
    // A duplicate would survive remove() as a stale entry and dangle once the
    // caller destroys the writer.
    if (std::find (writers.begin(), writers.end(), w) != writers.end())
        return false;
    if (writers.size() >= maxWriters)
        return false;
    writers.push_back (w);
    wake.signal();
    return true;
}

void WriterDrainPool::remove (ThreadedFileWriter* w)
{
    std::lock_guard<std::mutex> lk (m);
    const auto it = std::find (writers.begin(), writers.end(), w);
    if (it == writers.end())
        return;

    // Producer is stopped (precondition): drain to empty on this thread, then
    // flush, so nothing is left unwritten before the caller destroys the writer.
    while (w->drainOnce() > 0) {}
    w->finalizeExternal();
    writers.erase (it);
}

void WriterDrainPool::run()
{
    while (! stop.load (std::memory_order_acquire))
    {
        bool anyWork = false, anyWriter = false;
        {
            std::lock_guard<std::mutex> lk (m);
            anyWriter = ! writers.empty();
            for (auto* w : writers)
                if (w != nullptr && w->drainOnce() > 0)
                    anyWork = true;
        }
        if (anyWork)
            continue;
        if (anyWriter)
        {
            wake.wait (2);
            continue;
        }
        // add() signals after its writer is in, under m, and the destructor
        // after it sets stop. The event latches, so either landing since the
        // check above ends this wait at once.
        if (idleHook) idleHook();
        wake.wait();
    }
}
} // namespace dusk::audio
