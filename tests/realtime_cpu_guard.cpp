#include <catch2/catch_test_macros.hpp>

#include "engine/RealtimeKit.h"

#include <csignal>
#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// RLIMIT_RTTIME is process-wide and a lowered hard limit cannot be raised again,
// so every case runs in a forked child and reports through its exit status. A
// child the kernel kills shows up here as a signal rather than taking the test
// binary with it.

namespace
{
constexpr int kNoRealtime = 77;
// RTKit's default RTTimeUSecMax.
constexpr rlim_t kHardMicros = 200000;

template <typename Body>
int statusOfChild (Body body)
{
    const pid_t child = fork();
    if (child == 0)
        _exit (body());
    int status = 0;
    if (child < 0 || waitpid (child, &status, 0) != child)
        return -1;
    return status;
}

double threadCpuSeconds() noexcept
{
    timespec now {};
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &now);
    return (double) now.tv_sec + (double) now.tv_nsec * 1.0e-9;
}

bool takeRoundRobin() noexcept
{
    sched_param param {};
    param.sched_priority = sched_get_priority_min (SCHED_RR);
    return pthread_setschedparam (pthread_self(), SCHED_RR, &param) == 0;
}

bool isRealtime() noexcept
{
    const int policy = sched_getscheduler (0) & ~SCHED_RESET_ON_FORK;
    return policy == SCHED_RR || policy == SCHED_FIFO;
}

// Never blocks: clock_gettime on the thread clock stays in the vDSO.
void spinFor (double seconds) noexcept
{
    const double start = threadCpuSeconds();
    volatile double sink = 0.0;
    while (threadCpuSeconds() - start < seconds)
        sink = sink + 1.0;
}
} // namespace

TEST_CASE ("a realtime thread that stops blocking is moved off realtime, not killed",
           "[rt-priority][rttime]")
{
    const int status = statusOfChild ([]
    {
        // The shape RTKit asks for and PipeWire's module-rt sets.
        const rlimit limit { kHardMicros, kHardMicros };
        if (setrlimit (RLIMIT_RTTIME, &limit) != 0) return 2;
        duskstudio::rt::guardRealtimeCpuTime();
        if (! takeRoundRobin()) return kNoRealtime;

        // Twice the hard limit without blocking: a plug-in stuck in process().
        spinFor (2.0 * (double) kHardMicros * 1.0e-6);
        const auto first = duskstudio::rt::realtimeDemotions();
        if (isRealtime() || first.count != 1
            || first.lastThreadId != (std::int64_t) syscall (SYS_gettid))
            return 4;

        // The kernel moves the soft limit on by a second each time it warns.
        // A second overrun has to be warned about too, not killed at the hard
        // limit the first one left in reach.
        if (! takeRoundRobin()) return 5;
        spinFor (2.0 * (double) kHardMicros * 1.0e-6);
        if (isRealtime() || duskstudio::rt::realtimeDemotions().count != 2) return 6;
        return 0;
    });

    if (WIFEXITED (status) && WEXITSTATUS (status) == kNoRealtime)
        SKIP ("RLIMIT_RTPRIO allows no SCHED_RR here, and the test build has no RTKit");
    INFO ("child status " << status << (WIFSIGNALED (status) ? " (killed by signal " : " (exit ")
          << (WIFSIGNALED (status) ? WTERMSIG (status) : WEXITSTATUS (status)) << ")");
    REQUIRE_FALSE (WIFSIGNALED (status));
    REQUIRE (WIFEXITED (status));
    REQUIRE (WEXITSTATUS (status) == 0);
}

TEST_CASE ("the realtime CPU guard keeps limits and handlers it did not set",
           "[rt-priority][rttime]")
{
    SECTION ("a soft limit already below the hard one stays where it is")
    {
        const int status = statusOfChild ([]
        {
            const rlimit limit { kHardMicros / 2, kHardMicros };
            if (setrlimit (RLIMIT_RTTIME, &limit) != 0) return 2;
            if (! duskstudio::rt::guardRealtimeCpuTime()) return 3;
            rlimit after {};
            getrlimit (RLIMIT_RTTIME, &after);
            return after.rlim_cur == kHardMicros / 2 && after.rlim_max == kHardMicros ? 0 : 4;
        });
        REQUIRE (WIFEXITED (status));
        REQUIRE (WEXITSTATUS (status) == 0);
    }

    SECTION ("a soft limit equal to the hard one gets a warning step, the hard one is kept")
    {
        const int status = statusOfChild ([]
        {
            const rlimit limit { kHardMicros, kHardMicros };
            if (setrlimit (RLIMIT_RTTIME, &limit) != 0) return 2;
            if (! duskstudio::rt::guardRealtimeCpuTime()) return 3;
            rlimit after {};
            getrlimit (RLIMIT_RTTIME, &after);
            return after.rlim_cur < kHardMicros && after.rlim_cur > 0
                && after.rlim_max == kHardMicros ? 0 : 4;
        });
        REQUIRE (WIFEXITED (status));
        REQUIRE (WEXITSTATUS (status) == 0);
    }

    SECTION ("someone else's SIGXCPU handler is left with the limits it expects")
    {
        const int status = statusOfChild ([]
        {
            struct sigaction theirs {};
            theirs.sa_handler = [] (int) {};
            sigemptyset (&theirs.sa_mask);
            if (sigaction (SIGXCPU, &theirs, nullptr) != 0) return 2;
            const rlimit limit { kHardMicros, kHardMicros };
            if (setrlimit (RLIMIT_RTTIME, &limit) != 0) return 2;
            if (duskstudio::rt::guardRealtimeCpuTime()) return 3;
            rlimit after {};
            getrlimit (RLIMIT_RTTIME, &after);
            struct sigaction now {};
            sigaction (SIGXCPU, nullptr, &now);
            return after.rlim_cur == kHardMicros && now.sa_handler == theirs.sa_handler ? 0 : 4;
        });
        REQUIRE (WIFEXITED (status));
        REQUIRE (WEXITSTATUS (status) == 0);
    }

    SECTION ("no limit, nothing to guard")
    {
        const int status = statusOfChild ([]
        {
            rlimit before {};
            getrlimit (RLIMIT_RTTIME, &before);
            if (before.rlim_cur != RLIM_INFINITY) return 0;   // the box set one; nothing to show
            return duskstudio::rt::guardRealtimeCpuTime() ? 3 : 0;
        });
        REQUIRE (WIFEXITED (status));
        REQUIRE (WEXITSTATUS (status) == 0);
    }
}
