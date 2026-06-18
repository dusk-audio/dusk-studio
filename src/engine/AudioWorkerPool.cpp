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
            // Park until dispatched (or quiesced, or quitting). Auto-reset
            // event: a signal landing between our ack below and this wait() is
            // latched, so a wakeup is never lost.
            wake.wait();
            if (threadShouldExit() || pool.quit.load (std::memory_order_acquire))
                break;

            const auto s = pool.seq.load (std::memory_order_acquire);
            if (s == acked.load (std::memory_order_relaxed))
                continue;   // stale latched wake from an already-acked epoch

            // A wake from quiesce() must NOT run the job: the lane inputs
            // (trackJobs device-input pointers) may already be freed by the
            // device close path. Ack the epoch so quiesce can complete.
            if (! pool.quiescing.load (std::memory_order_acquire))
            {
                pool.job_ (lane);
                if (pool.done.fetch_add (1, std::memory_order_release) + 1 == pool.numWorkers)
                    pool.completion.signal();
            }
            acked.store (s, std::memory_order_release);
        }
    }

    AudioWorkerPool&      pool;
    int                   lane;
    juce::WaitableEvent   wake;       // auto-reset
    std::atomic<uint32_t> acked { 0 };
};

AudioWorkerPool::AudioWorkerPool() = default;
AudioWorkerPool::~AudioWorkerPool() { stop(); }

void AudioWorkerPool::start (int workers, std::function<void (int)> job, int rtJucePriority)
{
    stop();
    job_ = std::move (job);
    quit.store (false, std::memory_order_release);
    quiescing.store (false, std::memory_order_release);
    seq.store (0, std::memory_order_release);
    done.store (0, std::memory_order_release);

    const int want = juce::jmax (0, workers);
    for (int i = 0; i < want; ++i)
    {
        auto w = std::make_unique<Worker> (*this, (int) workers_.size());
        // Same realtime priority as the audio I/O thread (RtPriority.h) so RR
        // round-robins fairly if a worker shares the audio thread's core; fall
        // back to the highest normal priority if the OS denies real-time.
        const bool started = (rtJucePriority > 0
                               && w->startRealtimeThread (juce::Thread::RealtimeOptions{}
                                                              .withPriority (rtJucePriority)))
                          || w->startThread (juce::Thread::Priority::highest);
        if (started)
            workers_.push_back (std::move (w));
        // A worker that failed to start is dropped: it would never reach its
        // wake/job loop, so the join would wait forever on its completion.
    }
    // numWorkers reflects threads that actually started, so laneCount() and the
    // join can never expect a completion that won't arrive.
    numWorkers = (int) workers_.size();
}

void AudioWorkerPool::stop()
{
    if (workers_.empty()) { numWorkers = 0; return; }

    quiesce();   // joins in-flight lanes so stopThread never kills one mid-job

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
    seq.fetch_add (1, std::memory_order_release);
    for (auto& w : workers_)
        w->wake.signal();             // dispatch lanes [0, numWorkers)

    job_ (numWorkers);                // caller runs the last lane

    // Fast path: workers usually finish within a short spin. Past that, BLOCK
    // on the completion event — never spin unboundedly: sched_yield from a
    // high-priority SCHED_RR thread won't cede the core to a lower-priority
    // worker, so an unbounded spin can deadlock the callback. The counter is
    // the source of truth; the event (auto-reset, signalled once per dispatch
    // by the last worker) only accelerates the wait, and the 1 ms timeout
    // bounds any stale-signal consumption.
    for (int i = 0; i < 1024; ++i)
    {
        if (done.load (std::memory_order_acquire) >= numWorkers)
            return;
        juce::Thread::yield();
    }
    int waitedMs = 0;
    while (done.load (std::memory_order_acquire) < numWorkers)
    {
        completion.wait (1);
        // Stall diagnostic. A worker wedged inside a plugin's processBlock holds
        // the whole callback hostage. Latch it RT-safely — just bump an atomic,
        // NO stderr on the audio thread — and let a non-RT consumer surface
        // joinStallCount(). At 2 s the deadline is long dead, so a reader racing
        // this increment is immaterial.
        if (++waitedMs == 2000)
            joinStalls.fetch_add (1, std::memory_order_relaxed);
    }
}

void AudioWorkerPool::quiesce()
{
    if (numWorkers <= 0)
        return;

    const auto s = seq.load (std::memory_order_acquire);
    quiescing.store (true, std::memory_order_release);

    // Workers whose dispatch signal was delivered before their dispatcher died
    // finish their lane and ack; workers never signalled get woken here, see
    // `quiescing`, skip the job, and ack. Either way every worker converges on
    // acked == s, after which nothing is in flight.
    for (auto& w : workers_)
        if (w->acked.load (std::memory_order_acquire) != s)
            w->wake.signal();
    for (auto& w : workers_)
        while (w->acked.load (std::memory_order_acquire) != s)
            juce::Thread::sleep (1);

    quiescing.store (false, std::memory_order_release);
}

void AudioWorkerPool::dispatchForTest (int signalOnlyFirst)
{
    if (numWorkers <= 0)
        return;

    done.store (0, std::memory_order_release);
    seq.fetch_add (1, std::memory_order_release);
    const int n = signalOnlyFirst < 0 ? numWorkers
                                      : juce::jmin (signalOnlyFirst, numWorkers);
    for (int i = 0; i < n; ++i)
        workers_[(size_t) i]->wake.signal();
}
} // namespace duskstudio
