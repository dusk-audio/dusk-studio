#include "IpcSync.h"

#include <cerrno>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace duskstudio::ipc::platform
{

Deadline deadlineFromNow (long long nsFromNow) noexcept
{
    struct timespec now {};
    ::clock_gettime (CLOCK_MONOTONIC, &now);
    const long long total = (long long) now.tv_nsec + nsFromNow;
    const long long secs  = total / 1000000000LL;
    const long long rem   = total % 1000000000LL;
    Deadline d;
    d.monotonicSec  = (std::int64_t) now.tv_sec + (std::int64_t) secs;
    d.monotonicNsec = (std::int32_t) rem;
    return d;
}

WaitResult waitOnAddress (std::atomic<std::uint32_t>* addr,
                            std::uint32_t expected,
                            const Deadline* deadline) noexcept
{
    struct timespec abs {};
    const struct timespec* absPtr = nullptr;
    if (deadline != nullptr)
    {
        abs.tv_sec  = (time_t) deadline->monotonicSec;
        abs.tv_nsec = (long)   deadline->monotonicNsec;
        absPtr = &abs;
    }

    const long r = ::syscall (SYS_futex, addr,
                                 FUTEX_WAIT_BITSET,
                                 expected, absPtr, nullptr,
                                 FUTEX_BITSET_MATCH_ANY);
    if (r == 0) return WaitResult::Awoken;
    switch (errno)
    {
        case EAGAIN:    return WaitResult::ValueChanged;
        case ETIMEDOUT: return WaitResult::Timeout;
        case EINTR:     return WaitResult::Interrupted;
        default:        return WaitResult::Error;
    }
}

void wakeOneAddress (std::atomic<std::uint32_t>* addr) noexcept
{
    (void) ::syscall (SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
}

void cpuRelax() noexcept
{
   #if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
   #elif defined(__aarch64__) || defined(__arm__)
    asm volatile ("yield" ::: "memory");
   #else
    std::this_thread::yield();
   #endif
}

} // namespace duskstudio::ipc::platform
