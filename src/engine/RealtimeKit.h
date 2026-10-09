#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::rt
{
// Raises threads of this process that could not take SCHED_RR themselves,
// because RLIMIT_RTPRIO forbids it. Realtime first, through RTKit over the
// system bus - the route PipeWire's own data thread takes in the same process,
// at the same priority RTKit grants it. Where RTKit refuses or is absent, the
// lowest nice level RTKit or RLIMIT_NICE allows.
//
// Linux, message thread only: it makes blocking D-Bus round trips, so it runs
// once per thread at start, never on a thread that has a deadline to keep.
// Returns a one-line description of what the threads got.
std::string raiseThreadsWithoutRtPrio (const std::vector<std::int64_t>& threadIds);

// RLIMIT_RTTIME bounds the CPU a realtime thread may take without blocking.
// Past the soft limit the kernel sends SIGXCPU, whose default action ends the
// process; at the hard limit it sends SIGKILL. A soft limit equal to the hard
// one, which RTKit asks for and PipeWire's module-rt sets, kills with no
// warning: one plug-in stalling a realtime lane takes the whole session down
// with no crash report and no autosave.
//
// This catches SIGXCPU and puts the soft limit a quarter below a finite hard
// one. The thread that runs into it is moved to SCHED_OTHER and carries on, so
// the stall costs dropouts instead of the process. A hard limit is never
// lowered here, nor a soft one someone already set below it, and a SIGXCPU
// handler that is not this one is left alone with the limits it expects.
//
// Linux, message thread. Idempotent and cheap: the engine calls it once a
// second, because module-rt sets soft == hard again whenever PipeWire takes
// realtime for a new data thread. True when a finite limit is guarded.
bool guardRealtimeCpuTime() noexcept;

struct RealtimeDemotions
{
    int          count        = 0;   // threads moved off realtime
    std::int64_t lastThreadId = 0;   // the latest of them
    int          unattributed = 0;   // SIGXCPU taken on a thread that was not realtime
};

// What the guard has done so far. Any thread.
RealtimeDemotions realtimeDemotions() noexcept;

// A thread's scheduling class as the kernel holds it: the policy without the
// reset-on-fork flag, and the priority. SCHED_OTHER for a thread that is gone.
struct ThreadScheduling
{
    int policy   = 0;
    int priority = 0;

    bool isRealtime() const noexcept;
};

// Linux, any thread of this process, by its kernel thread id.
ThreadScheduling threadScheduling (std::int64_t threadId) noexcept;

// True while the thread is blocked, not running or waiting for a core.
bool threadIsBlocked (std::int64_t threadId) noexcept;

// Puts threads the guard moved off realtime back on the realtime class each
// had: itself where RLIMIT_RTPRIO allows that, else through RTKit.
//
// Make one before holding anything up for it. Connecting to the system bus
// waits for as long as the bus takes; after that a restore() waits a quarter
// second at most for RTKit, and once RTKit has not put a thread back it is not
// asked again. Linux, message thread.
class RealtimeRestorer
{
public:
    RealtimeRestorer();
    ~RealtimeRestorer();
    RealtimeRestorer (const RealtimeRestorer&) = delete;
    RealtimeRestorer& operator= (const RealtimeRestorer&) = delete;

    // Puts `threadId` back on `was`. True when it runs realtime again.
    //
    // Only for a thread that is blocked (threadIsBlocked) and will stay so
    // until this returns. The kernel restarts a realtime thread's CPU-time
    // count when it wakes as one, and at no other time: put back while it
    // runs, a thread keeps the count its overrun reached and is warned, and
    // moved off again, at the next tick.
    bool restore (std::int64_t threadId, const ThreadScheduling& was);

private:
    struct Bus;
    std::unique_ptr<Bus> bus;
};
} // namespace duskstudio::rt
