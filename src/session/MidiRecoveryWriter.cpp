#include "MidiRecoveryWriter.h"

#include <utility>

namespace duskstudio::midirecovery
{
Writer::Writer (Report reportIn) : report (std::move (reportIn)) {}

Writer::~Writer()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

void Writer::queue (PendingFile file) noexcept
{
    try
    {
        const std::lock_guard<std::mutex> lock (mutex);
        if (! worker.joinable())
            worker = std::thread ([this] { run(); });
        // A failed push leaves `file` as it was, for the write below.
        files.push_back (std::move (file));
        ++queued;
        wake.notify_one();
        return;
    }
    catch (...)
    {
    }
    write (file);
}

void Writer::flush()
{
    std::unique_lock<std::mutex> lock (mutex);
    const auto target = queued;
    progress.wait (lock, [this, target] { return finished >= target; });
}

void Writer::run() noexcept
{
    std::unique_lock<std::mutex> lock (mutex);
    for (;;)
    {
        wake.wait (lock, [this] { return stopping || ! files.empty(); });
        if (files.empty()) return;
        auto file = std::move (files.front());
        files.pop_front();
        lock.unlock();
        write (file);
        lock.lock();
        ++finished;
        progress.notify_all();
    }
}

void Writer::write (const PendingFile& file) noexcept
{
    try
    {
        const std::lock_guard<std::mutex> lock (writing);
        const auto result = writeFile (file);
        if (report) report (file, result);
    }
    catch (...)
    {
    }
}
} // namespace duskstudio::midirecovery
