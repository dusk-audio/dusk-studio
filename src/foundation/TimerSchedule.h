#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dusk::detail
{
// The thread that runs periodic timers wakes once for every distinct deadline, so
// timers left on their own phases add up: thirty 30 Hz timers started at different
// moments can cost 900 wake-ups a second where one shared tick costs 30. The schedule
// puts every deadline of a timer of kFineMin or longer on the instants of one 60 Hz
// clock, and of kCoarseMin or longer on every second instant (30 Hz), so timers fire
// together however many there are. A timer keeps its average rate, because each
// deadline is the first instant at or after the timer's own nominal schedule, and
// it never fires before its period has run since it started. Shorter timers keep
// their exact deadlines. Not thread-safe: the owner locks around it.
class TimerSchedule
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    static constexpr std::size_t kUnscheduled = static_cast<std::size_t> (-1);
    // A deadline this close counts as reached: the backend timer that wakes the
    // owner counts whole milliseconds.
    static constexpr Duration kTolerance = std::chrono::milliseconds (1);
    static constexpr Duration kFineMin = std::chrono::milliseconds (15);
    static constexpr Duration kCoarseMin = std::chrono::milliseconds (30);

    struct Slot
    {
        void* owner = nullptr;
        Duration period {};
        TimePoint nominal {}, due {};
        std::size_t index = kUnscheduled;
    };

    static bool isScheduled (const Slot& slot) noexcept { return slot.index != kUnscheduled; }

    void start (Slot& slot, Duration period, TimePoint now)
    {
        slot.period = std::max (period, Duration (1));
        slot.nominal = now + slot.period;
        slot.due = place (slot, now);
        if (! isScheduled (slot))
        {
            slot.index = slots.size();
            slots.push_back (&slot);
        }
    }

    void stop (Slot& slot) noexcept
    {
        if (! isScheduled (slot))
            return;
        Slot* const last = slots.back();
        slots[slot.index] = last;
        last->index = slot.index;
        slots.pop_back();
        slot.index = kUnscheduled;
    }

    // Appends every slot due at `now` to `out`, each moved on to its next deadline.
    // A timer more than a period behind skips what it missed rather than firing
    // in a burst.
    void takeDue (TimePoint now, std::vector<Slot*>& out)
    {
        for (Slot* slot : slots)
        {
            if (slot->due > now + kTolerance)
                continue;
            out.push_back (slot);
            slot->nominal += slot->period;
            if (slot->nominal <= now)
                slot->nominal = now + slot->period;
            slot->due = place (*slot, now);
        }
    }

    // Called just before a slot's callback runs. takeDue moved every slot of a pass
    // on together, so one that waited behind a slow callback can find its next
    // deadline already passed; it counts its period from now instead, as the
    // framework's timers do, rather than firing again at once.
    void rebaseIfOverdue (Slot& slot, TimePoint now) noexcept
    {
        if (! isScheduled (slot) || slot.due > now + kTolerance)
            return;
        slot.nominal = now + slot.period;
        slot.due = place (slot, now);
    }

    std::optional<TimePoint> nextDue() const noexcept
    {
        std::optional<TimePoint> next;
        for (const Slot* slot : slots)
            if (! next || slot->due < *next)
                next = slot->due;
        return next;
    }

    std::size_t size() const noexcept { return slots.size(); }

    // The first instant at or after t on the 60 Hz clock (step 1) or the 30 Hz
    // one (step 2). Instant k is at k / 60 s of the clock's epoch, computed in
    // thirds of a nanosecond so the instants do not drift.
    static TimePoint instantAtOrAfter (TimePoint t, int step) noexcept
    {
        const std::int64_t ns = std::chrono::duration_cast<Duration> (t.time_since_epoch()).count();
        const std::int64_t unit = std::int64_t { 50'000'000 } * step;
        const std::int64_t k = ns >= 0 ? (ns * 3 + unit - 1) / unit : (ns * 3) / unit;
        return TimePoint (std::chrono::duration_cast<Clock::duration> (Duration (k * unit / 3)));
    }

private:
    static TimePoint place (const Slot& slot, TimePoint now) noexcept
    {
        const int step = slot.period >= kCoarseMin ? 2 : slot.period >= kFineMin ? 1 : 0;
        if (step == 0)
            return slot.nominal;
        return instantAtOrAfter (std::max (slot.nominal, now + kTolerance + Duration (1)), step);
    }

    std::vector<Slot*> slots;
};
} // namespace dusk::detail
