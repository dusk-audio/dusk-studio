#pragma once

#include <cstdint>

namespace duskstudio::hosting
{
// Decides, one message-thread tick at a time, when a native plug-in's request
// for a deactivate / activate cycle (a CLAP request_restart, a VST3 latency or
// I/O restart) is carried out. Every cycle suspends the whole engine for a
// moment, so a plug-in that asks on every tick would otherwise cut the output
// out continuously.
//
//   - A request is held while a take records: the suspend would put a gap in
//     it. It runs on the first tick after the take ends.
//   - A request that arrives within kMinTicksBetween of the last cycle waits
//     out the rest of that interval, then runs, so the latest latency still
//     lands.
//   - After kMaxBackToBack such back-to-back cycles the plug-in is treated as
//     asking without end: its requests are held until it has been quiet for
//     kQuietTicks, and then run once. tick() reports that moment once, so the
//     caller can log it once.
//   - A reload (a new slot generation) starts afresh.
class RestartPacer
{
public:
    // At the engine drain's 30 Hz: half a second, about three seconds of
    // asking without a break, and one quiet second.
    static constexpr int kMinTicksBetween = 15;
    static constexpr int kMaxBackToBack   = 6;
    static constexpr int kQuietTicks      = 30;

    enum class Action { None, Restart, GaveUp };

    // requested: the plug-in asked since the last tick. holdForTake: a take is
    // recording. generation: the slot's load generation.
    Action tick (bool requested, bool holdForTake, std::uint64_t generation) noexcept
    {
        if (generation != loadedGeneration)
        {
            *this = RestartPacer {};
            loadedGeneration = generation;
        }

        ticksSinceRestart = saturatingIncrement (ticksSinceRestart);
        ticksSinceRequest = requested ? 0 : saturatingIncrement (ticksSinceRequest);
        pending = pending || requested;

        if (! pending || holdForTake)
            return Action::None;

        if (gaveUp)
        {
            if (ticksSinceRequest < kQuietTicks)
                return Action::None;
            gaveUp = false;
            backToBack = 0;
        }
        else
        {
            if (ticksSinceRestart < kMinTicksBetween)
                return Action::None;
            backToBack = ticksSinceRestart == kMinTicksBetween ? backToBack + 1 : 0;
            if (backToBack >= kMaxBackToBack)
            {
                gaveUp = true;
                return Action::GaveUp;
            }
        }

        pending = false;
        ticksSinceRestart = 0;
        return Action::Restart;
    }

    bool isHoldingBack() const noexcept { return gaveUp; }

private:
    static constexpr int kLongAgo = 1 << 20;

    static int saturatingIncrement (int ticks) noexcept
    {
        return ticks < kLongAgo ? ticks + 1 : ticks;
    }

    std::uint64_t loadedGeneration = 0;
    int  ticksSinceRestart = kLongAgo;
    int  ticksSinceRequest = kLongAgo;
    int  backToBack = 0;
    bool pending = false;
    bool gaveUp  = false;
};
} // namespace duskstudio::hosting
