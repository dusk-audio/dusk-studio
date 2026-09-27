#include <catch2/catch_test_macros.hpp>

#include "engine/hosting/RestartPacer.h"

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
