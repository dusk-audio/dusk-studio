#include "AudioWorkerPool.h"
#include "RtPriority.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(__linux__)
 #include "RealtimeKit.h"
 #include <pthread.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#elif defined(__APPLE__)
 #include <CoreAudio/CoreAudio.h>
 #include <mach/mach.h>
 #include <mach/mach_time.h>
 #include <mach/thread_policy.h>
 #include <os/workgroup.h>
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
#elif defined(__APPLE__)
namespace
{
// The workgroup CoreAudio runs the named device's IO thread in, retained, or
// null. The dusk device seam names devices as CoreAudio does, so a name that
// matches exactly one device is that device; none or several, and the workers
// make do with the time-constraint policy alone.
os_workgroup_t copyIoWorkgroup (const std::string& deviceName) noexcept
{
    constexpr AudioObjectPropertyElement kMainElement = 0;
    AudioObjectPropertyAddress devicesAddress { kAudioHardwarePropertyDevices,
                                                kAudioObjectPropertyScopeGlobal, kMainElement };
    UInt32 size = 0;
    if (deviceName.empty()
        || AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &devicesAddress,
                                           0, nullptr, &size) != noErr)
        return nullptr;
    std::vector<AudioObjectID> devices (size / sizeof (AudioObjectID));
    if (devices.empty()
        || AudioObjectGetPropertyData (kAudioObjectSystemObject, &devicesAddress,
                                       0, nullptr, &size, devices.data()) != noErr)
        return nullptr;
    devices.resize (size / sizeof (AudioObjectID));

    AudioObjectID match = kAudioObjectUnknown;
    int matches = 0;
    for (const auto device : devices)
    {
        AudioObjectPropertyAddress nameAddress { kAudioObjectPropertyName,
                                                 kAudioObjectPropertyScopeGlobal, kMainElement };
        CFStringRef name = nullptr;
        UInt32 nameSize = sizeof (name);
        if (AudioObjectGetPropertyData (device, &nameAddress, 0, nullptr, &nameSize, &name) != noErr
            || name == nullptr)
            continue;
        char utf8[512] {};
        const bool converted = CFStringGetCString (name, utf8, (CFIndex) sizeof (utf8), kCFStringEncodingUTF8);
        CFRelease (name);
        if (converted && deviceName == utf8)
        {
            match = device;
            ++matches;
        }
    }
    if (matches != 1)
        return nullptr;

    AudioObjectPropertyAddress workgroupAddress { kAudioDevicePropertyIOThreadOSWorkgroup,
                                                  kAudioObjectPropertyScopeWildcard, kMainElement };
    os_workgroup_t workgroup = nullptr;
    UInt32 workgroupSize = sizeof (workgroup);
    if (AudioObjectGetPropertyData (match, &workgroupAddress, 0, nullptr,
                                    &workgroupSize, &workgroup) != noErr)
        return nullptr;
    return workgroup;
}

// Realtime time-constraint policy for the calling thread, as CoreAudio gives
// its IO thread: woken once a period and allowed to compute through it. The
// kernel refuses a computation much past 50 ms, which only a very long block
// reaches.
void takeTimeConstraint (double periodSeconds) noexcept
{
    mach_timebase_info_data_t timebase {};
    if (periodSeconds <= 0.0 || mach_timebase_info (&timebase) != KERN_SUCCESS || timebase.numer == 0)
        return;
    const double ticksPerSecond = 1.0e9 * (double) timebase.denom / (double) timebase.numer;
    const auto ticks = [ticksPerSecond] (double seconds)
    {
        return (uint32_t) std::min (seconds * ticksPerSecond, 4.0e9);
    };
    thread_time_constraint_policy_data_t policy {};
    policy.period      = ticks (periodSeconds);
    policy.computation = ticks (std::min (periodSeconds, 0.05));
    policy.constraint  = ticks (periodSeconds);
    policy.preemptible = 1;
    thread_policy_set (pthread_mach_thread_np (pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                       (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
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
        // round-robins fairly if a worker shares the audio thread's core. An
        // RT-denied thread is left to start(), which asks on its behalf.
        const bool realtime = rt::applyRealtimeSchedRR (rtJucePriority);
       #if defined(__linux__)
        char name[16];
        std::snprintf (name, sizeof name, "DuskDSP %d", lane);
        pthread_setname_np (pthread_self(), name);
        threadId.store ((std::int64_t) syscall (SYS_gettid), std::memory_order_relaxed);
       #elif defined(_WIN32)
        const auto proAudio = joinProAudioTask();
       #endif
        gotRealtime.store (realtime, std::memory_order_relaxed);
        takeDeadline();
        started.store (true, std::memory_order_release);
        runLanes();
        leaveDeadline();
       #if defined(_WIN32)
        leaveProAudioTask (proAudio);
       #endif
    }

    // The pool's current device deadline (setDeviceDeadline), on this thread:
    // on macOS the IO thread's workgroup and a time-constraint policy for its
    // period. Acknowledged either way, so the setter can wait for every worker.
    void takeDeadline() noexcept
    {
        deadlineSeen = pool.deadlineGeneration.load (std::memory_order_acquire);
       #if defined(__APPLE__)
        leaveDeadline();
        if (pool.deadlineSampleRate > 0.0 && pool.deadlineBlockSize > 0)
            takeTimeConstraint ((double) pool.deadlineBlockSize / pool.deadlineSampleRate);
        auto* const workgroup = static_cast<os_workgroup_t> (pool.deadlineWorkgroup);
        if (workgroup != nullptr && os_workgroup_join (workgroup, &joinToken) == 0)
        {
            os_retain (workgroup);
            joinedWorkgroup = workgroup;
        }
       #endif
        deadlineTaken.store (deadlineSeen, std::memory_order_release);
    }

    void leaveDeadline() noexcept
    {
       #if defined(__APPLE__)
        if (joinedWorkgroup == nullptr) return;
        os_workgroup_leave (joinedWorkgroup, &joinToken);
        os_release (joinedWorkgroup);
        joinedWorkgroup = nullptr;
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
            if (pool.deadlineGeneration.load (std::memory_order_acquire) != deadlineSeen)
                takeDeadline();
            if (shouldExit.load (std::memory_order_acquire)
                || pool.quit.load (std::memory_order_acquire))
                break;

            const auto s = pool.seq.load (std::memory_order_acquire);
            if (s == acked.load (std::memory_order_relaxed))
                continue;   // stale latched wake from an already-acked epoch

            // A wake from quiesce() must NOT run the job: the lane inputs
            // (trackJobs device-input pointers) may already be freed by the
            // device close path. Ack the epoch so quiesce can complete. Nor
            // may a parked lane, past the count, run one.
            const int active = pool.numWorkers.load (std::memory_order_relaxed);
            if (! pool.quiescing.load (std::memory_order_acquire) && lane < active)
            {
                pool.job_ (lane);
                if (pool.done.fetch_add (1, std::memory_order_release) + 1 == active)
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

    // Published once the thread has set up its scheduling and is about to
    // park; start() waits for it.
    std::atomic<bool>         started { false };
    std::atomic<bool>         gotRealtime { false };
    std::atomic<std::int64_t> threadId { 0 };

    std::uint32_t             deadlineSeen = 0;   // worker thread only
    std::atomic<std::uint32_t> deadlineTaken { 0 };
   #if defined(__linux__)
    // What the thread was granted once start() had raised it. Message thread.
    rt::ThreadScheduling      granted;
   #endif
   #if defined(__APPLE__)
    os_workgroup_t            joinedWorkgroup = nullptr;
    os_workgroup_join_token_s joinToken {};
   #endif
};

AudioWorkerPool::AudioWorkerPool() = default;

AudioWorkerPool::~AudioWorkerPool()
{
    stop();
   #if defined(__APPLE__)
    if (deadlineWorkgroup != nullptr)
        os_release (deadlineWorkgroup);
   #endif
}

void AudioWorkerPool::start (int workers, std::function<void (int)> job, int rtJucePriority)
{
    // Every held lane parked and acknowledged before the job or the count
    // changes under it.
    quiesce();
    job_ = std::move (job);
    quit.store (false, std::memory_order_release);
    done.store (0, std::memory_order_release);

    const int want = std::max (0, workers);
    const auto held = workers_.size();
    for (auto lane = held; lane < (std::size_t) want; ++lane)
    {
        auto w = std::make_unique<Worker> (*this, (int) lane, rtJucePriority);
        // Born at the current epoch: nothing dispatched before it is its to run.
        w->acked.store (seq.load (std::memory_order_acquire), std::memory_order_relaxed);
        w->start();
        workers_.push_back (std::move (w));
    }

    // Every new worker has its scheduling before the first block can wait on it.
    for (auto lane = held; lane < workers_.size(); ++lane)
        while (! workers_[lane]->started.load (std::memory_order_acquire))
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    numWorkers.store (want, std::memory_order_relaxed);

   #if defined(__linux__)
    // Without RLIMIT_RTPRIO a worker stays at the default class while the
    // device's thread may well be realtime through RTKit (PipeWire's is), and
    // the callback then waits on lanes anything else can preempt. A thread
    // kept from an earlier call already has its answer.
    std::vector<std::int64_t> denied;
    for (auto lane = held; lane < workers_.size(); ++lane)
        if (! workers_[lane]->gotRealtime.load (std::memory_order_relaxed))
            denied.push_back (workers_[lane]->threadId.load (std::memory_order_relaxed));
    if (! denied.empty())
        std::fprintf (stderr, "[DuskStudio] DSP workers without RLIMIT_RTPRIO: %s\n",
                      rt::raiseThreadsWithoutRtPrio (denied).c_str());
    for (auto lane = held; lane < workers_.size(); ++lane)
        workers_[lane]->granted = rt::threadScheduling (workers_[lane]->threadId.load (std::memory_order_relaxed));
   #endif
}

bool AudioWorkerPool::anyLaneLostRealtime() const
{
   #if defined(__linux__)
    for (const auto& w : workers_)
        if (w->granted.isRealtime()
            && ! rt::threadScheduling (w->threadId.load (std::memory_order_relaxed)).isRealtime())
            return true;
   #endif
    return false;
}

int AudioWorkerPool::restoreRealtime (rt::RealtimeRestorer& restorer)
{
    int restored = 0;
   #if defined(__linux__)
    quiesce();
    for (auto& w : workers_)
    {
        const auto threadId = w->threadId.load (std::memory_order_relaxed);
        if (! w->granted.isRealtime() || rt::threadScheduling (threadId).isRealtime())
            continue;
        // Quiesced, the worker has acknowledged and nothing will wake it, but
        // it may not have reached its wait yet: moments, as it has nothing
        // left to run. One still not blocked after this many stays as it is,
        // and anyLaneLostRealtime() goes on saying so.
        for (int look = 0; look < 100 && ! rt::threadIsBlocked (threadId); ++look)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        if (rt::threadIsBlocked (threadId) && restorer.restore (threadId, w->granted))
            ++restored;
    }
   #else
    (void) restorer;
   #endif
    return restored;
}

void AudioWorkerPool::setDeviceDeadline (double sampleRate, int blockSize,
                                         const std::string& deviceName)
{
    void* const previous = deadlineWorkgroup;
    deadlineSampleRate = sampleRate;
    deadlineBlockSize = blockSize;
   #if defined(__APPLE__)
    deadlineWorkgroup = copyIoWorkgroup (deviceName);
   #else
    (void) deviceName;
   #endif
    const auto generation = deadlineGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    for (auto& w : workers_)
        w->wake.signal();
    for (auto& w : workers_)
        while (w->deadlineTaken.load (std::memory_order_acquire) != generation)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
   #if defined(__APPLE__)
    if (previous != nullptr)
        os_release (previous);
   #else
    (void) previous;
   #endif
}

void AudioWorkerPool::stop()
{
    if (workers_.empty()) { numWorkers.store (0, std::memory_order_relaxed); return; }

    quiesce();   // joins in-flight lanes so no thread is killed mid-job

    quit.store (true, std::memory_order_release);
    for (auto& w : workers_)
        w->requestExitAndWake();
    for (auto& w : workers_)
        w->join();
    workers_.clear();
    numWorkers.store (0, std::memory_order_relaxed);
}

void AudioWorkerPool::runBlock() noexcept
{
    const int active = numWorkers.load (std::memory_order_relaxed);
    if (active <= 0)
    {
        if (job_) job_ (0);           // inactive: caller is the only lane
        return;
    }

    done.store (0, std::memory_order_release);
    seq.fetch_add (1, std::memory_order_release);
    for (int lane = 0; lane < active; ++lane)
        workers_[(std::size_t) lane]->wake.signal();   // dispatch lanes [0, active)

    job_ (active);                    // caller runs the last lane

    // Fast path: workers usually finish within a short spin. Past that, BLOCK
    // on the completion event - never spin unboundedly: sched_yield from a
    // high-priority SCHED_RR thread won't cede the core to a lower-priority
    // worker, so an unbounded spin can deadlock the callback. The counter is
    // the source of truth; the event (auto-reset, signalled once per dispatch
    // by the last worker) only accelerates the wait, and the 1 ms timeout
    // bounds any stale-signal consumption.
    for (int i = 0; i < 1024; ++i)
    {
        if (done.load (std::memory_order_acquire) >= active)
            return;
        std::this_thread::yield();
    }
    int waitedMs = 0;
    while (done.load (std::memory_order_acquire) < active)
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
    // A parked lane has nothing in flight: the call that parked it quiesced
    // first, and no block since has dispatched it.
    if (workers_.empty())
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
    const int active = numWorkers.load (std::memory_order_relaxed);
    if (active <= 0)
        return;

    done.store (0, std::memory_order_release);
    seq.fetch_add (1, std::memory_order_release);
    const int n = signalOnlyFirst < 0 ? active
                                      : std::min (signalOnlyFirst, active);
    for (int i = 0; i < n; ++i)
        workers_[(size_t) i]->wake.signal();
}

std::vector<std::int64_t> AudioWorkerPool::workerThreadIdsForTest() const
{
    std::vector<std::int64_t> ids;
   #if defined(__linux__)
    for (const auto& w : workers_)
        ids.push_back (w->threadId.load (std::memory_order_relaxed));
   #endif
    return ids;
}
} // namespace duskstudio
