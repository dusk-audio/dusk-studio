#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "RenderProbeVst3.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <thread>

#if defined (__linux__)
 #include <sched.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#elif defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#elif defined (__APPLE__)
 #include <mach/mach.h>
 #include <mach/mach_time.h>
 #include <mach/thread_policy.h>
 #include <pthread.h>
#endif

namespace Steinberg::Vst
{
namespace
{
namespace Probe = duskstudio::test::renderprobevst3;

float flushesResults() noexcept
{
    volatile float smallest = std::numeric_limits<float>::min();
    const float half = smallest * 0.5f;
    std::uint32_t bits = 0;
    std::memcpy (&bits, &half, sizeof (bits));
    return bits == 0 ? 1.0f : 0.0f;
}

float flushesInputs() noexcept
{
    volatile float tiniest = std::numeric_limits<float>::denorm_min();
    return tiniest > 0.0f ? 0.0f : 1.0f;
}

// The calling thread's scheduling into the report (RenderProbeVst3.h).
void reportScheduling (float* report) noexcept
{
    report[Probe::kPolicy] = report[Probe::kPriority] = report[Probe::kThreadId] = -1.0f;
   #if defined (__linux__)
    sched_param param {};
    const int policy = sched_getscheduler (0);
    if (policy >= 0 && sched_getparam (0, &param) == 0)
    {
        report[Probe::kPolicy]   = (float) (policy & ~SCHED_RESET_ON_FORK);
        report[Probe::kPriority] = (float) param.sched_priority;
    }
    report[Probe::kThreadId] = (float) ::syscall (SYS_gettid);
   #elif defined (_WIN32)
    // GetThreadInformation's ThreadAbsoluteCpuPriority class, looked up at run
    // time so no Windows version guard decides whether it compiles.
    using GetInformation = BOOL (WINAPI*) (HANDLE, int, LPVOID, DWORD);
    static constexpr int kThreadAbsoluteCpuPriority = 1;
    const auto getInformation = reinterpret_cast<GetInformation> (reinterpret_cast<void (*)()> (
        GetProcAddress (GetModuleHandleW (L"kernel32.dll"), "GetThreadInformation")));
    report[Probe::kPolicy] = (float) GetThreadPriority (GetCurrentThread());
    LONG priority = 0;
    if (getInformation != nullptr
        && getInformation (GetCurrentThread(), kThreadAbsoluteCpuPriority, &priority, (DWORD) sizeof (priority)))
        report[Probe::kPriority] = (float) priority;
    report[Probe::kThreadId] = (float) GetCurrentThreadId();
   #elif defined (__APPLE__)
    const auto thread = pthread_mach_thread_np (pthread_self());
    thread_time_constraint_policy_data_t policy {};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t getDefault = FALSE;
    mach_timebase_info_data_t timebase {};
    if (thread_policy_get (thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t) &policy, &count, &getDefault)
            == KERN_SUCCESS
        && mach_timebase_info (&timebase) == KERN_SUCCESS && timebase.denom != 0)
    {
        report[Probe::kPolicy] = getDefault ? 0.0f : 1.0f;
        if (! getDefault)
            report[Probe::kPriority] = (float) ((double) policy.period * timebase.numer / timebase.denom / 1000.0);
    }
    report[Probe::kThreadId] = (float) thread;
   #endif
}

void holdWhileScanned()
{
   #if defined (__linux__)
    const char* folder = std::getenv (Probe::kHoldVariable);
    if (folder == nullptr || *folder == '\0') return;
    const std::filesystem::path root (folder);
    std::ofstream (root / "held.tmp") << (long long) ::getpid();
    std::error_code error;
    std::filesystem::rename (root / "held.tmp", root / "held", error);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (! std::filesystem::exists (root / "release", error)
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
   #endif
}

class RenderProbeEffect final : public SingleComponentEffect
{
public:
    static FUnknown* createInstance (void*)
    {
        return static_cast<IAudioProcessor*> (new RenderProbeEffect);
    }

    tresult PLUGIN_API initialize (FUnknown* context) override
    {
        const auto result = SingleComponentEffect::initialize (context);
        if (result != kResultOk) return result;
        addAudioInput (STR16 ("Input"), SpeakerArr::kStereo);
        addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
        holdWhileScanned();
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing (TBool) override { return kResultOk; }

    tresult PLUGIN_API process (ProcessData& data) override
    {
        if (data.numSamples == 0) return kResultOk;
        if (data.symbolicSampleSize != kSample32 || data.numInputs != 1 || data.inputs == nullptr
            || data.numOutputs != 1 || data.outputs == nullptr
            || data.inputs[0].numChannels != 2 || data.outputs[0].numChannels != 2)
            return kResultFalse;
        for (int32 channel = 0; channel < 2; ++channel)
            std::memcpy (data.outputs[0].channelBuffers32[channel], data.inputs[0].channelBuffers32[channel],
                         sizeof (float) * (std::size_t) data.numSamples);
        if (data.numSamples < Probe::kReportLength) return kResultOk;

        float* report = data.outputs[0].channelBuffers32[0];
        report[Probe::kFlushesResults] = flushesResults();
        report[Probe::kFlushesInputs]  = flushesInputs();
        reportScheduling (report);
        return kResultOk;
    }
};
} // namespace
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF ("Dusk Audio", "https://dusk.audio", "support@dusk.audio")
    DEF_CLASS2 (INLINE_UID (0x2C61D0A7, 0x4E8B4F12, 0x9D3A6B5C, 0x7F0E1A93),
                PClassInfo::kManyInstances, kVstAudioEffectClass,
                "Dusk Render Probe Fixture", 0, "Fx", "1.0.0", kVstVersionString,
                Steinberg::Vst::RenderProbeEffect::createInstance)
END_FACTORY
