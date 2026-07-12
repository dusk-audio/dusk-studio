#pragma once

#include "../foundation/AutoResetEvent.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

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

    // Message thread only. Spawns `workers` real-time threads at the given
    // realtime priority on JUCE's 0..10 scale (see RtPriority.h; < 0 or an
    // RT-denied thread runs at the OS default scheduling class); `job` is stored
    // once (never reallocated per block) and invoked as job(lane). A count <= 0
    // leaves the pool inactive (runBlock then runs job(0) inline on the caller).
    void start (int workers, std::function<void (int lane)> job, int rtJucePriority = 5);
    void stop();   // message thread only; quiesces, then joins every worker.

    bool isActive()  const noexcept { return numWorkers > 0; }
    int  laneCount() const noexcept { return numWorkers + 1; }

    // Audio thread. Dispatches lanes [0, workers) to the worker threads, runs
    // lane `workers` on the caller, and returns once every lane has finished.
    void runBlock() noexcept;

    // Message thread only; caller must guarantee no new runBlock can begin
    // (callback detached, device stopped, or device thread not yet spawned).
    // Returns once no worker lane is in flight - including lanes orphaned by a
    // force-killed dispatcher. Cheap no-op when the pool is idle.
    void quiesce();

    // Test-only: the dispatch half of runBlock without the join, simulating a
    // dispatcher force-killed mid-block. Signals the first `signalOnlyFirst`
    // workers (all if negative).
    void dispatchForTest (int signalOnlyFirst = -1);

    // Times the per-block join blocked >2 s (a worker wedged in a plugin's
    // processBlock). Bumped RT-safely from runBlock; poll from a non-RT thread.
    int joinStallCount() const noexcept { return joinStalls.load (std::memory_order_relaxed); }

private:
    struct Worker;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::function<void (int)> job_;
    int numWorkers = 0;
    std::atomic<int>      done { 0 };
    std::atomic<uint32_t> seq { 0 };          // dispatch epoch; equality-compared only
    std::atomic<bool>     quiescing { false };
    std::atomic<bool>     quit { false };
    std::atomic<int>      joinStalls { 0 };    // count of >2 s join stalls (diagnostic)
    dusk::AutoResetEvent  completion;          // auto-reset; signalled by the last worker

    AudioWorkerPool (const AudioWorkerPool&) = delete;
    AudioWorkerPool& operator= (const AudioWorkerPool&) = delete;
};
} // namespace duskstudio
