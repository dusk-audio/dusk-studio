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
// (opt-in; see AudioEngine). The audio thread calls runBlock(n); job(lane) then
// runs on worker thread `lane` for lane in [0, n), and on the audio thread
// itself for lane == workers. laneCount() == workers + 1, and the workers past
// n stay asleep for the block.
//
// Workers park on an RtWakeEvent between blocks, which the audio thread
// signals without a lock. The join is a short bounded spin (fast path: workers
// finish within the spin) followed by a blocking wait on a completion event -
// NEVER an unbounded yield-spin. sched_yield from a SCHED_RR thread does not
// cede the core to a lower-priority thread, so an unbounded spin deadlocks the
// callback if a worker is migrated onto the audio thread's core; the blocking
// wait frees the core and is immune to priority layout. Workers should be
// started at the SAME realtime priority as the audio thread (see RtPriority.h)
// so RR round-robins them fairly when they do share a core.
//
// Dispatches are identified by an epoch; each worker records the last epoch it
// acknowledged. quiesce() - message thread only, and only when no new runBlock
// can begin - waits until every worker has acknowledged the current epoch,
// waking and draining any worker whose dispatch signal was delivered but whose
// dispatcher died (the device I/O thread can be force-killed mid-runBlock by
// AlsaAudioIODevice::stop's thread timeout). A worker woken BY quiesce skips
// the job: its inputs may point into device buffers the close path has already
// freed. After quiesce returns, no lane is in flight and prepare may safely
// resize every per-block buffer.
//
// A worker quiesce() finds still running its job keeps that wake latched, and
// can take it after a later block that did not dispatch it has begun. So a
// block publishes its epoch and the number of worker lanes it dispatched in
// one word, which a worker reads at once: read apart, a worker could pair the
// new epoch with another block's count, run a lane twice and count itself
// done while a dispatched lane is still writing.
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

    // Audio thread. Dispatches lanes [0, workerLanes) to their worker threads,
    // runs lane `workers` on the caller, and returns once each of them has
    // finished. The dispatch takes no lock and allocates nothing, and the
    // workers past workerLanes are not woken; the join is the wait above.
    void runBlock (int workerLanes) noexcept;
    void runBlock() noexcept { runBlock (numWorkers.load (std::memory_order_relaxed)); }

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
    // workers (all if negative). With `killedInJoin` the dispatcher had reached
    // the join's blocking wait when it died.
    void dispatchForTest (int signalOnlyFirst = -1, bool killedInJoin = false);

    // Test-only: wakes worker `lane` with nothing dispatched to it, as the
    // latch quiesce() leaves on a worker it found running its job does.
    void wakeLaneForTest (int lane) noexcept;

    // Test-only, Linux: the kernel thread id of every worker thread the pool
    // holds, active or parked, in lane order. Empty elsewhere.
    std::vector<std::int64_t> workerThreadIdsForTest() const;

    // Test-only: how many times worker `lane` has woken. A worker counts a
    // wake once it is done with it, before it acknowledges the epoch.
    std::uint32_t workerWakeCountForTest (int lane) const noexcept;

    // Times the per-block join blocked >2 s (a worker wedged in a plugin's
    // processBlock). Bumped RT-safely from runBlock; poll from a non-RT thread.
    int joinStallCount() const noexcept { return joinStalls.load (std::memory_order_relaxed); }

    // Whether the caller of runBlock is blocked in the join, waiting on a lane
    // past the short spin. A watchdog that sees a block run long reads it to
    // tell a lane's stall, which joinStallCount() counts, from one on the
    // caller's own thread. Any thread.
    bool isJoinBlocked() const noexcept { return joinBlocked.load (std::memory_order_relaxed); }

    // Test-only: one join stall, counted as runBlock counts one.
    void noteJoinStallForTest() noexcept { joinStalls.fetch_add (1, std::memory_order_relaxed); }

    // Test-only, with no runBlock in flight: how many 1 ms waits the join makes
    // before it counts a stall, 2000 otherwise.
    void setJoinStallWaitsForTest (int waits) noexcept { joinStallWaits = waits; }

private:
    struct Worker;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::function<void (int)> job_;
    // The most worker lanes a block dispatches; workers_ past it are parked.
    // Written by the message thread with no block in flight.
    std::atomic<int>      numWorkers { 0 };
    std::atomic<int>      done { 0 };
    // The dispatch epoch in the high 48 bits, equality-compared only, and the
    // worker lanes that epoch dispatched in the low 16. A lane no block
    // dispatches keeps its last ack until the next quiesce(), so the epoch must
    // not wrap within a stream: 32 bits would, after 17 days at 3000 blocks a
    // second.
    std::atomic<std::uint64_t> dispatchWord { 0 };
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free,
                   "the audio thread publishes the dispatch word without a lock");
    static constexpr std::uint64_t epochOf (std::uint64_t word) noexcept { return word >> 16; }
    static constexpr int lanesOf (std::uint64_t word) noexcept { return (int) (word & 0xFFFFu); }
    // The dispatcher only: a new epoch for `lanes` worker lanes, done reset.
    void publishDispatch (int lanes) noexcept;
    std::atomic<bool>     quiescing { false };
    std::atomic<bool>     quit { false };
    std::atomic<int>      joinStalls { 0 };    // count of >2 s join stalls (diagnostic)
    std::atomic<bool>     joinBlocked { false };
    int                   joinStallWaits = 2000;
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
