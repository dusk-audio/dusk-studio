#include "IpcSync.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

// WaitOnAddress / WakeByAddressSingle can wake only threads in the calling
// process. Windows therefore uses an unnamed auto-reset event created as
// inheritable before CreateProcess. Both processes retain a handle to the same
// kernel object; the shared sequence word still supplies the value predicate
// and recovers safely from coalesced or early SetEvent calls.

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
    // WaitForSingleObject takes whole milliseconds. Round upward so a positive
    // sub-millisecond remainder cannot expire before the absolute deadline.
    const auto ms = duration_cast<milliseconds> (delta + nanoseconds (999999)).count();
    if (ms < 0)             return 0;
    if (ms > 0x7fffffffLL)  return 0x7fffffff;
    return (DWORD) ms;
}
} // namespace

InterprocessSignal::~InterprocessSignal()
{
    close();
}

bool InterprocessSignal::create (std::string& errorOut) noexcept
{
    close();

    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = TRUE;

    nativeHandle.h = ::CreateEventA (&sa, FALSE, FALSE, nullptr);
    if (isValid (nativeHandle)) return true;

    char buf[128];
    std::snprintf (buf, sizeof (buf), "CreateEvent failed: %lu",
                   (unsigned long) ::GetLastError());
    errorOut = buf;
    nativeHandle.h = nullptr;
    return false;
}

bool InterprocessSignal::sendToChild (NativeHandle& channel) const noexcept
{
    return sendHandle (channel, nativeHandle);
}

bool InterprocessSignal::receiveFromParent (NativeHandle& channel) noexcept
{
    close();
    return recvHandle (channel, nativeHandle);
}

void InterprocessSignal::close() noexcept
{
    closeHandle (nativeHandle);
}

WaitResult InterprocessSignal::wait (std::atomic<std::uint32_t>* addr,
                                      std::uint32_t expected,
                                      const Deadline* deadline) noexcept
{
    if (! isValid (nativeHandle)) return WaitResult::Error;
    if (addr->load (std::memory_order_acquire) != expected)
        return WaitResult::ValueChanged;

    const DWORD timeoutMs = deadline != nullptr
                              ? deadlineToTimeoutMs (*deadline)
                              : INFINITE;
    const DWORD result = ::WaitForSingleObject (
        reinterpret_cast<HANDLE> (nativeHandle.h), timeoutMs);
    if (result == WAIT_OBJECT_0) return WaitResult::Awoken;
    if (result == WAIT_TIMEOUT)  return WaitResult::Timeout;
    return WaitResult::Error;
}

void InterprocessSignal::wake (std::atomic<std::uint32_t>*) noexcept
{
    if (isValid (nativeHandle))
        (void) ::SetEvent (reinterpret_cast<HANDLE> (nativeHandle.h));
}

void cpuRelax() noexcept
{
    YieldProcessor();
}

} // namespace duskstudio::ipc::platform
