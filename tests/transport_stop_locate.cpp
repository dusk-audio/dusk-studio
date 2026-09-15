#include <catch2/catch_test_macros.hpp>

#include "engine/StopBehavior.h"
#include "engine/Transport.h"

#include <cstdint>

using duskstudio::StopBehavior;
using duskstudio::Transport;
using duskstudio::playheadAfterStop;

namespace
{
constexpr std::int64_t kRollStart   = 48000;
constexpr std::int64_t kLastClicked = 96000;
} // namespace

TEST_CASE ("Stop pressed while stopped returns to zero whatever the setting",
           "[transport][stop]")
{
    for (auto behavior : { StopBehavior::PauseInPlace, StopBehavior::ReturnToZero,
                           StopBehavior::ReturnToLastClicked, StopBehavior::ReturnToRollStart })
    {
        const auto target = playheadAfterStop (false, behavior, kRollStart, kLastClicked);
        REQUIRE (target.has_value());
        CHECK (*target == 0);
    }
}

TEST_CASE ("Stop on a rolling transport follows the Playhead on Stop setting",
           "[transport][stop]")
{
    SECTION ("return to where play or record started")
    {
        const auto target = playheadAfterStop (true, StopBehavior::ReturnToRollStart,
                                               kRollStart, kLastClicked);
        REQUIRE (target.has_value());
        CHECK (*target == kRollStart);
    }

    SECTION ("stay where it is")
    {
        CHECK_FALSE (playheadAfterStop (true, StopBehavior::PauseInPlace,
                                        kRollStart, kLastClicked).has_value());
    }

    SECTION ("return to zero")
    {
        const auto target = playheadAfterStop (true, StopBehavior::ReturnToZero,
                                               kRollStart, kLastClicked);
        REQUIRE (target.has_value());
        CHECK (*target == 0);
    }

    SECTION ("return to the last ruler click")
    {
        const auto target = playheadAfterStop (true, StopBehavior::ReturnToLastClicked,
                                               kRollStart, kLastClicked);
        REQUIRE (target.has_value());
        CHECK (*target == kLastClicked);
    }

    SECTION ("last clicked with no click yet stays where it is")
    {
        CHECK_FALSE (playheadAfterStop (true, StopBehavior::ReturnToLastClicked,
                                        kRollStart, -1).has_value());
    }
}

TEST_CASE ("A locate during playback becomes the point Stop returns to",
           "[transport][stop]")
{
    Transport t;
    t.setPlayhead (1000);
    t.setRollStart (1000);

    SECTION ("stopped: the playhead moves, the roll start does not")
    {
        t.locate (5000);
        CHECK (t.getPlayhead() == 5000);
        CHECK (t.getRollStart() == 1000);
    }

    SECTION ("playing: both move")
    {
        t.setState (Transport::State::Playing);
        t.locate (5000);
        CHECK (t.getPlayhead() == 5000);
        CHECK (t.getRollStart() == 5000);
    }

    SECTION ("recording: the take keeps its start")
    {
        t.setState (Transport::State::Recording);
        t.locate (5000);
        CHECK (t.getPlayhead() == 5000);
        CHECK (t.getRollStart() == 1000);
    }
}

TEST_CASE ("The persisted Playhead on Stop values keep their meaning",
           "[transport][stop]")
{
    CHECK (static_cast<int> (StopBehavior::PauseInPlace)        == 0);
    CHECK (static_cast<int> (StopBehavior::ReturnToZero)        == 1);
    CHECK (static_cast<int> (StopBehavior::ReturnToLastClicked) == 2);
    CHECK (static_cast<int> (StopBehavior::ReturnToRollStart)   == 3);
}
