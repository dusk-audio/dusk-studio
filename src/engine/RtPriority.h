#pragma once

#include <algorithm>

#if defined(__linux__)
 #include <pthread.h>
 #include <sched.h>
 #include <sys/resource.h>
#endif

namespace duskstudio::rt
{
// JUCE maps a realtime priority p in [0, 10] onto a kernel SCHED_RR
// sched_priority via jmap (p, 0, 10, min, max) - integer-truncating, 1..99 on
// Linux. pthread_setschedparam refuses a sched_priority above the soft
// RLIMIT_RTPRIO ceiling with EPERM, and JUCE then silently drops the thread to
// SCHED_OTHER. Every realtime thread in the engine (ALSA I/O thread, DSP worker
// pool) must therefore request a priority whose MAPPED value fits under the
// ceiling - and they must all use the same computation, or they end up at
// different SCHED_RR levels and the audio thread's join can starve a worker
// sharing its core (RR only round-robins between equal priorities).

struct RtPriorityInfo
{
    int       jucePriority;   // -1 = realtime not attainable; else a valid [0, 10]
    bool      haveRtLimit;
    long long rtLimit;        // -1 when unknown / unlimited
};

// Largest p in [0, 10] whose forward-mapped sched_priority fits under
// ceilingSched. Walks down from 10 against the forward map directly - the
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

// Puts the CALLING thread on SCHED_RR at the sched_priority that a JUCE
// realtime priority p in [0, 10] maps to (rrMin + p·span/10, the same forward
// map queryRealtimePriority walks) - the native equivalent of JUCE's
// startRealtimeThread(RealtimeOptions().withPriority(p)). Returns false if the
// kernel refuses (EPERM over RLIMIT_RTPRIO, non-Linux), leaving the thread at
// its default scheduling class.
inline bool applyRealtimeSchedRR (int jucePriority) noexcept
{
   #if defined(__linux__)
    const int rrMin = std::max (0, sched_get_priority_min (SCHED_RR));
    const int rrMax = std::max (1, sched_get_priority_max (SCHED_RR));
    const int p     = std::clamp (jucePriority, 0, 10);
    sched_param sp {};
    sp.sched_priority = rrMin + (p * (rrMax - rrMin)) / 10;
    return pthread_setschedparam (pthread_self(), SCHED_RR, &sp) == 0;
   #else
    (void) jucePriority;
    return false;
   #endif
}

inline RtPriorityInfo queryRealtimePriority() noexcept
{
   #if defined(__linux__)
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
