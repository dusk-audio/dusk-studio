#pragma once

#include <clap/clap.h>

#include <thread>

namespace duskstudio::clap
{
// Minimal CLAP host: owns a clap_host_t and provides the host extensions a
// no-GUI audio host needs (log, thread-check). One per plugin instance. The
// editor (gui), params, and state host-extensions get added in later increments.
// See docs/native-clap-host-plan.md.
class ClapHost
{
public:
    ClapHost();
    ClapHost (const ClapHost&)            = delete;
    ClapHost& operator= (const ClapHost&) = delete;

    const clap_host_t* get() const noexcept { return &host; }

    // Record which thread is the audio thread, so is_audio_thread() answers
    // correctly when the plugin queries it from process(). Set from the thread
    // that will drive process() before the first call.
    void setAudioThread (std::thread::id id) noexcept { audioThreadId = id; }

private:
    static const void* getExtension (const clap_host_t*, const char* id) noexcept;
    static bool isMainThread  (const clap_host_t*) noexcept;
    static bool isAudioThread (const clap_host_t*) noexcept;
    static void logMsg (const clap_host_t*, clap_log_severity, const char* msg) noexcept;

    clap_host_t              host {};
    clap_host_log_t          logExt {};
    clap_host_thread_check_t threadCheckExt {};

    const std::thread::id mainThreadId { std::this_thread::get_id() };
    std::thread::id       audioThreadId {};
};
} // namespace duskstudio::clap
