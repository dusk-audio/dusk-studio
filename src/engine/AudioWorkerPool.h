#pragma once

#include "../foundation/AutoResetEvent.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::rt { class RealtimeRestorer; }

namespace duskstudio
{
// Fixed pool of real-time worker threads for the per-block strip-DSP fan-out
// (opt-in; see AudioEngine). The audio thread calls runBlock(); job(lane) then
// runs on each of the `workers` worker threads for lane in [0, workers), and on
// the audio thread itself for lane == workers. So laneCount() == workers + 1.
//
// Workers park on an auto-reset event between blocks and are woken
// by the audio thread's signal() - a bounded, uncontended, ns-scale event that
// is the one deliberate exception to the no-lock-on-the-audio-thread rule, used
// ONLY in this opt-in parallel mode. The join is a short bounded spin (fast
// path: workers finish within the spin) followed by a blocking wait on a
// completion event - NEVER an unbounded yield-spin. sched_yield from a SCHED_RR
// thread does not cede the core to a lower-priority thread, so an unbounded
// spin deadlocks the callback if a worker is migrated onto the audio thread's
// core; the blocking wait frees the core and is immune to priority layout.
// Workers should be started at the SAME realtime priority as the audio thread
// (see RtPriority.h) so RR round-robins them fairly when they do share a core.
//
// Dispatches are identified by an epoch counter (`seq`); each worker records the
// last epoch it acknowledged. quiesce() - message thread only, and only when no
// new runBlock can begin - waits until every worker has acknowledged the current
// epoch, waking and draining any worker whose dispatch signal was delivered but
// whose dispatcher died (the device I/O thread can be force-killed mid-runBlock
// by AlsaAudioIODevice::stop's thread timeout). A worker woken BY quiesce skips
// the job: its inputs may point into device buffers the close path has already
// freed. After quiesce returns, no lane is in flight and prepare may safely
// resize every per-block buffer.
class AudioWorkerPool
{
public:
    AudioWorkerPool();
    ~AudioWorkerPool();

    // Message thread only, with no runBlock in flight. Runs `workers` lanes on
    // real-time threads at the given realtime priority on JUCE's 0..10 scale
    // (see RtPriority.h), and returns once every one has its scheduling. On
    // Linux a thread RLIMIT_RTPRIO refuses is raised through RTKit, or failing
    // that to a better nice level (RealtimeKit.h); on Windows each joins the
    // MMCSS "Pro Audio" task. `job` is stored once (never reallocated per
    // block) and invoked as job(lane). A count <= 0 leaves the pool inactive
    // (runBlock then runs job(0) inline on the caller).
    //
    // Threads outlive a call: a smaller count parks the ones past it and a
    // larger one wakes them again before it spawns any, so each thread asks
    // for its scheduling once. RTKit grants a user about 25 requests in 20 s,
    // and a lane it refuses runs at nice beside realtime ones.
    void start (int workers, std::function<void (int lane)> job, int rtJucePriority = 5);
    void stop();   // message thread only; quiesces, then joins every worker.

    // Message thread, with no runBlock in flight (the engine's gate held, the
    // pool quiesced). The deadline the workers share with the device's IO
    // thread. On macOS each worker takes a realtime time-constraint policy for
    // the block period and joins the IO thread's audio workgroup, found by the
    // device's name; elsewhere it changes nothing. Returns once every worker
    // has taken it; workers started later take it as they start.
    void setDeviceDeadline (double sampleRate, int blockSize, const std::string& deviceName);

    bool isActive()  const noexcept { return numWorkers.load (std::memory_order_relaxed) > 0; }
    int  laneCount() const noexcept { return numWorkers.load (std::memory_order_relaxed) + 1; }

    // Audio thread. Dispatches lanes [0, workers) to the worker threads, runs
    // lane `workers` on the caller, and returns once every lane has finished.
    void runBlock() noexcept;

    // Message thread only; caller must guarantee no new runBlock can begin
    // (callback detached, device stopped, or device thread not yet spawned).
    // Returns once no worker lane is in flight - including lanes orphaned by a
    // force-killed dispatcher. Cheap no-op when the pool is idle.
    void quiesce();

    // Message thread, with no runBlock able to begin (the engine's gate held, or
    // the callback detached). A lane that computed past the realtime CPU-time
    // warning was moved to normal priority on the spot (RealtimeKit.h) and,
    // as threads outlive start(), would stay there, slowing every block that
    // joins it. This parks every lane and puts each one that had realtime when
    // it started back on it, through `restorer`, which the caller makes before
    // holding the audio back. Linux; returns how many it put back.
    int restoreRealtime (rt::RealtimeRestorer& restorer);

    // Message thread. True when a lane that started with realtime runs
    // without it now, which restoreRealtime() is for. Linux.
    bool anyLaneLostRealtime() const;

    // Test-only: the dispatch half of runBlock without the join, simulating a
    // dispatcher force-killed mid-block. Signals the first `signalOnlyFirst`
    // workers (all if negative).
    void dispatchForTest (int signalOnlyFirst = -1);

    // Test-only, Linux: the kernel thread id of every worker thread the pool
    // holds, active or parked, in lane order. Empty elsewhere.
    std::vector<std::int64_t> workerThreadIdsForTest() const;

    // Times the per-block join blocked >2 s (a worker wedged in a plugin's
    // processBlock). Bumped RT-safely from runBlock; poll from a non-RT thread.
    int joinStallCount() const noexcept { return joinStalls.load (std::memory_order_relaxed); }

private:
    struct Worker;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::function<void (int)> job_;
    // Lanes dispatched per block; workers_ past it are parked. Written by the
    // message thread with no block in flight, published to the workers by seq.
    std::atomic<int>      numWorkers { 0 };
    std::atomic<int>      done { 0 };
    std::atomic<uint32_t> seq { 0 };          // dispatch epoch; equality-compared only
    std::atomic<bool>     quiescing { false };
    std::atomic<bool>     quit { false };
    std::atomic<int>      joinStalls { 0 };    // count of >2 s join stalls (diagnostic)
    dusk::AutoResetEvent  completion;          // auto-reset; signalled by the last worker

    // The last setDeviceDeadline. Written by the message thread only while
    // the workers are parked, and published to them by the generation.
    double deadlineSampleRate = 0.0;
    int    deadlineBlockSize  = 0;
    void*  deadlineWorkgroup  = nullptr;       // macOS: a retained os_workgroup_t
    std::atomic<std::uint32_t> deadlineGeneration { 0 };

    AudioWorkerPool (const AudioWorkerPool&) = delete;
    AudioWorkerPool& operator= (const AudioWorkerPool&) = delete;
};
} // namespace duskstudio
