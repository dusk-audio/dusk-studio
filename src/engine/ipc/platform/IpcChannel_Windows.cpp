#include "IpcChannel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
} // namespace

void closeHandle (NativeHandle& h) noexcept
{
    HANDLE w = asWinHandle (h);
    if (handleIsValid (w))
        ::CloseHandle (w);
    h.h = nullptr;
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
        PIPE_ACCESS_DUPLEX,
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
    while (n > 0)
    {
        DWORD got = 0;
        const BOOL ok = ::ReadFile (w, p, (DWORD) n, &got, nullptr);
        if (! ok || got == 0) return false;
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
        const BOOL ok = ::WriteFile (w, p, (DWORD) n, &wrote, nullptr);
        if (! ok || wrote == 0) return false;
        p += wrote;
        n -= (std::size_t) wrote;
    }
    return true;
}

bool setReadTimeout (NativeHandle& h, int /*ms*/) noexcept
{
    // Win32 named pipes don't have an SO_RCVTIMEO equivalent for
    // synchronous I/O. Phase 2 leaves this as a no-op; the parent
    // relies on the child terminating to unblock a hung read. Future
    // work: switch to overlapped I/O + WaitForSingleObject(timeout).
    (void) h;
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
    std::uint64_t value = 0;
    if (! readExact (channel, &value, sizeof (value))) return false;
    storeWinHandle (payloadOut, (HANDLE) (std::uintptr_t) value);
    return handleIsValid (asWinHandle (payloadOut));
}

} // namespace duskstudio::ipc::platform
