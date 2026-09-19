#include "AutomationRecorder.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace duskstudio
{
namespace
{
// A continuous control that has not moved is still sampled this often, so a
// long hold does not collapse to one point at its start.
constexpr std::int64_t kMaxHoldSamples = 22050;
constexpr float kMovedEpsilon = 0.001f;
} // namespace

void AutomationPassRecorder::record (AutomationLane& lane, std::int64_t playhead,
                                     float value, float bpm, std::uint32_t locates)
{
    if (active() && &lane.pointsConst() != base)
        drop (lane);
    if (active() && (playhead < spanEnd || locates != passLocates))
        finish (lane, 0);

    AutomationPoint point;
    point.timeSamples = playhead;
    point.value = normalizeAutomationValue (param, value);
    point.recordedAtBPM = bpm;

    if (! active())
    {
        lane.passOpen.store (true, std::memory_order_release);
        base = &lane.pointsConst();
        passLocates = locates;
        pass.push_back (point);
    }
    else if (pass.back().timeSamples == playhead)
    {
        pass.back() = point;
    }
    else
    {
        const auto& last = pass.back();
        const bool moved = isContinuousParam (param)
            ? std::abs (point.value - last.value) >= kMovedEpsilon
                  || playhead - last.timeSamples >= kMaxHoldSamples
            : (point.value >= 0.5f) != (last.value >= 0.5f);
        if (moved)
            pass.push_back (point);
    }

    spanEnd = playhead;
    lastValue = point.value;
    lastBpm = bpm;
}

void AutomationPassRecorder::drop (AutomationLane& lane) noexcept
{
    pass.clear();
    lane.passOpen.store (false, std::memory_order_release);
}

void AutomationPassRecorder::finish (AutomationLane& lane, std::int64_t returnSamples)
{
    if (! active()) return;
    if (&lane.pointsConst() != base)
    {
        drop (lane);
        return;
    }

    const auto& old = lane.pointsConst();
    const auto spanStart = pass.front().timeSamples;
    if (pass.back().timeSamples < spanEnd)
        pass.push_back ({ spanEnd, lastValue, lastBpm });

    std::vector<AutomationPoint> spliced;
    spliced.reserve (old.size() + pass.size() + 2);
    for (const auto& point : old)
        if (point.timeSamples < spanStart)
            spliced.push_back (point);
    // Hold the earlier ride right up to the span, or the stretch before it
    // would slope from the ride's last point into the pass.
    if (! spliced.empty() && spliced.back().timeSamples < spanStart - 1)
    {
        AutomationPoint entry;
        entry.timeSamples = spanStart - 1;
        entry.value = normalizeAutomationValue (param, evaluateLane (old, spanStart - 1, param));
        entry.recordedAtBPM = spliced.back().recordedAtBPM;
        spliced.push_back (entry);
    }
    spliced.insert (spliced.end(), pass.begin(), pass.end());

    // The earlier ride picks up again after the span, if it has anything there.
    if (! old.empty() && old.back().timeSamples > spanEnd)
    {
        const auto resumeAt = spanEnd + std::max<std::int64_t> (1, returnSamples);
        AutomationPoint resume;
        resume.timeSamples = resumeAt;
        resume.value = normalizeAutomationValue (param, evaluateLane (old, resumeAt, param));
        resume.recordedAtBPM = lastBpm;
        spliced.push_back (resume);
        for (const auto& point : old)
            if (point.timeSamples > resumeAt)
                spliced.push_back (point);
    }

    lane.publishPoints (std::move (spliced));
    drop (lane);
}

namespace
{
template <std::size_t... Index>
AutomationPassRecorders makeRecorders (std::index_sequence<Index...>)
{
    return {{ AutomationPassRecorder ((AutomationParam) Index)... }};
}
} // namespace

AutomationPassRecorders makeAutomationPassRecorders()
{
    return makeRecorders (std::make_index_sequence<(std::size_t) kNumAutomationParams>());
}
} // namespace duskstudio
