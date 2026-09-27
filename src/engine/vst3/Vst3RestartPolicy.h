#pragma once

#include "../hosting/RestartPacer.h"

#include <algorithm>
#include <cstdint>

namespace duskstudio::vst3
{
// Decides, one engine-drain tick at a time, what a native VST3 slot's restart
// requests get. Every restart suspends the whole engine for a moment.
//
// A latency change goes through a RestartPacer: held while a take records,
// spaced out, and held back for a plug-in that keeps asking.
//
// A bus-layout change (kIoChanged) cannot wait like that: the plug-in outputs
// silence from the moment it announces one until it has been restarted. It
// runs on the next tick, take or no take, and serves a latency change waiting
// beside it. What is guarded against is a plug-in that announces another as
// soon as it has been restarted:
//
//   - A change within kBackToBackTicks of the previous restart for one counts
//     as back to back. The kMaxBackToBack-th in a row takes the plug-in
//     offline instead: the caller quarantines the slot, which then passes the
//     dry signal (silence, for an instrument), drops the change and tells the
//     user the first time.
//   - While offline, each further change is dropped and starts the quiet time
//     again, and a latency change is dropped too: the restart that brings the
//     plug-in back reads its latency. After kQuietTicks without a change the
//     plug-in is restarted, once no take is recording: offline it is not
//     silent, so that restart has no reason to cut a gap into a take. If it
//     announces another change within kBackToBackTicks of that restart, it
//     goes offline for good.
//   - A reload (a new slot generation) starts afresh. The slot coming back
//     online through another reactivation, such as a device change, returns
//     it to service with a fresh back-to-back count, but the times it has been
//     taken offline still count, so the user is not told again for this load.
class Vst3RestartPolicy
{
public:
    static constexpr int kBackToBackTicks = hosting::RestartPacer::kMinTicksBetween;
    static constexpr int kMaxBackToBack   = hosting::RestartPacer::kMaxBackToBack;
    static constexpr int kQuietTicks      = hosting::RestartPacer::kQuietTicks;

    enum class Action
    {
        None,
        Restart,         // reactivate under the engine fence
        LatencyGaveUp,   // the latency pacer has started holding back
        TakeOffline,     // quarantine the slot under the fence, drop the change
        DropIoChange,    // already offline: drop the change
    };

    // latencyChanged: announced since the last tick. ioChangePending: the
    // plug-in's bus-layout change flag is set. online: the slot processes audio
    // (it is not quarantined). holdForTake: a take is recording. generation:
    // the slot's load generation.
    Action tick (bool latencyChanged, bool ioChangePending, bool online,
                 bool holdForTake, std::uint64_t generation) noexcept
    {
        if (generation != loadedGeneration)
        {
            *this = Vst3RestartPolicy {};
            loadedGeneration = generation;
        }
        if (ticksSinceIoRestart < kLongAgo) ++ticksSinceIoRestart;

        if (isOffline() && online)
        {
            io = Io::Serving;
            backToBack = 0;
            probation = false;
        }

        if (isOffline())
        {
            if (ioChangePending)
            {
                ticksQuiet = 0;
                return Action::DropIoChange;
            }
            if (io == Io::OfflineForGood)
                return Action::None;
            ticksQuiet = std::min (ticksQuiet + 1, kQuietTicks);
            if (ticksQuiet < kQuietTicks || holdForTake)
                return Action::None;
            io = Io::Serving;
            backToBack = 0;
            probation = true;
            return restartForIo (generation);
        }

        if (ioChangePending)
        {
            const bool soon = ticksSinceIoRestart < kBackToBackTicks;
            if (soon && probation)
                return takeOffline (Io::OfflineForGood);
            probation = false;
            backToBack = soon ? backToBack + 1 : 0;
            if (backToBack >= kMaxBackToBack)
                return takeOffline (Io::Offline);
            return restartForIo (generation);
        }

        switch (latency.tick (latencyChanged, holdForTake, generation))
        {
            case hosting::RestartPacer::Action::None:    return Action::None;
            case hosting::RestartPacer::Action::Restart: return Action::Restart;
            case hosting::RestartPacer::Action::GaveUp:  return Action::LatencyGaveUp;
        }
        return Action::None;
    }

    bool isOffline() const noexcept { return io != Io::Serving; }
    // How many times this load has been taken offline.
    int timesTakenOffline() const noexcept { return offlineCount; }

private:
    enum class Io { Serving, Offline, OfflineForGood };
    static constexpr int kLongAgo = 1 << 20;

    Action restartForIo (std::uint64_t generation) noexcept
    {
        ticksSinceIoRestart = 0;
        latency.restartedAnyway (generation);
        return Action::Restart;
    }

    Action takeOffline (Io to) noexcept
    {
        io = to;
        ticksQuiet = 0;
        ++offlineCount;
        return Action::TakeOffline;
    }

    hosting::RestartPacer latency;
    std::uint64_t loadedGeneration = 0;
    Io   io = Io::Serving;
    int  ticksSinceIoRestart = kLongAgo;
    int  ticksQuiet = 0;
    int  backToBack = 0;
    int  offlineCount = 0;
    bool probation = false;
};
} // namespace duskstudio::vst3
