#pragma once

#include <condition_variable>
#include <chrono>
#include <mutex>

// Auto-reset event, matching JUCE's WaitableEvent default (auto-reset)
// semantics: signal() latches; the next wait() consumes the latch and clears it
// so exactly one waiter is released per signal. A signal delivered while no one
// is waiting is remembered, so a wakeup is never lost between a worker's ack and
// its next wait (the invariant AudioWorkerPool relies on).
namespace dusk
{
class AutoResetEvent
{
public:
    void signal() noexcept
    {
        // Notify while holding the lock: notifying after releasing it leaves the
        // store-to-signalled and the waiter's cv.wait relock unsynchronised, which
        // ThreadSanitizer flags as a data race on the shared state.
        std::lock_guard<std::mutex> lk (m);
        signalled = true;
        cv.notify_one();
    }

    void wait() noexcept
    {
        std::unique_lock<std::mutex> lk (m);
        cv.wait (lk, [this] { return signalled; });
        signalled = false;
    }

    // Returns true if the event was signalled within the timeout (and consumes
    // it), false on timeout.
    bool wait (int timeoutMs) noexcept
    {
        std::unique_lock<std::mutex> lk (m);
        if (! cv.wait_for (lk, std::chrono::milliseconds (timeoutMs),
                           [this] { return signalled; }))
            return false;
        signalled = false;
        return true;
    }

private:
    std::mutex              m;
    std::condition_variable cv;
    bool                    signalled = false;
};
} // namespace dusk
