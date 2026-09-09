#include "MessageThread.h"

#include <juce_events/juce_events.h>

#include <csignal>
#include <utility>

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

void Timer::callAfterDelay (int milliseconds, std::function<void()> fn)
{
    juce::Timer::callAfterDelay (milliseconds, std::move (fn));
}

bool callAsync (std::function<void()> fn)
{
    return juce::MessageManager::callAsync (std::move (fn));
}

namespace
{
volatile std::sig_atomic_t quitRequestPending = 0;

extern "C" {
static void latchQuitRequest (int) { quitRequestPending = 1; }
}

constexpr int quitSignals[] =
{
    SIGINT,
    SIGTERM,
   #if defined (SIGHUP)
    SIGHUP,
   #endif
};

bool setQuitDisposition (void (*handler) (int)) noexcept
{
    bool ok = true;

   #if defined (_WIN32)
    for (const int sig : quitSignals)
        ok = (std::signal (sig, handler) != SIG_ERR) && ok;
   #else
    // SA_RESTART so a trapped signal does not turn every blocking read in the
    // process into a spurious EINTR; the handler does no work of its own.
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset (&action.sa_mask);
    action.sa_flags = SA_RESTART;

    for (const int sig : quitSignals)
        ok = (::sigaction (sig, &action, nullptr) == 0) && ok;
   #endif

    return ok;
}
} // namespace

struct QuitSignalHandler::Impl final : private Timer
{
    explicit Impl (std::function<void()> fn) : onQuitRequested (std::move (fn))
    {
        quitRequestPending = 0;
        installed = setQuitDisposition (latchQuitRequest);
        if (installed)
            startTimer (50);
    }

    ~Impl() override
    {
        if (installed)
            setQuitDisposition (SIG_DFL);
        quitRequestPending = 0;
    }

    void timerCallback() override
    {
        if (quitRequestPending == 0) return;
        quitRequestPending = 0;
        if (onQuitRequested) onQuitRequested();
    }

    std::function<void()> onQuitRequested;
    bool installed = false;
};

QuitSignalHandler::QuitSignalHandler (std::function<void()> onQuitRequested)
    : impl (std::make_unique<Impl> (std::move (onQuitRequested))) {}
QuitSignalHandler::~QuitSignalHandler() = default;

bool QuitSignalHandler::isInstalled() const noexcept { return impl->installed; }
} // namespace dusk
