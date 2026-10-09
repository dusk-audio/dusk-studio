#pragma once

#include <cstdint>
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
} // namespace duskstudio::rt
