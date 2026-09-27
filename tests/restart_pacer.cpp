#include <catch2/catch_test_macros.hpp>

#include "engine/hosting/RestartPacer.h"
#include "engine/vst3/Vst3RestartPolicy.h"

#include <vector>

using duskstudio::hosting::RestartPacer;
using Action = RestartPacer::Action;

// Each drain tick that restarts a native plug-in suspends the whole engine, so
// the pacer is what stands between a plug-in that asks on every tick and an
// output that drops out 30 times a second.

namespace
{
constexpr std::uint64_t kGeneration = 1;

struct Run
{
    std::vector<int> restarts;
    int gaveUp = 0;
};

// Drives `ticks` ticks, asking on every tick when `asking`.
Run drive (RestartPacer& pacer, int ticks, bool asking, int firstTick = 0,
           bool holdForTake = false, std::uint64_t generation = kGeneration)
{
    Run run;
    for (int t = firstTick; t < firstTick + ticks; ++t)
    {
        const auto action = pacer.tick (asking, holdForTake, generation);
        if (action == Action::Restart) run.restarts.push_back (t);
        if (action == Action::GaveUp) ++run.gaveUp;
    }
    return run;
}
} // namespace

TEST_CASE ("RestartPacer restarts at once, then spaces and holds back a plug-in that keeps asking",
           "[hosting][restart][issue-764]")
{
    RestartPacer pacer;
    const auto run = drive (pacer, 300, /*asking*/ true);

    REQUIRE_FALSE (run.restarts.empty());
    CHECK (run.restarts.front() == 0);
    CHECK ((int) run.restarts.size() == RestartPacer::kMaxBackToBack);
    for (size_t i = 1; i < run.restarts.size(); ++i)
        CHECK (run.restarts[i] - run.restarts[i - 1] == RestartPacer::kMinTicksBetween);
    CHECK (run.gaveUp == 1);
    CHECK (pacer.isHoldingBack());
}

TEST_CASE ("RestartPacer lands the last request of a burst", "[hosting][restart][issue-764]")
{
    RestartPacer pacer;
    CHECK (pacer.tick (true, false, kGeneration) == Action::Restart);
    for (int t = 1; t < 5; ++t)
        CHECK (pacer.tick (true, false, kGeneration) == Action::None);

    // Nothing asked since, yet the held request runs once the interval is out.
    const auto run = drive (pacer, 40, /*asking*/ false, /*firstTick*/ 5);
    REQUIRE (run.restarts.size() == 1);
    CHECK (run.restarts.front() == RestartPacer::kMinTicksBetween);
}

TEST_CASE ("RestartPacer holds a request through a take and runs it after",
           "[hosting][restart][issue-764]")
{
    RestartPacer pacer;
    CHECK (pacer.tick (true, /*holdForTake*/ true, kGeneration) == Action::None);
    const auto during = drive (pacer, 100, /*asking*/ false, 1, /*holdForTake*/ true);
    CHECK (during.restarts.empty());

    CHECK (pacer.tick (false, /*holdForTake*/ false, kGeneration) == Action::Restart);
}

TEST_CASE ("RestartPacer runs a held-back plug-in's last request once it goes quiet",
           "[hosting][restart][issue-764]")
{
    RestartPacer pacer;
    const auto storm = drive (pacer, 200, /*asking*/ true);
    REQUIRE (storm.gaveUp == 1);

    const auto quiet = drive (pacer, RestartPacer::kQuietTicks + 5, /*asking*/ false, 200);
    REQUIRE (quiet.restarts.size() == 1);
    CHECK (quiet.restarts.front() == 200 + RestartPacer::kQuietTicks - 1);
    CHECK_FALSE (pacer.isHoldingBack());

    // Back to normal: a lone request once the interval is out runs at once.
    REQUIRE (drive (pacer, RestartPacer::kMinTicksBetween, /*asking*/ false).restarts.empty());
    CHECK (pacer.tick (true, false, kGeneration) == Action::Restart);
}

TEST_CASE ("RestartPacer starts afresh for a reloaded plug-in", "[hosting][restart][issue-764]")
{
    RestartPacer pacer;
    REQUIRE (drive (pacer, 200, /*asking*/ true).gaveUp == 1);

    CHECK (pacer.tick (true, false, kGeneration + 1) == Action::Restart);
    CHECK_FALSE (pacer.isHoldingBack());
}

namespace
{
using duskstudio::vst3::Vst3RestartPolicy;
using Vst3Action = Vst3RestartPolicy::Action;

// A native VST3 plug-in as the engine drain sees it: the bus-layout change flag
// is a level the drain clears when it restarts the plug-in or drops the change,
// and the slot is online unless the drain took it offline.
struct Vst3Plugin
{
    bool ioPending = false;
    bool online = true;
    // Announces another change during every restart, one that really moves its
    // buses, so Vst3Instance::activate keeps it.
    bool changesBusesInActivation = false;
};

struct Vst3Run
{
    std::vector<int> restarts;
    std::vector<int> takenOffline;
    int dropped = 0;
};

// One drain tick: what the engine does with the policy's answer.
Vst3Action drainTick (Vst3RestartPolicy& policy, Vst3Plugin& plugin, bool latencyChanged,
                      bool holdForTake, std::uint64_t generation = kGeneration)
{
    const auto action = policy.tick (latencyChanged, plugin.ioPending, plugin.online,
                                     holdForTake, generation);
    switch (action)
    {
        case Vst3Action::Restart:
            plugin.ioPending = plugin.changesBusesInActivation;
            plugin.online = true;
            break;
        case Vst3Action::TakeOffline:
            plugin.online = false;
            plugin.ioPending = false;
            break;
        case Vst3Action::DropIoChange:
            plugin.ioPending = false;
            break;
        case Vst3Action::None:
        case Vst3Action::LatencyGaveUp:
            break;
    }
    return action;
}

// Drives `ticks` ticks; `raise (t)` says whether the plug-in announces a
// bus-layout change on tick t, before the drain looks.
template <typename Raise>
Vst3Run driveVst3 (Vst3RestartPolicy& policy, Vst3Plugin& plugin, int ticks, Raise&& raise,
                   bool holdForTake = false, int firstTick = 0)
{
    Vst3Run run;
    for (int t = firstTick; t < firstTick + ticks; ++t)
    {
        if (raise (t)) plugin.ioPending = true;
        switch (drainTick (policy, plugin, false, holdForTake))
        {
            case Vst3Action::Restart:      run.restarts.push_back (t); break;
            case Vst3Action::TakeOffline:  run.takenOffline.push_back (t); break;
            case Vst3Action::DropIoChange: ++run.dropped; break;
            case Vst3Action::None:
            case Vst3Action::LatencyGaveUp:
                break;
        }
    }
    return run;
}
} // namespace

TEST_CASE ("Vst3RestartPolicy restarts a plug-in for a bus-layout change at once, take or no take",
           "[hosting][restart][vst3][issue-769]")
{
    Vst3RestartPolicy policy;
    Vst3Plugin plugin;

    SECTION ("while a take records")
    {
        const auto run = driveVst3 (policy, plugin, 90, [] (int t) { return t == 0 || t == 40; },
                                    /*holdForTake*/ true);
        CHECK (run.restarts == std::vector<int> { 0, 40 });
        CHECK (run.takenOffline.empty());
        CHECK_FALSE (plugin.ioPending);
    }

    SECTION ("a second change soon after a restart is not made to wait")
    {
        const auto run = driveVst3 (policy, plugin, 20, [] (int t) { return t == 0 || t == 3; });
        CHECK (run.restarts == std::vector<int> { 0, 3 });
        CHECK_FALSE (plugin.ioPending);
    }
}

TEST_CASE ("Vst3RestartPolicy still holds a latency-only change through a take",
           "[hosting][restart][vst3][issue-769]")
{
    Vst3RestartPolicy policy;
    Vst3Plugin plugin;

    CHECK (drainTick (policy, plugin, /*latencyChanged*/ true, /*holdForTake*/ true) == Vst3Action::None);
    for (int t = 1; t < 100; ++t)
        REQUIRE (drainTick (policy, plugin, false, /*holdForTake*/ true) == Vst3Action::None);

    SECTION ("and runs it once the take ends")
    {
        CHECK (drainTick (policy, plugin, false, /*holdForTake*/ false) == Vst3Action::Restart);
        CHECK (drainTick (policy, plugin, false, false) == Vst3Action::None);
    }

    SECTION ("unless a bus-layout change restarted the plug-in meanwhile, which read the latency too")
    {
        plugin.ioPending = true;
        CHECK (drainTick (policy, plugin, false, /*holdForTake*/ true) == Vst3Action::Restart);
        for (int t = 0; t < 60; ++t)
            REQUIRE (drainTick (policy, plugin, false, /*holdForTake*/ false) == Vst3Action::None);
    }
}

TEST_CASE ("Vst3RestartPolicy takes a plug-in that keeps changing its buses offline instead of restarting it without end",
           "[hosting][restart][vst3][issue-769]")
{
    Vst3RestartPolicy policy;
    Vst3Plugin plugin;

    SECTION ("one that announces a change on every tick")
    {
        const auto run = driveVst3 (policy, plugin, 300, [] (int) { return true; });
        CHECK ((int) run.restarts.size() == Vst3RestartPolicy::kMaxBackToBack);
        CHECK (run.restarts.back() == Vst3RestartPolicy::kMaxBackToBack - 1);
        // Offline, and told once: the caller quarantines the slot so it passes
        // the dry signal, and raises the alert for the first time only.
        CHECK (run.takenOffline == std::vector<int> { Vst3RestartPolicy::kMaxBackToBack });
        CHECK (run.dropped == 300 - Vst3RestartPolicy::kMaxBackToBack - 1);
        CHECK (policy.isOffline());
        CHECK (policy.timesTakenOffline() == 1);
        CHECK_FALSE (plugin.online);
    }

    SECTION ("one that changes its buses again inside every restart")
    {
        plugin.changesBusesInActivation = true;
        const auto run = driveVst3 (policy, plugin, 300, [] (int t) { return t == 0; });
        // Its burst, one more try once it has been quiet, and no more.
        REQUIRE ((int) run.restarts.size() == Vst3RestartPolicy::kMaxBackToBack + 1);
        CHECK (run.restarts.back() == Vst3RestartPolicy::kMaxBackToBack + Vst3RestartPolicy::kQuietTicks);
        CHECK (run.takenOffline.size() == 2);
        CHECK (policy.isOffline());
        CHECK (policy.timesTakenOffline() == 2);
        CHECK_FALSE (plugin.online);
    }
}

TEST_CASE ("Vst3RestartPolicy brings an offline plug-in back once it stops changing its buses",
           "[hosting][restart][vst3][issue-769]")
{
    Vst3RestartPolicy policy;
    Vst3Plugin plugin;
    const auto burst = driveVst3 (policy, plugin, 10, [] (int) { return true; });
    REQUIRE (burst.takenOffline.size() == 1);
    REQUIRE_FALSE (plugin.online);

    SECTION ("after a quiet second")
    {
        const auto quiet = driveVst3 (policy, plugin, Vst3RestartPolicy::kQuietTicks + 5,
                                      [] (int) { return false; }, false, 10);
        REQUIRE (quiet.restarts == std::vector<int> { 10 + Vst3RestartPolicy::kQuietTicks - 1 });
        CHECK (plugin.online);
        CHECK_FALSE (policy.isOffline());

        // A lone change once that restart is out of the way runs at once again.
        const auto later = driveVst3 (policy, plugin, 30, [] (int t) { return t == 65; },
                                      false, 10 + Vst3RestartPolicy::kQuietTicks + 5);
        CHECK (later.restarts == std::vector<int> { 65 });
        CHECK (later.takenOffline.empty());
    }

    SECTION ("but not while a take records, since it passes the dry signal meanwhile")
    {
        const auto during = driveVst3 (policy, plugin, 100, [] (int) { return false; },
                                       /*holdForTake*/ true, 10);
        CHECK (during.restarts.empty());
        CHECK (policy.isOffline());
        CHECK_FALSE (plugin.online);

        const auto after = driveVst3 (policy, plugin, 5, [] (int) { return false; }, false, 110);
        CHECK (after.restarts == std::vector<int> { 110 });
        CHECK (plugin.online);
    }

    SECTION ("when another reactivation, such as a device change, brings it back")
    {
        plugin.online = true;
        const auto run = driveVst3 (policy, plugin, 5, [] (int t) { return t == 12; }, false, 10);
        CHECK (run.restarts == std::vector<int> { 12 });
        CHECK_FALSE (policy.isOffline());
    }

    SECTION ("when it is reloaded")
    {
        plugin.ioPending = true;
        CHECK (drainTick (policy, plugin, false, false, kGeneration + 1) == Vst3Action::Restart);
        CHECK_FALSE (policy.isOffline());
        CHECK (policy.timesTakenOffline() == 0);
    }
}
