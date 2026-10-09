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
} // namespace duskstudio::rt
