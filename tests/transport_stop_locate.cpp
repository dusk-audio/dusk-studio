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

    SECTION ("A request during a callback survives its final position publication")
    {
        t.setState (Transport::State::Playing);
        t.onLocate = [&] (std::int64_t s) { t.requestLocate (s); };
        CHECK (t.beginAudioBlock() == 1000);
        std::int64_t requested = 5000, endpoint = 1256;
        bool newerRequest = false;
        SECTION ("A normal callback") {}
        SECTION ("A wrapping callback") { endpoint = 100; }
        SECTION ("A repeated position") { requested = 1000; }
        SECTION ("A negative target") { requested = -48000; }
        SECTION ("A newer request after consumption") { newerRequest = true; }
        t.locate (requested);
        CHECK (t.getPlayhead() == requested);
        t.finishAudioBlock (endpoint);
        CHECK (t.getCommittedPlayhead() == endpoint);
        CHECK (t.getPlayhead() == requested);
        CHECK (t.getRollStart() == requested);
        CHECK (t.getLocateCount() == 0);
        CHECK (t.beginAudioBlock() == requested);
        CHECK (t.getLocateCount() == 1);
        const auto following = newerRequest ? 7000 : requested;
        t.locate (following);
        t.finishAudioBlock (requested + 256);
        CHECK (t.beginAudioBlock() == following);
        CHECK (t.getLocateCount() == 2);
        t.finishAudioBlock (following + 256);
        CHECK (t.getPlayhead() == following + 256);
    }

    SECTION ("Explicit Stop positioning discards a request audio never consumed")
    {
        t.setState (Transport::State::Recording);
        CHECK (t.beginAudioBlock() == 1000);
        t.requestLocate (5000);
        t.finishAudioBlock (1256);
        CHECK (t.getCommittedPlayhead() == 1256);
        t.setState (Transport::State::Stopped);
        t.setPlayhead (t.getCommittedPlayhead());
        CHECK (t.getPlayhead() == 1256);
        CHECK (t.beginAudioBlock() == 1256);
        CHECK (t.getRollStart() == 1000);
    }
    SECTION ("cold seeks leave audio and automation on the committed timeline until ready")
    {
        t.setState (Transport::State::Playing);
        t.requestLocate (5000);
        for (int block = 0; block < 32; ++block)
        {
            CHECK (t.beginAudioBlock ([] (std::int64_t) noexcept { return false; }) == 1000 + 256 * block);
            CHECK (t.getPlayhead() == 5000);
            const auto position = t.getAutomationPosition();
            CHECK (position.samples == 1000 + 256 * block);
            CHECK (position.locateSerial == 0);
            t.finishAudioBlock (position.samples + 256);
        }
        CHECK (t.beginAudioBlock ([] (std::int64_t) noexcept { return true; }) == 5000);
        const auto position = t.getAutomationPosition();
        CHECK (position.samples == 5000);
        CHECK (position.locateSerial == 1);
    }
    SECTION ("a newer request cannot consume an older target's readiness")
    {
        t.setState (Transport::State::Recording);
        t.requestLocate (5000);
        std::int64_t checkedTarget = -1;
        CHECK (t.beginAudioBlock ([&] (std::int64_t target) noexcept
        {
            checkedTarget = target;
            t.requestLocate (7000);
            return true;
        }) == 1000);
        CHECK (checkedTarget == 5000);
        CHECK (t.getPlayhead() == 7000);
        CHECK (t.getAutomationPosition().locateSerial == 0);
        CHECK (t.beginAudioBlock ([] (std::int64_t target) noexcept { return target == 5000; }) == 1000);
        CHECK (t.beginAudioBlock ([] (std::int64_t target) noexcept { return target == 7000; }) == 7000);
        CHECK (t.getAutomationPosition().locateSerial == 1);
        CHECK (t.getRollStart() == 1000);
    }
    SECTION ("stopping cancels a still-cold seek without a committed locate serial")
    {
        t.setState (Transport::State::Playing);
        t.requestLocate (5000);
        CHECK (t.beginAudioBlock ([] (std::int64_t) noexcept { return false; }) == 1000);
        t.finishAudioBlock (1256);
        t.setState (Transport::State::Stopped);
        t.setPlayhead (1256);
        CHECK (t.beginAudioBlock() == 1256);
        CHECK (t.getPlayhead() == 1256);
        CHECK (t.getAutomationPosition().samples == 1256);
        CHECK (t.getAutomationPosition().locateSerial == 0);
    }
    SECTION ("readiness can map a loop target while consuming the original request")
    {
        t.setState (Transport::State::Playing);
        t.requestLocate (5000);
        std::int64_t checkedTarget = -1;
        CHECK (t.beginAudioBlock ([&] (std::int64_t& target) noexcept
        {
            checkedTarget = target;
            target = 2000;
            return true;
        }) == 2000);
        CHECK (checkedTarget == 5000);
        CHECK (t.getPlayhead() == 2000);
        CHECK (t.getAutomationPosition().locateSerial == 1);
        t.requestLocate (7000);
        CHECK (t.beginAudioBlock ([&] (std::int64_t& target) noexcept
        {
            target = 2500;
            t.requestLocate (9000);
            return true;
        }) == 2000);
        CHECK (t.getPlayhead() == 9000);
        CHECK (t.getAutomationPosition().locateSerial == 1);
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
