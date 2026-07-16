#include "MessageThread.h"

#include <juce_events/juce_events.h>

namespace dusk
{
// The backend timer. Nested so it can reach Timer's protected timerCallback()
// (enclosing-class access) and forward the tick to the derived override.
struct Timer::Impl final : private juce::Timer
{
    explicit Impl (dusk::Timer& o) noexcept : owner (o) {}

    void timerCallback() override { owner.timerCallback(); }

    void start (int intervalMs)  noexcept { startTimer (intervalMs); }
    void startHz (int hz)        noexcept { startTimerHz (hz); }
    void stop()                  noexcept { stopTimer(); }
    bool running() const         noexcept { return isTimerRunning(); }

    dusk::Timer& owner;
};

Timer::Timer() : impl (std::make_unique<Impl> (*this)) {}
Timer::~Timer() { impl->stop(); }

void Timer::startTimer   (int intervalMs) noexcept { impl->start (intervalMs); }
void Timer::startTimerHz (int hz)         noexcept { impl->startHz (hz); }
void Timer::stopTimer()                   noexcept { impl->stop(); }
bool Timer::isTimerRunning() const        noexcept { return impl->running(); }

bool callAsync (std::function<void()> fn)
{
    return juce::MessageManager::callAsync (std::move (fn));
}
} // namespace dusk
