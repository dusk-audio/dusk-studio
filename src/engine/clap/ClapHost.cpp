#include "ClapHost.h"

#include <cstdio>
#include <cstring>

namespace duskstudio::clap
{
ClapHost::ClapHost()
{
    host.clap_version  = CLAP_VERSION;
    host.host_data     = this;
    host.name          = "Dusk Studio";
    host.vendor        = "Dusk Audio";
    host.url           = "https://dusk-audio.com";
    host.version       = "0.12.0";
    host.get_extension = &ClapHost::getExtension;
    // Restart / wake / main-thread-callback requests are no-ops until the host
    // gains a real engine integration (later increments wire these to the
    // suspend/reactivate + message-loop paths).
    host.request_restart  = [] (const clap_host_t*) {};
    host.request_process  = [] (const clap_host_t*) {};
    host.request_callback = [] (const clap_host_t*) {};

    logExt.log                     = &ClapHost::logMsg;
    threadCheckExt.is_main_thread  = &ClapHost::isMainThread;
    threadCheckExt.is_audio_thread = &ClapHost::isAudioThread;
}

const void* ClapHost::getExtension (const clap_host_t* h, const char* id) noexcept
{
    auto* self = static_cast<ClapHost*> (h->host_data);
    if (std::strcmp (id, CLAP_EXT_LOG)          == 0) return &self->logExt;
    if (std::strcmp (id, CLAP_EXT_THREAD_CHECK) == 0) return &self->threadCheckExt;
    return nullptr;   // unsupported — the plugin must handle null
}

bool ClapHost::isMainThread (const clap_host_t* h) noexcept
{
    return std::this_thread::get_id() == static_cast<ClapHost*> (h->host_data)->mainThreadId;
}

bool ClapHost::isAudioThread (const clap_host_t* h) noexcept
{
    return std::this_thread::get_id() == static_cast<ClapHost*> (h->host_data)->audioThreadId;
}

void ClapHost::logMsg (const clap_host_t*, clap_log_severity sev, const char* msg) noexcept
{
    // Warnings and above only; plugins are chatty at debug/info.
    if (sev >= CLAP_LOG_WARNING)
        std::fprintf (stderr, "[clap host] sev=%d %s\n", (int) sev, msg != nullptr ? msg : "");
}
} // namespace duskstudio::clap
