#include "RtWakeEvent.h"

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif defined(__APPLE__)
 #include <mach/mach.h>
#else
 #include <cerrno>
#endif

namespace dusk
{
#if defined(__APPLE__)
static_assert (sizeof (semaphore_t) == sizeof (std::uint32_t), "a Mach semaphore name fits the stored word");
#endif

RtWakeEvent::RtWakeEvent() noexcept
{
   #if defined(_WIN32)
    semaphore = CreateSemaphoreW (nullptr, 0, 1, nullptr);
   #elif defined(__APPLE__)
    semaphore_t created = SEMAPHORE_NULL;
    semaphore_create (mach_task_self(), &created, SYNC_POLICY_FIFO, 0);
    semaphore = (std::uint32_t) created;
   #else
    sem_init (&semaphore, 0, 0);
   #endif
}

RtWakeEvent::~RtWakeEvent()
{
   #if defined(_WIN32)
    if (semaphore != nullptr)
        CloseHandle (semaphore);
   #elif defined(__APPLE__)
    if (semaphore != 0)
        semaphore_destroy (mach_task_self(), (semaphore_t) semaphore);
   #else
    sem_destroy (&semaphore);
   #endif
}

void RtWakeEvent::signal() noexcept
{
    if (latched.exchange (true, std::memory_order_acq_rel))
        return;
   #if defined(_WIN32)
    ReleaseSemaphore (semaphore, 1, nullptr);
   #elif defined(__APPLE__)
    semaphore_signal ((semaphore_t) semaphore);
   #else
    sem_post (&semaphore);
   #endif
}

void RtWakeEvent::wait() noexcept
{
   #if defined(_WIN32)
    WaitForSingleObject (semaphore, INFINITE);
   #elif defined(__APPLE__)
    while (semaphore_wait ((semaphore_t) semaphore) == KERN_ABORTED) {}
   #else
    while (sem_wait (&semaphore) != 0 && errno == EINTR) {}
   #endif
    latched.exchange (false, std::memory_order_acquire);
}
} // namespace dusk
