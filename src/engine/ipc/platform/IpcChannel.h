#pragma once

#include <cstddef>
#include <string>

// Platform-abstracted IPC channel primitives. The "channel" is a pair of
// connected endpoints used for control-plane request/reply between the
// parent (Dusk Studio) and the child (dusk-studio-plugin-host). It is NOT
// the hot audio path - audio goes through shared memory + the IpcSync
// wake/wait primitives. Channel I/O happens on the message thread (parent)
// and a dedicated socket-reader thread (child), so blocking calls are OK.
//
// Per-platform impls live in IpcChannel_Linux.cpp / _Mac.mm / _Windows.cpp.
//
// Linux  : socketpair(AF_UNIX, SOCK_STREAM) for the pair; SCM_RIGHTS for
//          handle handoff (used to pass the SHM memfd to the child).
// macOS  : Mach port pair (or socketpair as a fallback); mach_msg send-right
//          for handle handoff.
// Windows: anonymous duplex named pipe; DuplicateHandle for handle handoff.

namespace duskstudio::ipc::platform
{

// Opaque per-platform handle. Public-by-layout so the impl files can
// construct it directly; callers go through the free functions below.
//
// Linux / macOS: file descriptor.
// Windows      : Win32 HANDLE (stored as void* so this header stays
//                Windows.h-free; the impl casts via reinterpret_cast).
struct NativeHandle
{
   #if defined(_WIN32)
    void* h { nullptr };
   #else
    int fd { -1 };
   #endif
};

inline bool isValid (const NativeHandle& h) noexcept
{
   #if defined(_WIN32)
    return h.h != nullptr && h.h != reinterpret_cast<void*> (-1);
   #else
    return h.fd >= 0;
   #endif
}

// Close + invalidate. Idempotent - closing an already-invalid handle is
// a no-op. On Linux: ::close(fd).
void closeHandle (NativeHandle& h) noexcept;

// Connected pair of channel endpoints. After construction, the parent
// keeps `parentEnd` and the child inherits `childEnd` at a known fd
// number (kChildInheritFd) via the process spawn path.
struct ChannelPair
{
    NativeHandle parentEnd;
    NativeHandle childEnd;
};

bool createChannelPair (ChannelPair& out, std::string& errorOut) noexcept;

// Fd number where the Linux child finds the inherited channel endpoint
// after exec. Linux uses dup2() to land the channel here. Windows uses
// CreateProcess(bInheritHandles=TRUE) instead and passes the inherited
// HANDLE value on the command line; locateInheritedChannel() abstracts
// over both.
constexpr int kChildInheritFd = 3;

// Move the given handle so its underlying fd is exactly `targetFd`.
// Linux: dup2() + close-source. Used in the forked child between
// fork() and execv() so the channel lands at kChildInheritFd. Windows:
// no-op success (handle inheritance + CLI value used instead).
bool moveHandleToFd (NativeHandle& h, int targetFd) noexcept;

// Child-side: find the channel endpoint the parent's spawn handed us.
// Linux: returns { fd = kChildInheritFd } unconditionally (argv unused).
// Windows: scans argv for "--ipc-channel=0xHEX" and returns the parsed
//          inherited HANDLE; returns an invalid handle if missing.
NativeHandle locateInheritedChannel (int argc, const char* const* argv) noexcept;

// Blocking I/O. Retries on EINTR. Returns true only on a full transfer
// of n bytes; returns false on EOF, peer-close, error, or short write.
// `h` must be valid (isValid(h) == true).
bool readExact  (NativeHandle& h, void* buf, std::size_t n) noexcept;
bool writeExact (NativeHandle& h, const void* buf, std::size_t n) noexcept;

// Set the receive timeout on a channel endpoint. ms = 0 disables.
// Linux: SO_RCVTIMEO. Used by control-plane RPCs that need a deadline.
bool setReadTimeout (NativeHandle& h, int ms) noexcept;

// Send a single NativeHandle to the peer over a connected channel.
// Linux: SCM_RIGHTS ancillary data over a 1-byte sendmsg. macOS: same
// or mach_msg with a send-right; Windows: DuplicateHandle to the child
// process + write the value across the pipe.
//
// `payload` remains owned by the sender; the kernel duplicates it.
bool sendHandle (NativeHandle& channel, const NativeHandle& payload) noexcept;

// Receive a NativeHandle the peer sent. The returned handle is owned
// by the caller (close via closeHandle when done).
bool recvHandle (NativeHandle& channel, NativeHandle& payloadOut) noexcept;

} // namespace duskstudio::ipc::platform
