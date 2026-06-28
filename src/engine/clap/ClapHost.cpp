#include "ClapHost.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <poll.h>

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
    host.request_restart  = [] (const clap_host_t*) {};
    host.request_process  = [] (const clap_host_t*) {};
    host.request_callback = [] (const clap_host_t*) {};

    logExt.log                     = &ClapHost::logMsg;
    threadCheckExt.is_main_thread  = &ClapHost::isMainThread;
    threadCheckExt.is_audio_thread = &ClapHost::isAudioThread;

    guiExt.resize_hints_changed = &ClapHost::resizeHintsChanged;
    guiExt.request_resize       = &ClapHost::requestResize;
    guiExt.request_show         = &ClapHost::requestShow;
    guiExt.request_hide         = &ClapHost::requestHide;
    guiExt.closed               = &ClapHost::guiClosed;

    fdExt.register_fd   = &ClapHost::registerFd;
    fdExt.modify_fd     = &ClapHost::modifyFd;
    fdExt.unregister_fd = &ClapHost::unregisterFd;

    timerExt.register_timer   = &ClapHost::registerTimer;
    timerExt.unregister_timer = &ClapHost::unregisterTimer;
}

const void* ClapHost::getExtension (const clap_host_t* h, const char* id) noexcept
{
    auto& s = self (h);
    if (std::strcmp (id, CLAP_EXT_LOG)               == 0) return &s.logExt;
    if (std::strcmp (id, CLAP_EXT_THREAD_CHECK)      == 0) return &s.threadCheckExt;
    if (std::strcmp (id, CLAP_EXT_GUI)               == 0) return &s.guiExt;
    if (std::strcmp (id, CLAP_EXT_POSIX_FD_SUPPORT)  == 0) return &s.fdExt;
    if (std::strcmp (id, CLAP_EXT_TIMER_SUPPORT)     == 0) return &s.timerExt;
    return nullptr;
}

bool ClapHost::isMainThread (const clap_host_t* h) noexcept
{
    return std::this_thread::get_id() == self (h).mainThreadId;
}

bool ClapHost::isAudioThread (const clap_host_t* h) noexcept
{
    return std::this_thread::get_id() == self (h).audioThreadId;
}

void ClapHost::logMsg (const clap_host_t*, clap_log_severity sev, const char* msg) noexcept
{
    if (sev >= CLAP_LOG_WARNING)
        std::fprintf (stderr, "[clap host] sev=%d %s\n", (int) sev, msg != nullptr ? msg : "");
}

// ── gui ──────────────────────────────────────────────────────────────────────
void ClapHost::resizeHintsChanged (const clap_host_t* h) noexcept
{
    if (auto* c = self (h).callbacks) c->onResizeHintsChanged();
}
bool ClapHost::requestResize (const clap_host_t* h, uint32_t w, uint32_t hgt) noexcept
{
    auto* c = self (h).callbacks; return c != nullptr && c->onRequestResize (w, hgt);
}
bool ClapHost::requestShow (const clap_host_t* h) noexcept
{
    auto* c = self (h).callbacks; return c != nullptr && c->onRequestShow();
}
bool ClapHost::requestHide (const clap_host_t* h) noexcept
{
    auto* c = self (h).callbacks; return c != nullptr && c->onRequestHide();
}
void ClapHost::guiClosed (const clap_host_t* h, bool wasDestroyed) noexcept
{
    if (auto* c = self (h).callbacks) c->onGuiClosed (wasDestroyed);
}

// ── posix-fd ─────────────────────────────────────────────────────────────────
bool ClapHost::registerFd (const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) noexcept
{
    auto& s = self (h);
    for (auto& r : s.fds) if (r.fd == fd) { r.flags = flags; return true; }
    s.fds.push_back ({ fd, flags });
    return true;
}
bool ClapHost::modifyFd (const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) noexcept
{
    for (auto& r : self (h).fds) if (r.fd == fd) { r.flags = flags; return true; }
    return false;
}
bool ClapHost::unregisterFd (const clap_host_t* h, int fd) noexcept
{
    auto& v = self (h).fds;
    const auto n = v.size();
    v.erase (std::remove_if (v.begin(), v.end(), [fd] (const RegFd& r) { return r.fd == fd; }), v.end());
    return v.size() != n;
}

// ── timer ────────────────────────────────────────────────────────────────────
bool ClapHost::registerTimer (const clap_host_t* h, uint32_t periodMs, clap_id* idOut) noexcept
{
    if (idOut == nullptr) return false;
    auto& s = self (h);
    const auto id = s.nextTimerId++;
    s.timers.push_back ({ id, std::max<uint32_t> (1, periodMs), 0.0 });
    *idOut = id;
    return true;
}
bool ClapHost::unregisterTimer (const clap_host_t* h, clap_id id) noexcept
{
    auto& v = self (h).timers;
    const auto n = v.size();
    v.erase (std::remove_if (v.begin(), v.end(), [id] (const RegTimer& t) { return t.id == id; }), v.end());
    return v.size() != n;
}

// ── pump ─────────────────────────────────────────────────────────────────────
void ClapHost::pumpGui (double elapsedMs)
{
    if (plugin == nullptr) return;

    if (! fds.empty())
    {
        if (const auto* fdSup = static_cast<const clap_plugin_posix_fd_support_t*> (
                plugin->get_extension (plugin, CLAP_EXT_POSIX_FD_SUPPORT)); fdSup != nullptr && fdSup->on_fd != nullptr)
        {
            std::vector<pollfd> pfds;
            pfds.reserve (fds.size());
            for (const auto& r : fds)
            {
                short ev = 0;
                if (r.flags & CLAP_POSIX_FD_READ)  ev |= POLLIN;
                if (r.flags & CLAP_POSIX_FD_WRITE) ev |= POLLOUT;
                pfds.push_back ({ r.fd, ev, 0 });
            }
            if (::poll (pfds.data(), (nfds_t) pfds.size(), 0) > 0)
                for (const auto& p : pfds)   // iterates the local snapshot — on_fd may (un)register
                {
                    clap_posix_fd_flags_t f = 0;
                    if (p.revents & POLLIN)               f |= CLAP_POSIX_FD_READ;
                    if (p.revents & POLLOUT)              f |= CLAP_POSIX_FD_WRITE;
                    if (p.revents & (POLLERR | POLLHUP))  f |= CLAP_POSIX_FD_ERROR;
                    if (f != 0) fdSup->on_fd (plugin, p.fd, f);
                }
        }
    }

    if (! timers.empty())
    {
        if (const auto* tSup = static_cast<const clap_plugin_timer_support_t*> (
                plugin->get_extension (plugin, CLAP_EXT_TIMER_SUPPORT)); tSup != nullptr && tSup->on_timer != nullptr)
        {
            // Advance accumulators first (no callbacks → `timers` stays stable),
            // collect what's due, then fire — on_timer may (un)register timers.
            std::vector<clap_id> due;
            for (auto& t : timers)
            {
                t.accumMs += elapsedMs;
                if (t.accumMs >= (double) t.periodMs) { t.accumMs = 0.0; due.push_back (t.id); }
            }
            for (const auto id : due)
                tSup->on_timer (plugin, id);
        }
    }
}
} // namespace duskstudio::clap
