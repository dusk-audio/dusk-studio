#include "IpcSync.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <synchapi.h>

#include <chrono>
#include <cstdint>

// WaitOnAddress / WakeByAddressSingle (synchapi.h, Win8+). The wait
// function compares the value at `addr` against the value at
// `&compare`; if they differ on entry it returns immediately (mapping
// to WaitResult::ValueChanged). Otherwise it sleeps until a wake call
// targets the same address, or the timeout elapses.
//
// Works across processes when `addr` lies inside a shared mapping
// (CreateFileMapping + MapViewOfFile) - the kernel hashes the page
// frame, the same way Linux futex does.
//
// Link with synchronization.lib.

namespace duskstudio::ipc::platform
{

Deadline deadlineFromNow (long long nsFromNow) noexcept
{
    const auto target = std::chrono::steady_clock::now()
                       + std::chrono::nanoseconds (nsFromNow);
    const auto sinceEpoch = target.time_since_epoch();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds> (sinceEpoch);
    const auto remNs = sinceEpoch - secs;
    Deadline d;
    d.monotonicSec  = (std::int64_t) secs.count();
    d.monotonicNsec = (std::int32_t)
        std::chrono::duration_cast<std::chrono::nanoseconds> (remNs).count();
    return d;
}

namespace
{
DWORD deadlineToTimeoutMs (const Deadline& d) noexcept
{
    using namespace std::chrono;
    const auto target = seconds (d.monotonicSec) + nanoseconds (d.monotonicNsec);
    const auto now    = steady_clock::now().time_since_epoch();
    if (target <= now) return 0;
    const auto delta = target - now;
    const auto ms = duration_cast<milliseconds> (delta).count();
    if (ms < 0)             return 0;
    if (ms > 0x7fffffffLL)  return 0x7fffffff;
    return (DWORD) ms;
}
} // namespace

WaitResult waitOnAddress (std::atomic<std::uint32_t>* addr,
                            std::uint32_t expected,
                            const Deadline* deadline) noexcept
{
    const DWORD timeoutMs = (deadline != nullptr)
                              ? deadlineToTimeoutMs (*deadline)
                              : INFINITE;

    // Don't short-circuit on timeoutMs == 0 - pass it through so the
    // value-comparison in WaitOnAddress still runs; otherwise a wake
    // that arrived between the caller's load and our call would look
    // like a timeout to the caller and trigger a spurious crash.
    const BOOL ok = ::WaitOnAddress (addr, &expected, sizeof (expected), timeoutMs);
    if (ok) return WaitResult::Awoken;
    if (::GetLastError() == ERROR_TIMEOUT) return WaitResult::Timeout;
    return WaitResult::Error;
}

void wakeOneAddress (std::atomic<std::uint32_t>* addr) noexcept
{
    ::WakeByAddressSingle (addr);
}

void cpuRelax() noexcept
{
    YieldProcessor();
}

} // namespace duskstudio::ipc::platform
