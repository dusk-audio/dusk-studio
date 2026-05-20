#pragma once

#include <atomic>
#include <cstdint>

// Cross-process wait/wake on a 32-bit atomic stored in shared memory.
// Used by RemotePluginConnection::processBlockSync (audio thread, parent
// side) and the dusk-studio-plugin-host audio worker (child side) to
// signal each other when a block is ready / a reply has arrived without
// either side allocating or holding a mutex.
//
// Linux  : futex(SYS_futex, FUTEX_WAIT_BITSET / FUTEX_WAKE) — non-private
//          so the address is hashed by physical page and works across
//          two processes mmap'ing the same memfd.
// macOS  : os_sync_wait_on_address_with_timeout / os_sync_wake_by_address
//          (macOS 14.4+) — supports OS_SYNC_WAIT_ON_ADDRESS_SHARED for
//          cross-process. Fallback: POSIX semaphores in shared memory.
// Windows: WaitOnAddress / WakeByAddressSingle — supports cross-process
//          when the address is in a shared mapping.
//
// MUST be RT-safe on the audio path: no allocation, no exceptions, the
// wake-side never blocks. Wait-side blocks for at most the deadline.

namespace duskstudio::ipc::platform
{

enum class WaitResult
{
    Awoken,        // wake call delivered (possibly spurious) — caller re-checks atom
    ValueChanged,  // atom already differed from `expected` when entering kernel
    Timeout,       // deadline elapsed without a wake
    Interrupted,   // signal-interrupted; caller may retry the wait
    Error          // unrecoverable
};

// Absolute monotonic-clock deadline. Each platform converts to its own
// timeout shape (Linux: struct timespec for FUTEX_WAIT_BITSET, Windows:
// ms relative for WaitOnAddress, macOS: ns relative for os_sync_*).
struct Deadline
{
    std::int64_t monotonicSec  { 0 };
    std::int32_t monotonicNsec { 0 };
};

Deadline deadlineFromNow (long long nsFromNow) noexcept;

// Block while `addr->load() == expected`, up to `*deadline` if non-null.
// Atomic memory order on `addr` is the caller's responsibility — pass an
// atomic loaded with `acquire`. Spurious wakes are possible (FUTEX_WAIT
// semantics); always re-check the atom after a return.
WaitResult waitOnAddress (std::atomic<std::uint32_t>* addr,
                            std::uint32_t expected,
                            const Deadline* deadline) noexcept;

// Wake a single waiter sleeping on `addr`. Wakes are best-effort —
// missed wakes are recovered by the wait-side polling on next entry.
void wakeOneAddress (std::atomic<std::uint32_t>* addr) noexcept;

// Polite spin pause for the bounded-spin loop in processBlockSync.
// Reduces SMT contention with the producer thread. Per-arch:
//   x86_64 / i386 : pause
//   aarch64 / arm : yield
//   other         : std::this_thread::yield()
void cpuRelax() noexcept;

} // namespace duskstudio::ipc::platform
