#include "IpcSync.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#include <thread>

// macOS 14.4+ provides os_sync_wait_on_address - equivalent to
// Linux's FUTEX_WAIT_BITSET / Windows' WaitOnAddress, and supports
// cross-process wait/wake when the address is in a shared mapping
// via the OS_SYNC_WAIT_ON_ADDRESS_SHARED flag.
//
// Older macOS would need a fallback (named sem_open + sem_wait, or
// the private __ulock_wait/__ulock_wake APIs); for now Dusk Studio's
// OOP host requires macOS 14.4 and the in-process fallback handles
// older systems via DUSKSTUDIO_HAS_OOP_PLUGINS being undefined.

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

namespace
{
std::uint64_t remainingNs (const Deadline& d) noexcept
{
    struct timespec now {};
    ::clock_gettime (CLOCK_MONOTONIC, &now);
    const long long deltaSec = (long long) d.monotonicSec - (long long) now.tv_sec;
    const long long deltaNs  = (long long) d.monotonicNsec - (long long) now.tv_nsec;
    const long long total    = deltaSec * 1000000000LL + deltaNs;
    if (total <= 0) return 0;
    return (std::uint64_t) total;
}
} // namespace

WaitResult waitOnAddress (std::atomic<std::uint32_t>* addr,
                            std::uint32_t expected,
                            const Deadline* deadline) noexcept
{
    constexpr os_sync_wait_on_address_flags_t flags = OS_SYNC_WAIT_ON_ADDRESS_SHARED;

    int r;
    if (deadline == nullptr)
    {
        r = ::os_sync_wait_on_address (addr, (uint64_t) expected,
                                          sizeof (expected), flags);
    }
    else
    {
        const std::uint64_t ns = remainingNs (*deadline);
        r = ::os_sync_wait_on_address_with_timeout (
                addr, (uint64_t) expected, sizeof (expected), flags,
                OS_CLOCK_MACH_ABSOLUTE_TIME, ns);
    }

    if (r >= 0) return WaitResult::Awoken;
    switch (errno)
    {
        case ETIMEDOUT: return WaitResult::Timeout;
        case EINTR:     return WaitResult::Interrupted;
        default:        return WaitResult::Error;
    }
}

void wakeOneAddress (std::atomic<std::uint32_t>* addr) noexcept
{
    (void) ::os_sync_wake_by_address_any (addr, sizeof (std::uint32_t),
                                             OS_SYNC_WAKE_BY_ADDRESS_SHARED);
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
