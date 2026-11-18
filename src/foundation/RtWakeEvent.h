#pragma once

#include <atomic>
#include <cstdint>

#if ! defined(_WIN32) && ! defined(__APPLE__)
 #include <semaphore.h>
#endif

namespace dusk
{
// An auto-reset event a real-time thread can signal: signal() takes no lock and
// allocates nothing, and wait() sleeps in the kernel. A signal made while no
// one waits is kept for the next wait(), and signals made before that wait()
// returns collapse into one, as with AutoResetEvent. One waiting thread only.
//
// A latch flag over a kernel semaphore. Only the signal that sets the flag
// posts, so the semaphore never counts past one. wait() clears the flag once
// woken and reads it with acquire, so whatever a signaller wrote before its
// signal the waiter sees, whichever signal woke it. The semaphore is POSIX on
// Linux, Mach on macOS (which has no unnamed POSIX semaphores) and Win32 on
// Windows; Clang's RealtimeSanitizer intercepts no sem_* call.
class RtWakeEvent
{
public:
    RtWakeEvent() noexcept;
    ~RtWakeEvent();

    void signal() noexcept;
    void wait() noexcept;

    RtWakeEvent (const RtWakeEvent&) = delete;
    RtWakeEvent& operator= (const RtWakeEvent&) = delete;

private:
    std::atomic<bool> latched { false };
   #if defined(_WIN32)
    void* semaphore = nullptr;          // a HANDLE
   #elif defined(__APPLE__)
    std::uint32_t semaphore = 0;        // a semaphore_t
   #else
    sem_t semaphore {};
   #endif
};
} // namespace dusk
