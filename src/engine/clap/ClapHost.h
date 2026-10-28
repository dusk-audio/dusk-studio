#pragma once

#include <clap/clap.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>   // std::hash
#include <thread>
#include <vector>

namespace duskstudio::clap
{
// Minimal CLAP host: owns a clap_host_t and the host extensions an embedded-GUI
// audio host needs - log, thread-check, latency, gui, timer-support, state,
// params, and posix-fd-support everywhere but Windows (the extension must not
// exist there). One per plugin instance. The plugin's embedded editor asks us
// to pump its fds + timers; pumpGui() does that from the message thread.
// See docs/archive/native-clap-host-plan.md.
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

    // The plugin's thread-check ext is [thread-safe] - it may read the audio-thread
    // identity from any thread while the audio thread publishes it. Store a hash in a
    // lock-free atomic (the write happens on the audio thread, so it must not lock):
    // release on publish, acquire on read.
    void setAudioThread (std::thread::id id) noexcept
        { audioThreadHash.store (std::hash<std::thread::id>{} (id), std::memory_order_release); }
    // For the message thread temporarily designating itself the audio thread to
    // satisfy a plugin's thread-check inside an [audio-thread] call. The stamp
    // MUST be handed back: left in place, the message thread answers true to both
    // is_audio_thread and is_main_thread for as long as the instance lives.
    std::size_t exchangeAudioThread (std::thread::id id) noexcept
        { return audioThreadHash.exchange (std::hash<std::thread::id>{} (id),
                                           std::memory_order_acq_rel); }
    void restoreAudioThread (std::size_t previous) noexcept
        { audioThreadHash.store (previous, std::memory_order_release); }
    void setPlugin (const clap_plugin* p) noexcept    { plugin = p; }
    // The gui request callbacks (request_resize/show/hide) are CLAP [thread-safe] -
    // the plugin may fire them from its own thread while teardown swaps this target on
    // the message thread. Publish atomically; the trampolines load-acquire before deref.
    void setCallbacks (Callbacks* c) noexcept         { callbacks.store (c, std::memory_order_release); }

    // An editor that closed on the leak path left the plugin's GUI created. CLAP
    // allows one GUI per instance, so the next editor over this instance must not
    // call create() again. Latched for the instance's lifetime - message thread.
    void markGuiLeaked() noexcept    { guiLeaked = true; }
    bool isGuiLeaked() const noexcept { return guiLeaked; }

    // request_restart is [thread-safe], and a plug-in that changes its latency
    // while active calls it, often from process(). The trampoline only raises
    // this flag; the engine's message-thread drain consumes it and runs the
    // deactivate / activate cycle, after which the latency is read again.
    bool consumeRestartRequest() noexcept
        { return restartRequested.exchange (false, std::memory_order_acquire); }

    // The plug-in's persistent-state revision. The instance bumps it for host
    // writes and the values the plug-in announces; the plug-in bumps it through
    // clap_host_state.mark_dirty and value rescans. Kept here, not on the
    // instance, so a call the plug-in makes from inside destroy() still lands on
    // live storage. Any thread bumps (release); readers acquire.
    void markStateChanged() noexcept { revision.fetch_add (1, std::memory_order_release); }
    std::uint64_t stateRevision() const noexcept { return revision.load (std::memory_order_acquire); }

    // request_flush is [thread-safe] and only raises this flag. The next
    // process() answers it; for an instance no block is running, the
    // instance's flush outside process() does.
    bool isFlushRequested() const noexcept { return flushRequested.load (std::memory_order_acquire); }
    bool consumeFlushRequest() noexcept
    {
        return flushRequested.load (std::memory_order_relaxed)
            && flushRequested.exchange (false, std::memory_order_acquire);
    }

    // After rescan(CLAP_PARAM_RESCAN_ALL) every cookie the plug-in issued is
    // void, including those on writes already queued; events carry nullptr.
    bool paramCookiesRevoked() const noexcept { return cookiesRevoked.load (std::memory_order_acquire); }

    // Message thread: poll the plugin's registered fds (level-triggered, POSIX
    // platforms) and fire its registered timers, so the embedded GUI processes
    // its events + repaints.
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
#if ! defined(_WIN32)
    // posix-fd: never advertised on Windows, so nothing fd-shaped compiles there.
    static bool registerFd   (const clap_host_t*, int, clap_posix_fd_flags_t) noexcept;
    static bool modifyFd     (const clap_host_t*, int, clap_posix_fd_flags_t) noexcept;
    static bool unregisterFd (const clap_host_t*, int) noexcept;
#endif
    // timer
    static bool registerTimer   (const clap_host_t*, uint32_t, clap_id*) noexcept;
    static bool unregisterTimer (const clap_host_t*, clap_id) noexcept;

    // params
    static void rescanParams (const clap_host_t*, clap_param_rescan_flags) noexcept;

    static ClapHost& self (const clap_host_t* h) noexcept
        { return *static_cast<ClapHost*> (h->host_data); }

    clap_host_t                  host {};
    clap_host_log_t              logExt {};
    clap_host_thread_check_t     threadCheckExt {};
    clap_host_latency_t          latencyExt {};
    clap_host_gui_t              guiExt {};
#if ! defined(_WIN32)
    clap_host_posix_fd_support_t fdExt {};
#endif
    clap_host_timer_support_t    timerExt {};
    clap_host_state_t            stateExt {};
    clap_host_params_t           paramsExt {};

    const clap_plugin*       plugin = nullptr;
    std::atomic<Callbacks*>  callbacks { nullptr };
    bool                     guiLeaked = false;

#if ! defined(_WIN32)
    struct RegFd    { int fd; clap_posix_fd_flags_t flags; };
    std::vector<RegFd>    fds;
#endif
    struct RegTimer { clap_id id; uint32_t periodMs; double accumMs; };
    std::vector<RegTimer> timers;
    clap_id nextTimerId = 1;
    std::atomic<bool> callbackRequested { false };   // request_callback -> on_main_thread, drained in pumpGui
    std::atomic<bool> restartRequested  { false };   // request_restart -> engine drain reactivates
    std::atomic<bool> flushRequested    { false };
    std::atomic<bool> cookiesRevoked    { false };
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free,
                   "CLAP state revision must be lock-free");
    std::atomic<std::uint64_t> revision { 0 };

    const std::thread::id mainThreadId { std::this_thread::get_id() };
    std::atomic<std::size_t> audioThreadHash { 0 };
};
} // namespace duskstudio::clap
