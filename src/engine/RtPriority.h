#pragma once

#include <algorithm>

#if JUCE_LINUX
 #include <sched.h>
 #include <sys/resource.h>
#endif

namespace duskstudio::rt
{
// JUCE maps a realtime priority p in [0, 10] onto a kernel SCHED_RR
// sched_priority via jmap (p, 0, 10, min, max) — integer-truncating, 1..99 on
// Linux. pthread_setschedparam refuses a sched_priority above the soft
// RLIMIT_RTPRIO ceiling with EPERM, and JUCE then silently drops the thread to
// SCHED_OTHER. Every realtime thread in the engine (ALSA I/O thread, DSP worker
// pool) must therefore request a priority whose MAPPED value fits under the
// ceiling — and they must all use the same computation, or they end up at
// different SCHED_RR levels and the audio thread's join can starve a worker
// sharing its core (RR only round-robins between equal priorities).

struct RtPriorityInfo
{
    int       jucePriority;   // -1 = realtime not attainable; else a valid [0, 10]
    bool      haveRtLimit;
    long long rtLimit;        // -1 when unknown / unlimited
};

// Largest p in [0, 10] whose forward-mapped sched_priority fits under
// ceilingSched. Walks down from 10 against the forward map directly — the
// closed-form inverse mis-rounds in both directions under integer division.
// Returns -1 when nothing fits: 0 is a VALID JUCE priority (maps to the lowest
// SCHED_RR level) so it cannot double as the "no RT" sentinel.
inline int jucePriorityForSchedCeiling (int ceilingSched, int rrMin, int rrMax) noexcept
{
    if (rrMax <= rrMin || ceilingSched < rrMin)
        return -1;

    const int span = rrMax - rrMin;
    int p = 10;
    while (p >= 0 && rrMin + (p * span) / 10 > ceilingSched)
        --p;
    return std::max (-1, p);
}

inline RtPriorityInfo queryRealtimePriority() noexcept
{
   #if JUCE_LINUX
    struct rlimit rl {};
    if (getrlimit (RLIMIT_RTPRIO, &rl) != 0)
        return { -1, false, -1 };

    const int rrMin = std::max (0, sched_get_priority_min (SCHED_RR));
    const int rrMax = std::max (1, sched_get_priority_max (SCHED_RR));

    if (rl.rlim_cur == RLIM_INFINITY)
        return { jucePriorityForSchedCeiling (rrMax, rrMin, rrMax), true, -1 };

    const auto limit = (long long) rl.rlim_cur;
    return { jucePriorityForSchedCeiling ((int) std::min (limit, (long long) rrMax),
                                          rrMin, rrMax),
             true, limit };
   #else
    // macOS / Windows: JUCE's realtime path doesn't go through SCHED_RR
    // rlimits; the default RealtimeOptions priority is appropriate.
    return { 5, false, -1 };
   #endif
}
} // namespace duskstudio::rt
