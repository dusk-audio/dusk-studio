#pragma once

#include "IpcChannel.h"

#include <atomic>
#include <cstdint>
#include <string>

// Cross-process wait/wake paired with a 32-bit atomic stored in shared memory.
// Used by RemotePluginConnection::processBlockSync (audio thread, parent
// side) and the dusk-studio-plugin-host audio worker (child side) to
// signal each other when a block is ready / a reply has arrived without
// either side allocating or holding a mutex.
//
// Linux  : futex(SYS_futex, FUTEX_WAIT_BITSET / FUTEX_WAKE) - non-private
//          so the address is hashed by physical page and works across
//          two processes mmap'ing the same memfd.
// macOS  : os_sync_wait_on_address_with_timeout / os_sync_wake_by_address
//          (macOS 14.4+) - supports OS_SYNC_WAIT_ON_ADDRESS_SHARED for
//          cross-process. Fallback: POSIX semaphores in shared memory.
// Windows: an inheritable auto-reset event per signal direction. Win32's
//          address-wait APIs are process-local even for shared mappings.
//
// MUST be RT-safe on the audio path: no allocation, no exceptions, the
// wake-side never blocks. Wait-side blocks for at most the deadline.

namespace duskstudio::ipc::platform
{

enum class WaitResult
{
    Awoken,        // wake call delivered (possibly spurious) - caller re-checks atom
    ValueChanged,  // atom already differed from `expected` when entering kernel
    Timeout,       // deadline elapsed without a wake
    Interrupted,   // signal-interrupted; caller may retry the wait
    Error          // unrecoverable
};

// Absolute monotonic-clock deadline. Each platform converts to its own
// timeout shape (Linux: struct timespec for FUTEX_WAIT_BITSET, Windows:
// ms relative for WaitForSingleObject, macOS: ns relative for os_sync_*).
struct Deadline
{
    std::int64_t monotonicSec  { 0 };
    std::int32_t monotonicNsec { 0 };
};

Deadline deadlineFromNow (long long nsFromNow) noexcept;

// Block while `addr->load() == expected`, up to `*deadline` if non-null.
// Atomic memory order on `addr` is the caller's responsibility - pass an
// atomic loaded with `acquire`. Spurious wakes are possible (FUTEX_WAIT
// semantics); always re-check the atom after a return.
class InterprocessSignal
{
public:
    InterprocessSignal() = default;
    ~InterprocessSignal();

    InterprocessSignal (const InterprocessSignal&) = delete;
    InterprocessSignal& operator= (const InterprocessSignal&) = delete;

    // Parent-side setup and control-channel handoff. Linux and macOS need no
    // separate kernel object, so these are successful no-ops there. Windows
    // creates an inheritable event before spawn and sends its inherited handle
    // value to the child after the shared-memory handle.
    bool create (std::string& errorOut) noexcept;
    bool sendToChild (NativeHandle& channel) const noexcept;
    bool receiveFromParent (NativeHandle& channel) noexcept;
    void close() noexcept;

    // Block while `addr` still equals `expected`, up to `deadline`. The
    // sequence atom makes missed/coalesced event wakes harmless: every caller
    // re-checks it after this returns.
    WaitResult wait (std::atomic<std::uint32_t>* addr,
                       std::uint32_t expected,
                       const Deadline* deadline) noexcept;

    // Publish one non-blocking wake. `addr` identifies the futex/os_sync word
    // on Linux/macOS; Windows uses this object's auto-reset event.
    void wake (std::atomic<std::uint32_t>* addr) noexcept;

private:
    NativeHandle nativeHandle {};
};

// Polite spin pause for the bounded-spin loop in processBlockSync.
// Reduces SMT contention with the producer thread. Per-arch:
//   x86_64 / i386 : pause
//   aarch64 / arm : yield
//   other         : std::this_thread::yield()
void cpuRelax() noexcept;

} // namespace duskstudio::ipc::platform
