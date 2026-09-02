#include "IpcSync.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <thread>
#include <unistd.h>

// macOS gained a public shared-address wait only in 14.4. Use a pipe instead
// so the release can retain its macOS 11 deployment target. Both processes own
// duplicates of both descriptors: wake() is a non-blocking write, and wait()
// polls the read end up to the same monotonic deadline used by the other
// platforms. The shared sequence word remains the predicate, so stale or
// coalesced pipe bytes are harmless.

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

bool configurePipeDescriptor (int fd) noexcept
{
    const int descriptorFlags = ::fcntl (fd, F_GETFD);
    if (descriptorFlags < 0
        || ::fcntl (fd, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)
        return false;

    const int statusFlags = ::fcntl (fd, F_GETFL);
    return statusFlags >= 0
        && ::fcntl (fd, F_SETFL, statusFlags | O_NONBLOCK) == 0;
}

int deadlineToTimeoutMs (const Deadline& deadline) noexcept
{
    const std::uint64_t ns = remainingNs (deadline);
    if (ns == 0) return 0;

    const std::uint64_t ms = ns / 1000000ULL + (ns % 1000000ULL != 0 ? 1ULL : 0ULL);
    const auto max = (std::uint64_t) std::numeric_limits<int>::max();
    return (int) (ms < max ? ms : max);
}
} // namespace

InterprocessSignal::~InterprocessSignal()
{
    close();
}

bool InterprocessSignal::create (std::string& errorOut) noexcept
{
    close();

    int descriptors[2] { -1, -1 };
    if (::pipe (descriptors) != 0)
    {
        errorOut = std::string ("pipe failed: ") + std::strerror (errno);
        return false;
    }

    readHandle.fd  = descriptors[0];
    writeHandle.fd = descriptors[1];
    if (configurePipeDescriptor (readHandle.fd)
        && configurePipeDescriptor (writeHandle.fd))
        return true;

    const int error = errno;
    close();
    errorOut = std::string ("pipe setup failed: ") + std::strerror (error);
    return false;
}

bool InterprocessSignal::sendToChild (NativeHandle& channel) const noexcept
{
    return isValid (readHandle) && isValid (writeHandle)
        && sendHandle (channel, readHandle)
        && sendHandle (channel, writeHandle);
}

bool InterprocessSignal::receiveFromParent (NativeHandle& channel) noexcept
{
    close();
    if (! recvHandle (channel, readHandle)
        || ! recvHandle (channel, writeHandle)
        || ! configurePipeDescriptor (readHandle.fd)
        || ! configurePipeDescriptor (writeHandle.fd))
    {
        close();
        return false;
    }
    return true;
}

void InterprocessSignal::close() noexcept
{
    closeHandle (readHandle);
    closeHandle (writeHandle);
}

WaitResult InterprocessSignal::wait (std::atomic<std::uint32_t>* addr,
                                      std::uint32_t expected,
                                      const Deadline* deadline) noexcept
{
    if (! isValid (readHandle)) return WaitResult::Error;
    if (addr->load (std::memory_order_acquire) != expected)
        return WaitResult::ValueChanged;

    struct pollfd descriptor { readHandle.fd, POLLIN, 0 };
    const int timeoutMs = deadline != nullptr ? deadlineToTimeoutMs (*deadline) : -1;
    const int result = ::poll (&descriptor, 1, timeoutMs);
    if (result == 0) return WaitResult::Timeout;
    if (result < 0)
    {
        return errno == EINTR ? WaitResult::Interrupted : WaitResult::Error;
    }

    if ((descriptor.revents & POLLIN) != 0)
    {
        // Fast producer/consumer pairs often observe the sequence during the
        // spin phase and never enter wait(), leaving old wake bytes queued.
        // Drain them together so the first later wait cannot spend one syscall
        // per completed audio block. A concurrent new wake either lands in
        // this drain (the sequence re-check observes it) or remains readable.
        std::uint8_t bytes[256];
        for (;;)
        {
            const ssize_t count = ::read (readHandle.fd, bytes, sizeof (bytes));
            if (count > 0) continue;
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return WaitResult::Awoken;
            return WaitResult::Error;
        }
    }

    return WaitResult::Error;
}

void InterprocessSignal::wake (std::atomic<std::uint32_t>*) noexcept
{
    if (! isValid (writeHandle)) return;

    const std::uint8_t byte = 1;
    for (;;)
    {
        if (::write (writeHandle.fd, &byte, sizeof (byte)) == (ssize_t) sizeof (byte))
            return;
        if (errno == EINTR) continue;
        // EAGAIN means at least one wake is already pending. The sequence word
        // carries the actual state, so another byte is unnecessary.
        return;
    }
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
