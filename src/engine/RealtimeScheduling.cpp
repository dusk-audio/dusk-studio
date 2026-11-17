#include "RealtimeScheduling.h"

#if defined(__APPLE__)
 #include <mach/mach.h>
 #include <mach/mach_time.h>
 #include <mach/thread_policy.h>

 #include <algorithm>
 #include <cstdint>
#elif defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace duskstudio::rt
{
#if defined(_WIN32)
namespace
{
// GetProcAddress hands back a generic pointer; going through void (*)() is the
// cast compilers accept to any other function type without a warning.
template <typename Function>
Function avrtFunction (HMODULE avrt, const char* name) noexcept
{
    return reinterpret_cast<Function> (reinterpret_cast<void (*)()> (GetProcAddress (avrt, name)));
}
} // namespace

// avrt.dll is loaded at run time, as JUCE does, so nothing new is linked.
ProAudioTask joinProAudioTask() noexcept
{
    using Join = HANDLE (WINAPI*) (LPCWSTR, LPDWORD);
    ProAudioTask joined;
    const HMODULE avrt = LoadLibraryW (L"avrt.dll");
    joined.avrt = avrt;
    if (avrt != nullptr)
        if (const auto join = avrtFunction<Join> (avrt, "AvSetMmThreadCharacteristicsW"))
        {
            DWORD taskIndex = 0;
            joined.task = join (L"Pro Audio", &taskIndex);
        }
    if (joined.task == nullptr)
        joined.timeCritical = SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
    return joined;
}

void leaveProAudioTask (const ProAudioTask& joined) noexcept
{
    using Leave = BOOL (WINAPI*) (HANDLE);
    const auto avrt = static_cast<HMODULE> (joined.avrt);
    if (joined.task != nullptr)
        if (const auto leave = avrtFunction<Leave> (avrt, "AvRevertMmThreadCharacteristics"))
            leave (static_cast<HANDLE> (joined.task));
    if (avrt != nullptr)
        FreeLibrary (avrt);
}
#elif defined(__APPLE__)
// The kernel refuses a computation much past 50 ms, which only a very long
// block reaches.
bool takeTimeConstraint (pthread_t thread, double periodSeconds) noexcept
{
    mach_timebase_info_data_t timebase {};
    if (periodSeconds <= 0.0 || mach_timebase_info (&timebase) != KERN_SUCCESS || timebase.numer == 0)
        return false;
    const double ticksPerSecond = 1.0e9 * (double) timebase.denom / (double) timebase.numer;
    const auto ticks = [ticksPerSecond] (double seconds)
    {
        return (uint32_t) std::min (seconds * ticksPerSecond, 4.0e9);
    };
    thread_time_constraint_policy_data_t policy {};
    policy.period      = ticks (periodSeconds);
    policy.computation = ticks (std::min (periodSeconds, 0.05));
    policy.constraint  = ticks (periodSeconds);
    policy.preemptible = 1;
    return thread_policy_set (pthread_mach_thread_np (thread), THREAD_TIME_CONSTRAINT_POLICY,
                              (thread_policy_t) &policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT)
        == KERN_SUCCESS;
}
#endif
} // namespace duskstudio::rt
