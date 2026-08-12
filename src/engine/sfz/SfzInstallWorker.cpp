#include "SfzInstallWorker.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif defined(__APPLE__)
 #include <pthread.h>
#else
 #include <pthread.h>
 #include <sys/resource.h>
#endif

namespace duskstudio::sfz
{
namespace
{
constexpr int kGatePollMilliseconds = 250;

// Installing must never cost the studio a block. The worker runs below every
// interactive thread, so a long expansion loses the CPU to the message thread
// rather than the other way round.
void applyBackgroundPriority()
{
#if defined(_WIN32)
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
    pthread_setname_np ("Dusk-SfzInstall");
#else
    // Linux nice values are per-thread, and pid 0 means the calling thread.
    setpriority (PRIO_PROCESS, 0, 10);
    pthread_setname_np (pthread_self(), "Dusk-SfzInstall");
#endif
}
} // namespace

InstallWorker::InstallWorker (Transport& transportToUse, StoreLayout layoutToUse,
                              InstallLimits limitsToUse, InstallActivityGate* gateToUse)
    : transport (transportToUse),
      layout (std::move (layoutToUse)),
      limits (limitsToUse),
      gate (gateToUse)
{
    // Clear scratch a previous run left behind when it was killed mid-install;
    // staging only ever holds an in-flight expansion, never anything worth
    // keeping. Downloads are content-addressed and reap themselves per outcome,
    // so they are left alone here to keep a partial transfer resumable.
    std::error_code ec;
    std::filesystem::remove_all (layout.stagingDirectory(), ec);

    worker = std::thread ([this] { run(); });
}

InstallWorker::~InstallWorker()
{
    {
        // Published under the lock so the worker cannot evaluate the wait
        // predicate, miss the shutdown and then block past the notification.
        std::lock_guard<std::mutex> held (lock);
        quit.store (true, std::memory_order_relaxed);
        for (const auto& job : pending)
            job->cancelled->store (true, std::memory_order_relaxed);
        // Drop the listener under the lock, before the wake: publish() copies it
        // under the same lock, so a worker mid-job gets an empty observer and
        // cannot call back into an object whose destructor is already running.
        listener = nullptr;
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

void InstallWorker::setListener (std::function<void (const InstallJobProgress&)> newListener)
{
    std::lock_guard<std::mutex> held (lock);
    listener = std::move (newListener);
}

std::uint64_t InstallWorker::enqueue (CatalogPack pack)
{
    Job job;
    job.cancelled = std::make_shared<std::atomic<bool>> (false);
    job.progress.packId = pack.id;
    job.progress.releaseId = pack.releaseId;
    job.pack = std::move (pack);

    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> held (lock);
        id = nextJobId++;
        job.progress.id = id;
        pending.push_back (std::make_shared<Job> (std::move (job)));
    }
    wake.notify_all();
    return id;
}

void InstallWorker::cancel (std::uint64_t jobId)
{
    {
        std::lock_guard<std::mutex> held (lock);
        for (const auto& job : pending)
            if (job->progress.id == jobId)
                job->cancelled->store (true, std::memory_order_relaxed);
    }
    wake.notify_all();
}

void InstallWorker::notifyActivityChanged()
{
    {
        std::lock_guard<std::mutex> held (lock);
        activityChanged = true;
    }
    wake.notify_all();
}

std::vector<InstallJobProgress> InstallWorker::snapshot() const
{
    std::vector<InstallJobProgress> jobs;
    std::lock_guard<std::mutex> held (lock);
    jobs.reserve (pending.size());
    for (const auto& job : pending)
        jobs.push_back (job->progress);
    return jobs;
}

bool InstallWorker::gateIsClosed()
{
    return gate != nullptr && gate->shouldPauseBackgroundWork();
}

void InstallWorker::run()
{
    applyBackgroundPriority();

    const auto publish = [this] (const std::shared_ptr<Job>& job, InstallJobState state)
    {
        InstallJobProgress record;
        std::function<void (const InstallJobProgress&)> observer;
        {
            std::lock_guard<std::mutex> held (lock);
            // A job held behind the gate re-tests it on every poll; only the
            // transition is worth telling anyone about.
            if (job->progress.state == state)
                return;
            job->progress.state = state;
            record = job->progress;
            observer = listener;
        }
        // The listener runs outside the lock so it may call back in.
        if (observer)
            observer (record);
    };

    const auto finish = [this, &publish] (const std::shared_ptr<Job>& job,
                                          InstallStatus status, std::string error)
    {
        {
            std::lock_guard<std::mutex> held (lock);
            job->progress.status = status;
            job->progress.error = std::move (error);
            if (! pending.empty() && pending.front() == job)
                pending.pop_front();
        }
        publish (job, InstallJobState::finished);
    };

    // Publishes the paused state and blocks until the gate might have reopened or
    // the worker is shutting down. Returns true when it should stop. Both the
    // not-yet-started and the interrupted-download cases wait here rather than
    // re-entering installPack, so a still-closed gate cannot spin the loop.
    const auto pauseUntilActivity = [this, &publish] (const std::shared_ptr<Job>& job)
    {
        publish (job, InstallJobState::paused);
        std::unique_lock<std::mutex> held (lock);
        wake.wait_for (held, std::chrono::milliseconds (kGatePollMilliseconds),
                       [this]
                       {
                           return quit.load (std::memory_order_relaxed) || activityChanged;
                       });
        activityChanged = false;
        return quit.load (std::memory_order_relaxed);
    };

    for (;;)
    {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> held (lock);
            wake.wait (held, [this]
            {
                return quit.load (std::memory_order_relaxed) || ! pending.empty();
            });
            if (quit.load (std::memory_order_relaxed))
                return;
            job = pending.front();
        }

        if (job->cancelled->load (std::memory_order_relaxed))
        {
            finish (job, InstallStatus::cancelled, {});
            continue;
        }

        if (gateIsClosed())
        {
            if (pauseUntilActivity (job))
                return;
            continue;
        }

        publish (job, InstallJobState::running);

        InstallCallbacks callbacks;
        // Real cancellation interrupts every phase; the gate only reaches the
        // download, so a busy studio pauses a transfer but never restarts an
        // expansion already in flight.
        callbacks.isCancelled = [this, job]
        {
            return quit.load (std::memory_order_relaxed)
                || job->cancelled->load (std::memory_order_relaxed);
        };
        callbacks.isDownloadCancelled = [this, job]
        {
            return quit.load (std::memory_order_relaxed)
                || job->cancelled->load (std::memory_order_relaxed)
                || gateIsClosed();
        };
        callbacks.onProgress = [this, job] (InstallPhase phase, std::uint64_t completed,
                                            std::uint64_t total)
        {
            std::lock_guard<std::mutex> held (lock);
            job->progress.phase = phase;
            job->progress.completed = completed;
            job->progress.total = total;
        };

        const auto result = installPack (transport, job->pack, layout, limits, callbacks);

        if (result.status == InstallStatus::cancelled
            && ! job->cancelled->load (std::memory_order_relaxed)
            && ! quit.load (std::memory_order_relaxed))
        {
            // The gate interrupted the download - the only phase it can reach.
            // The job keeps its place and its partial transfer, and waits on the
            // paused path instead of looping straight back into installPack's
            // prologue, which with the gate still closed would spin.
            if (pauseUntilActivity (job))
                return;
            continue;
        }

        finish (job, result.status, result.error);
    }
}
} // namespace duskstudio::sfz
