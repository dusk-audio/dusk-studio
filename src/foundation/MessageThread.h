#pragma once

#include <functional>
#include <memory>

// The message-thread event seam: a periodic Timer and a post-to-message-thread
// callAsync, mirroring the slice of JUCE's Timer / MessageManager the non-GUI
// engine code consumes. Backed by the platform event loop (JUCE today; a
// bespoke Wayland loop later) so those engine units can drop their JUCE
// includes now and the backend swaps behind this pair without touching them.
// Construct, start and stop a Timer on the message thread only. callAsync may be
// called from any thread - that is its purpose: schedule work onto the message
// thread from off it.
namespace dusk
{
// Periodic message-thread callback. Override timerCallback(); start/stop it via
// the interval helpers. Not copyable (owns a backend timer). Stopping is
// idempotent and happens automatically at destruction.
class Timer
{
public:
    Timer();
    virtual ~Timer();

    void startTimer (int intervalMs) noexcept;
    void startTimerHz (int hz) noexcept;
    void stopTimer() noexcept;
    bool isTimerRunning() const noexcept;

    static void callAfterDelay (int milliseconds, std::function<void()> fn);

protected:
    virtual void timerCallback() = 0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    Timer (const Timer&) = delete;
    Timer& operator= (const Timer&) = delete;
};

// Post fn to run once on the message thread. Returns false if the message loop
// is gone (shutdown) and the call could not be queued.
bool callAsync (std::function<void()> fn);

// Traps the signals a desktop session or a supervisor uses to ask a process to
// exit (SIGTERM, plus SIGINT and SIGHUP where the platform defines them) and
// runs onQuitRequested on the message thread. The signal handler itself only
// latches a flag: callAsync allocates and takes the message-manager lock, and
// neither is safe from a signal handler, so a message-thread watcher polls the
// latch instead. Construct and destroy on the message thread. The dispositions
// are process-wide, so only one of these may exist at a time; the destructor
// restores the defaults. isInstalled() is false when the platform refused the
// handlers, in which case the default disposition stands and the callback
// never runs.
class QuitSignalHandler
{
public:
    explicit QuitSignalHandler (std::function<void()> onQuitRequested);
    ~QuitSignalHandler();

    bool isInstalled() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    QuitSignalHandler (const QuitSignalHandler&) = delete;
    QuitSignalHandler& operator= (const QuitSignalHandler&) = delete;
};
} // namespace dusk
