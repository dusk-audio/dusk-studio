#pragma once

#if defined(__APPLE__)
 #include <pthread.h>
#endif

// Realtime scheduling on macOS and Windows for the threads an audio block waits
// on: the engine's DSP workers (AudioWorkerPool) and the sandbox child's audio
// thread (PluginHostMain). Linux goes through RtPriority.h and RealtimeKit.h.
namespace duskstudio::rt
{
#if defined(_WIN32)
// What joinProAudioTask got the calling thread.
struct ProAudioTask
{
    void* avrt = nullptr;         // avrt.dll, held while the task is
    void* task = nullptr;         // the MMCSS task, or null when MMCSS refused
    bool  timeCritical = false;   // refused, the thread took THREAD_PRIORITY_TIME_CRITICAL
};

// The calling thread into the MMCSS "Pro Audio" task, the one the device's
// callback thread runs in. When MMCSS refuses, the thread still goes above
// every normal-priority thread.
ProAudioTask joinProAudioTask() noexcept;
void leaveProAudioTask (const ProAudioTask& joined) noexcept;
#elif defined(__APPLE__)
// The time-constraint policy CoreAudio gives its IO thread, for `thread`: woken
// once a period and allowed to compute through it. False when the kernel
// refuses, which leaves the thread as it was.
bool takeTimeConstraint (pthread_t thread, double periodSeconds) noexcept;
#endif
} // namespace duskstudio::rt
