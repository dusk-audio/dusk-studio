#include "MessageThread.h"
#include "TimerSchedule.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace dusk
{
namespace
{
using Schedule = detail::TimerSchedule;

// Every dusk::Timer runs from one backend timer armed for the next shared deadline,
// so the framework's timer thread and the message thread wake once per deadline
// however many timers there are. The lock keeps start and stop as safe off the
// message thread as the framework's own timers are; callbacks run outside it.
class SharedTick
{
public:
    using Fire = void (*) (void* owner);

    static SharedTick& get()
    {
        // Never destroyed, so a timer that outlives static destruction still finds it.
        static auto* const instance = new SharedTick();
        return *instance;
    }

    void attach (Fire fireOwner)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        fire = fireOwner;
        if (users++ == 0)
            backend = std::make_unique<Backend>();
    }

    void detach (Schedule::Slot& slot)
    {
        std::unique_ptr<Backend> retired;
        const std::lock_guard<std::mutex> lock (mutex);
        stopLocked (slot);
        if (--users == 0)
            retired = std::move (backend);
    }

    void start (Schedule::Slot& slot, Schedule::Duration period)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        const auto now = Schedule::Clock::now();
        forget (slot);
        schedule.start (slot, period, now);
        armLocked (now);
    }

    void stop (Schedule::Slot& slot)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopLocked (slot);
    }

    bool isRunning (const Schedule::Slot& slot)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return Schedule::isScheduled (slot);
    }

private:
    struct Backend final : private juce::Timer
    {
        ~Backend() override { stopTimer(); }
        void arm (int ms) { startTimer (ms); }
        void disarm() { stopTimer(); }
        void timerCallback() override { SharedTick::get().tick(); }
    };

    // The timers one pass calls. A callback can stop, restart or delete any timer,
    // and one it does is skipped for the rest of the pass, as the framework skips a
    // timer restarted before its turn. A callback that runs a nested event loop
    // nests a pass inside this one, so passes chain.
    struct Pass
    {
        std::vector<Schedule::Slot*> slots;
        Pass* outer = nullptr;
    };

    SharedTick() = default;

    void tick()
    {
        Pass pass;
        {
            const std::lock_guard<std::mutex> lock (mutex);
            const auto now = Schedule::Clock::now();
            armedFor.reset();
            schedule.takeDue (now, pass.slots);
            pass.outer = innermost;
            innermost = &pass;
            armLocked (now);
        }
        struct Unlink
        {
            SharedTick& tick;
            Pass& pass;
            ~Unlink()
            {
                const std::lock_guard<std::mutex> lock (tick.mutex);
                tick.innermost = pass.outer;
            }
        } unlink { *this, pass };

        for (std::size_t i = 0; i < pass.slots.size(); ++i)
        {
            void* owner = nullptr;
            Fire fireOwner = nullptr;
            {
                const std::lock_guard<std::mutex> lock (mutex);
                if (auto* slot = pass.slots[i])
                {
                    schedule.rebaseIfOverdue (*slot, Schedule::Clock::now());
                    owner = slot->owner;
                }
                fireOwner = fire;
            }
            if (owner != nullptr)
                fireOwner (owner);
        }
    }

    void forget (const Schedule::Slot& slot) noexcept
    {
        for (auto* p = innermost; p != nullptr; p = p->outer)
            for (auto*& entry : p->slots)
                if (entry == &slot)
                    entry = nullptr;
    }

    void stopLocked (Schedule::Slot& slot)
    {
        forget (slot);
        schedule.stop (slot);
        if (schedule.size() == 0)
            armLocked (Schedule::Clock::now());
    }

    // A deadline later than the one the backend already waits for is picked up
    // when that one fires.
    void armLocked (Schedule::TimePoint now)
    {
        if (backend == nullptr)
            return;
        const auto next = schedule.nextDue();
        if (! next)
        {
            backend->disarm();
            armedFor.reset();
            return;
        }
        if (armedFor && *armedFor <= *next)
            return;
        const auto wait = std::chrono::ceil<std::chrono::milliseconds> (*next - now).count();
        const int ms = static_cast<int> (std::clamp<long long> (wait, 1, 1 << 30));
        backend->arm (ms);
        armedFor = now + std::chrono::milliseconds (ms);
    }

    std::mutex mutex;
    Schedule schedule;
    std::unique_ptr<Backend> backend;
    std::optional<Schedule::TimePoint> armedFor;
    Pass* innermost = nullptr;
    Fire fire = nullptr;
    int users = 0;
};
} // namespace

// Nested so it can reach Timer's protected timerCallback() for the shared tick.
struct Timer::Impl
{
    explicit Impl (dusk::Timer& owner)
    {
        slot.owner = &owner;
        SharedTick::get().attach (&Impl::fire);
    }

    ~Impl() { SharedTick::get().detach (slot); }

    static void fire (void* owner) { static_cast<dusk::Timer*> (owner)->timerCallback(); }

    Schedule::Slot slot;
};

Timer::Timer() : impl (std::make_unique<Impl> (*this)) {}
Timer::~Timer() = default;

void Timer::startTimer (int intervalMs) noexcept
{
    SharedTick::get().start (impl->slot, std::chrono::milliseconds (std::max (1, intervalMs)));
}

void Timer::startTimerHz (int hz) noexcept
{
    if (hz <= 0)
    {
        stopTimer();
        return;
    }
    const Schedule::Duration period = std::chrono::seconds (1);
    SharedTick::get().start (impl->slot, std::max<Schedule::Duration> (std::chrono::milliseconds (1), period / hz));
}

void Timer::stopTimer() noexcept { SharedTick::get().stop (impl->slot); }
bool Timer::isTimerRunning() const noexcept { return SharedTick::get().isRunning (impl->slot); }

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
