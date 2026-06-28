#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <thread>
#include <vector>

namespace duskstudio::clap
{
// Minimal CLAP host: owns a clap_host_t and the host extensions an embedded-GUI
// audio host needs — log, thread-check, gui, posix-fd-support, timer-support.
// One per plugin instance. The plugin's editor runs its own X11 event loop and
// asks us to pump its fd + timers; pumpGui() does that from the message thread.
// See docs/native-clap-host-plan.md.
class ClapHost
{
public:
    // GUI events the plugin sends back to the host (forwarded to the editor).
    struct Callbacks
    {
        virtual ~Callbacks() = default;
        virtual bool onRequestResize (uint32_t /*w*/, uint32_t /*h*/) { return false; }
        virtual bool onRequestShow()                                   { return false; }
        virtual bool onRequestHide()                                   { return false; }
        virtual void onGuiClosed (bool /*wasDestroyed*/)               {}
        virtual void onResizeHintsChanged()                            {}
    };

    ClapHost();
    ClapHost (const ClapHost&)            = delete;
    ClapHost& operator= (const ClapHost&) = delete;

    const clap_host_t* get() const noexcept { return &host; }

    void setAudioThread (std::thread::id id) noexcept { audioThreadId = id; }
    void setPlugin (const clap_plugin* p) noexcept    { plugin = p; }
    void setCallbacks (Callbacks* c) noexcept         { callbacks = c; }

    // Message thread: poll the plugin's registered fds (level-triggered) and fire
    // its registered timers, so the embedded GUI processes its X11 events + repaints.
    void pumpGui (double elapsedMs);

private:
    static const void* getExtension (const clap_host_t*, const char* id) noexcept;
    static bool isMainThread  (const clap_host_t*) noexcept;
    static bool isAudioThread (const clap_host_t*) noexcept;
    static void logMsg (const clap_host_t*, clap_log_severity, const char* msg) noexcept;

    // gui
    static void resizeHintsChanged (const clap_host_t*) noexcept;
    static bool requestResize (const clap_host_t*, uint32_t, uint32_t) noexcept;
    static bool requestShow (const clap_host_t*) noexcept;
    static bool requestHide (const clap_host_t*) noexcept;
    static void guiClosed (const clap_host_t*, bool) noexcept;
    // posix-fd
    static bool registerFd   (const clap_host_t*, int, clap_posix_fd_flags_t) noexcept;
    static bool modifyFd     (const clap_host_t*, int, clap_posix_fd_flags_t) noexcept;
    static bool unregisterFd (const clap_host_t*, int) noexcept;
    // timer
    static bool registerTimer   (const clap_host_t*, uint32_t, clap_id*) noexcept;
    static bool unregisterTimer (const clap_host_t*, clap_id) noexcept;

    static ClapHost& self (const clap_host_t* h) noexcept
        { return *static_cast<ClapHost*> (h->host_data); }

    clap_host_t                  host {};
    clap_host_log_t              logExt {};
    clap_host_thread_check_t     threadCheckExt {};
    clap_host_gui_t              guiExt {};
    clap_host_posix_fd_support_t fdExt {};
    clap_host_timer_support_t    timerExt {};

    const clap_plugin* plugin    = nullptr;
    Callbacks*         callbacks = nullptr;

    struct RegFd    { int fd; clap_posix_fd_flags_t flags; };
    struct RegTimer { clap_id id; uint32_t periodMs; double accumMs; };
    std::vector<RegFd>    fds;
    std::vector<RegTimer> timers;
    clap_id nextTimerId = 1;

    const std::thread::id mainThreadId { std::this_thread::get_id() };
    std::thread::id       audioThreadId {};
};
} // namespace duskstudio::clap
