#include "AudioWorkerPool.h"

namespace duskstudio
{
struct AudioWorkerPool::Worker : public juce::Thread
{
    Worker (AudioWorkerPool& p, int idx)
        : juce::Thread ("DuskDSP " + juce::String (idx)), pool (p), lane (idx) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            // Park until the audio thread dispatches a block (or we're quitting).
            // Auto-reset event: a signal that lands between our completion below
            // and this wait() is latched, so a wakeup is never lost.
            wake.wait();
            if (threadShouldExit() || pool.quit.load (std::memory_order_acquire))
                break;

            pool.job_ (lane);
            pool.done.fetch_add (1, std::memory_order_release);
        }
    }

    AudioWorkerPool&    pool;
    int                 lane;
    juce::WaitableEvent wake;   // auto-reset
};

AudioWorkerPool::AudioWorkerPool() = default;
AudioWorkerPool::~AudioWorkerPool() { stop(); }

void AudioWorkerPool::start (int workers, std::function<void (int)> job)
{
    stop();
    job_ = std::move (job);
    quit.store (false, std::memory_order_release);

    const int want = juce::jmax (0, workers);
    for (int i = 0; i < want; ++i)
    {
        auto w = std::make_unique<Worker> (*this, (int) workers_.size());
        // Prefer RT scheduling so a worker can't be preempted by ordinary
        // threads mid-block; fall back to the highest normal priority if the
        // OS denies real-time (e.g. no rtprio limit / unprivileged container).
        const bool started = w->startRealtimeThread (juce::Thread::RealtimeOptions {})
                          || w->startThread (juce::Thread::Priority::highest);
        if (started)
            workers_.push_back (std::move (w));
        // A worker that failed to start is dropped: it would never reach its
        // wake/job loop, so the audio thread's spin-join would wait forever.
    }
    // numWorkers reflects threads that actually started, so laneCount() and the
    // spin-join can never expect a completion that won't arrive.
    numWorkers = (int) workers_.size();
}

void AudioWorkerPool::stop()
{
    if (workers_.empty()) { numWorkers = 0; return; }

    quit.store (true, std::memory_order_release);
    for (auto& w : workers_)
    {
        w->signalThreadShouldExit();
        w->wake.signal();             // unpark so run() can observe the exit
    }
    for (auto& w : workers_)
        w->stopThread (1000);
    workers_.clear();
    numWorkers = 0;
}

void AudioWorkerPool::runBlock() noexcept
{
    if (numWorkers <= 0)
    {
        if (job_) job_ (0);           // inactive: caller is the only lane
        return;
    }

    done.store (0, std::memory_order_release);
    for (auto& w : workers_)
        w->wake.signal();             // dispatch lanes [0, numWorkers)

    job_ (numWorkers);                // caller runs the last lane

    // Lock-free join: the audio thread must wait for the workers anyway, and
    // the wait is bounded by the slowest lane's DSP time.
    while (done.load (std::memory_order_acquire) < numWorkers)
        juce::Thread::yield();
}
} // namespace duskstudio
