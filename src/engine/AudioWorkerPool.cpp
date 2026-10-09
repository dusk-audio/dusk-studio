#include "AudioWorkerPool.h"
#include "RtPriority.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(__linux__)
 #include <pthread.h>
#elif defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace duskstudio
{
#if defined(_WIN32)
namespace
{
// The WASAPI callback thread runs in the MMCSS "Pro Audio" task, which JUCE's
// device thread joins. The workers that thread waits on every block have to
// run in it too: at normal priority they share the cores with every other
// normal thread, a software GL renderer's while an editor draws among them,
// and the callback overruns waiting for its lanes. avrt.dll is loaded at run
// time, as JUCE does, so nothing new is linked.
struct ProAudioTask
{
    HMODULE avrt = nullptr;
    HANDLE  task = nullptr;
};

// GetProcAddress hands back a generic pointer; going through void (*)() is the
// cast compilers accept to any other function type without a warning.
template <typename Function>
Function avrtFunction (HMODULE avrt, const char* name) noexcept
{
    return reinterpret_cast<Function> (reinterpret_cast<void (*)()> (GetProcAddress (avrt, name)));
}

ProAudioTask joinProAudioTask() noexcept
{
    using Join = HANDLE (WINAPI*) (LPCWSTR, LPDWORD);
    ProAudioTask joined;
    joined.avrt = LoadLibraryW (L"avrt.dll");
    if (joined.avrt != nullptr)
        if (const auto join = avrtFunction<Join> (joined.avrt, "AvSetMmThreadCharacteristicsW"))
        {
            DWORD taskIndex = 0;
            joined.task = join (L"Pro Audio", &taskIndex);
        }
    // Without MMCSS, still above every normal-priority thread.
    if (joined.task == nullptr)
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    return joined;
}

void leaveProAudioTask (const ProAudioTask& joined) noexcept
{
    using Leave = BOOL (WINAPI*) (HANDLE);
    if (joined.task != nullptr)
        if (const auto leave = avrtFunction<Leave> (joined.avrt, "AvRevertMmThreadCharacteristics"))
            leave (joined.task);
    if (joined.avrt != nullptr)
        FreeLibrary (joined.avrt);
}
} // namespace
#endif

struct AudioWorkerPool::Worker
{
    Worker (AudioWorkerPool& p, int idx, int rtPrio)
        : pool (p), lane (idx), rtJucePriority (rtPrio) {}

    void start() { th = std::thread ([this] { run(); }); }

    void run()
    {
        // Same realtime priority as the audio I/O thread (RtPriority.h) so RR
        // round-robins fairly if a worker shares the audio thread's core; on an
        // RT-denied system the thread simply runs at the default class.
        rt::applyRealtimeSchedRR (rtJucePriority);
       #if defined(__linux__)
        char name[16];
        std::snprintf (name, sizeof name, "DuskDSP %d", lane);
        pthread_setname_np (pthread_self(), name);
       #elif defined(_WIN32)
        const auto proAudio = joinProAudioTask();
       #endif
        runLanes();
       #if defined(_WIN32)
        leaveProAudioTask (proAudio);
       #endif
    }

    void runLanes()
    {
        while (! shouldExit.load (std::memory_order_acquire))
        {
            // Park until dispatched (or quiesced, or quitting). Auto-reset
            // event: a signal landing between our ack below and this wait() is
            // latched, so a wakeup is never lost.
            wake.wait();
            if (shouldExit.load (std::memory_order_acquire)
                || pool.quit.load (std::memory_order_acquire))
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

    void requestExitAndWake() noexcept
    {
        shouldExit.store (true, std::memory_order_release);
        wake.signal();   // unpark so run() can observe the exit
    }

    void join() { if (th.joinable()) th.join(); }

    AudioWorkerPool&      pool;
    int                   lane;
    int                   rtJucePriority;
    std::thread           th;
    dusk::AutoResetEvent  wake;             // auto-reset
    std::atomic<uint32_t> acked { 0 };
    std::atomic<bool>     shouldExit { false };
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

    const int want = std::max (0, workers);
    for (int i = 0; i < want; ++i)
    {
        auto w = std::make_unique<Worker> (*this, (int) workers_.size(), rtJucePriority);
        w->start();
        workers_.push_back (std::move (w));
    }
    numWorkers = (int) workers_.size();
}

void AudioWorkerPool::stop()
{
    if (workers_.empty()) { numWorkers = 0; return; }

    quiesce();   // joins in-flight lanes so no thread is killed mid-job

    quit.store (true, std::memory_order_release);
    for (auto& w : workers_)
        w->requestExitAndWake();
    for (auto& w : workers_)
        w->join();
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
    // on the completion event - never spin unboundedly: sched_yield from a
    // high-priority SCHED_RR thread won't cede the core to a lower-priority
    // worker, so an unbounded spin can deadlock the callback. The counter is
    // the source of truth; the event (auto-reset, signalled once per dispatch
    // by the last worker) only accelerates the wait, and the 1 ms timeout
    // bounds any stale-signal consumption.
    for (int i = 0; i < 1024; ++i)
    {
        if (done.load (std::memory_order_acquire) >= numWorkers)
            return;
        std::this_thread::yield();
    }
    int waitedMs = 0;
    while (done.load (std::memory_order_acquire) < numWorkers)
    {
        completion.wait (1);
        // Stall diagnostic. A worker wedged inside a plugin's processBlock holds
        // the whole callback hostage. Latch it RT-safely - just bump an atomic,
        // NO stderr on the audio thread - and let a non-RT consumer surface
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
            std::this_thread::sleep_for (std::chrono::milliseconds (1));

    quiescing.store (false, std::memory_order_release);
}

void AudioWorkerPool::dispatchForTest (int signalOnlyFirst)
{
    if (numWorkers <= 0)
        return;

    done.store (0, std::memory_order_release);
    seq.fetch_add (1, std::memory_order_release);
    const int n = signalOnlyFirst < 0 ? numWorkers
                                      : std::min (signalOnlyFirst, numWorkers);
    for (int i = 0; i < n; ++i)
        workers_[(size_t) i]->wake.signal();
}
} // namespace duskstudio
