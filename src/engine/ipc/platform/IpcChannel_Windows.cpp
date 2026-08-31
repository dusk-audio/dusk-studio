#include "IpcChannel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

// Windows uses a duplex named pipe instead of a Unix-domain socketpair.
// The pipe name is unique per pair (process id + atomic counter) so
// concurrent ChannelPairs never collide; the name is only used for the
// CreateFile/CreateNamedPipe handshake and never escapes this file.
//
// Handle passing: on Windows, a handle is "sent" to the peer by writing
// its 64-bit HANDLE value down the same pipe. The handle has to be
// marked SECURITY_ATTRIBUTES.bInheritHandle = TRUE before CreateProcess
// (bInheritHandles = TRUE) so the child inherits it at the same numeric
// value. SharedMemory::createAnonymous + createChannelPair (childEnd)
// both produce inheritable handles, so the value the parent sends down
// the pipe is already valid in the child's handle table on the other
// side.

namespace duskstudio::ipc::platform
{
namespace
{
inline HANDLE asWinHandle (const NativeHandle& h) noexcept
{
    return reinterpret_cast<HANDLE> (h.h);
}

inline void storeWinHandle (NativeHandle& dst, HANDLE src) noexcept
{
    dst.h = reinterpret_cast<void*> (src);
}

inline bool handleIsValid (HANDLE h) noexcept
{
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

DWORD timeoutUntil (std::chrono::steady_clock::time_point deadline) noexcept
{
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (deadline <= now) return 0;
    const auto remaining = deadline - now;
    const auto ms = duration_cast<milliseconds> (remaining + nanoseconds (999999)).count();
    return ms > (long long) std::numeric_limits<DWORD>::max()
             ? std::numeric_limits<DWORD>::max()
             : (DWORD) ms;
}

bool finishOverlapped (HANDLE handle, OVERLAPPED& operation,
                       DWORD timeoutMs, DWORD& transferred) noexcept
{
    const DWORD waitResult = ::WaitForSingleObject (operation.hEvent, timeoutMs);
    if (waitResult == WAIT_OBJECT_0)
        return ::GetOverlappedResult (handle, &operation, &transferred, FALSE) != FALSE;

    // OVERLAPPED and its event must stay alive until cancellation has
    // completed, including the race where the operation finishes just before
    // CancelIoEx observes it. Apply the same cleanup to WAIT_FAILED.
    (void) ::CancelIoEx (handle, &operation);
    (void) ::WaitForSingleObject (operation.hEvent, INFINITE);
    DWORD ignored = 0;
    (void) ::GetOverlappedResult (handle, &operation, &ignored, FALSE);
    return false;
}

bool readOverlapped (HANDLE handle, void* buffer, DWORD size,
                     DWORD timeoutMs, DWORD& transferred) noexcept
{
    OVERLAPPED operation {};
    operation.hEvent = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);
    if (operation.hEvent == nullptr) return false;

    const BOOL started = ::ReadFile (handle, buffer, size, nullptr, &operation);
    bool ok = false;
    if (started != FALSE)
        ok = ::GetOverlappedResult (handle, &operation, &transferred, FALSE) != FALSE;
    else if (::GetLastError() == ERROR_IO_PENDING)
        ok = finishOverlapped (handle, operation, timeoutMs, transferred);

    ::CloseHandle (operation.hEvent);
    return ok;
}

bool writeOverlapped (HANDLE handle, const void* buffer, DWORD size,
                      DWORD& transferred) noexcept
{
    OVERLAPPED operation {};
    operation.hEvent = ::CreateEventW (nullptr, TRUE, FALSE, nullptr);
    if (operation.hEvent == nullptr) return false;

    const BOOL started = ::WriteFile (handle, buffer, size, nullptr, &operation);
    bool ok = false;
    if (started != FALSE)
        ok = ::GetOverlappedResult (handle, &operation, &transferred, FALSE) != FALSE;
    else if (::GetLastError() == ERROR_IO_PENDING)
        ok = finishOverlapped (handle, operation, INFINITE, transferred);

    ::CloseHandle (operation.hEvent);
    return ok;
}
} // namespace

void closeHandle (NativeHandle& h) noexcept
{
    HANDLE w = asWinHandle (h);
    if (handleIsValid (w))
        ::CloseHandle (w);
    h.h = nullptr;
    h.overlapped = false;
    h.readTimeoutMs = 0;
}

bool createChannelPair (ChannelPair& out, std::string& errorOut) noexcept
{
    static std::atomic<std::uint64_t> counter { 0 };
    const auto seq = counter.fetch_add (1, std::memory_order_relaxed);

    char pipeName[128];
    std::snprintf (pipeName, sizeof (pipeName),
                    R"(\\.\pipe\dusk-studio-ipc-%lu-%llu)",
                    (unsigned long) ::GetCurrentProcessId(),
                    (unsigned long long) seq);

    HANDLE server = ::CreateNamedPipeA (
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                  // max instances
        64 * 1024,          // out buffer
        64 * 1024,          // in buffer
        0,                  // default timeout
        nullptr);

    if (! handleIsValid (server))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "CreateNamedPipe failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        return false;
    }

    SECURITY_ATTRIBUTES sa {};
    sa.nLength              = sizeof (sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE client = ::CreateFileA (
        pipeName,
        GENERIC_READ | GENERIC_WRITE,
        0,                  // no sharing
        &sa,                // inheritable
        OPEN_EXISTING,
        0,
        nullptr);

    if (! handleIsValid (client))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "CreateFile (pipe client) failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        ::CloseHandle (server);
        return false;
    }

    storeWinHandle (out.parentEnd, server);
    out.parentEnd.overlapped = true;
    storeWinHandle (out.childEnd,  client);
    return true;
}

bool moveHandleToFd (NativeHandle&, int) noexcept
{
    // Windows uses inheritance + CreateProcess(bInheritHandles=TRUE)
    // rather than a known fd number. No-op success; the child finds
    // its inherited channel end via the spawn-side wiring.
    return true;
}

NativeHandle locateInheritedChannel (int argc, const char* const* argv) noexcept
{
    NativeHandle h;
    const char* const kPrefix = "--ipc-channel=";
    const std::size_t prefixLen = std::strlen (kPrefix);
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] != nullptr && std::strncmp (argv[i], kPrefix, prefixLen) == 0)
        {
            const char* tail = argv[i] + prefixLen;
            std::uint64_t value = 0;
            // Accept 0x-prefixed hex; fall back to plain hex parsing.
            if (tail[0] == '0' && (tail[1] == 'x' || tail[1] == 'X')) tail += 2;
            while (*tail != '\0')
            {
                value <<= 4;
                const char c = *tail++;
                if (c >= '0' && c <= '9')       value |= (std::uint64_t) (c - '0');
                else if (c >= 'a' && c <= 'f')  value |= (std::uint64_t) (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')  value |= (std::uint64_t) (c - 'A' + 10);
                else { return NativeHandle{}; }
            }
            storeWinHandle (h, (HANDLE) (std::uintptr_t) value);
            return h;
        }
    }
    return h;
}

bool readExact (NativeHandle& h, void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<char*> (buf);
    HANDLE w = asWinHandle (h);
    const auto deadline = h.readTimeoutMs > 0
                            ? std::chrono::steady_clock::now()
                                + std::chrono::milliseconds (h.readTimeoutMs)
                            : std::chrono::steady_clock::time_point::max();
    while (n > 0)
    {
        DWORD got = 0;
        const DWORD chunk = n > (std::size_t) std::numeric_limits<DWORD>::max()
                              ? std::numeric_limits<DWORD>::max()
                              : (DWORD) n;
        if (h.overlapped)
        {
            const DWORD timeout = h.readTimeoutMs > 0 ? timeoutUntil (deadline) : INFINITE;
            if (! readOverlapped (w, p, chunk, timeout, got) || got == 0)
                return false;
        }
        else
        {
            const BOOL ok = ::ReadFile (w, p, chunk, &got, nullptr);
            if (! ok || got == 0) return false;
        }
        p += got;
        n -= (std::size_t) got;
    }
    return true;
}

bool writeExact (NativeHandle& h, const void* buf, std::size_t n) noexcept
{
    auto* p = static_cast<const char*> (buf);
    HANDLE w = asWinHandle (h);
    while (n > 0)
    {
        DWORD wrote = 0;
        const DWORD chunk = n > (std::size_t) std::numeric_limits<DWORD>::max()
                              ? std::numeric_limits<DWORD>::max()
                              : (DWORD) n;
        if (h.overlapped)
        {
            if (! writeOverlapped (w, p, chunk, wrote) || wrote == 0)
                return false;
        }
        else
        {
            const BOOL ok = ::WriteFile (w, p, chunk, &wrote, nullptr);
            if (! ok || wrote == 0) return false;
        }
        p += wrote;
        n -= (std::size_t) wrote;
    }
    return true;
}

bool setReadTimeout (NativeHandle& h, int ms) noexcept
{
    if (! isValid (h) || ms < 0) return false;
    // A positive timeout can only be honoured on a handle opened with
    // FILE_FLAG_OVERLAPPED. Refuse false confidence on synchronous handles.
    if (ms > 0 && ! h.overlapped) return false;
    h.readTimeoutMs = ms;
    return true;
}

bool sendHandle (NativeHandle& channel, const NativeHandle& payload) noexcept
{
    const std::uint64_t value = (std::uint64_t) (std::uintptr_t) asWinHandle (payload);
    return writeExact (channel, &value, sizeof (value));
}

bool recvHandle (NativeHandle& channel, NativeHandle& payloadOut) noexcept
{
    payloadOut.h = nullptr;
    payloadOut.overlapped = false;
    payloadOut.readTimeoutMs = 0;
    std::uint64_t value = 0;
    if (! readExact (channel, &value, sizeof (value))) return false;
    storeWinHandle (payloadOut, (HANDLE) (std::uintptr_t) value);
    return handleIsValid (asWinHandle (payloadOut));
}

} // namespace duskstudio::ipc::platform
