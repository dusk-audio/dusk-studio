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
} // namespace dusk
