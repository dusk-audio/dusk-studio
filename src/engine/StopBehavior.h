#pragma once

#include <cstdint>
#include <optional>

namespace duskstudio
{
// Playhead on Stop. The values are persisted per-machine, so each keeps its
// meaning for good.
enum class StopBehavior : int
{
    PauseInPlace        = 0,
    ReturnToZero        = 1,
    ReturnToLastClicked = 2,
    ReturnToRollStart   = 3,
};

// Where a press of Stop leaves the playhead; nullopt leaves it where it is. A
// press while the transport is already stopped returns to zero whatever the
// setting. rollStart is where play or record last started; lastClicked is the
// last ruler click, negative when there has not been one.
inline std::optional<std::int64_t> playheadAfterStop (bool wasRolling,
                                                       StopBehavior behavior,
                                                       std::int64_t rollStart,
                                                       std::int64_t lastClicked) noexcept
{
    if (! wasRolling)
        return std::int64_t { 0 };

    switch (behavior)
    {
        case StopBehavior::PauseInPlace:
            return std::nullopt;
        case StopBehavior::ReturnToZero:
            return std::int64_t { 0 };
        case StopBehavior::ReturnToLastClicked:
            if (lastClicked >= 0)
                return lastClicked;
            return std::nullopt;
        case StopBehavior::ReturnToRollStart:
            return rollStart;
    }
    return std::nullopt;
}
} // namespace duskstudio
