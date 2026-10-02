#include "AutomationLaneEdit.h"

#include <algorithm>
#include <cmath>

namespace duskstudio
{
namespace
{
bool earlier (const AutomationPoint& a, const AutomationPoint& b) noexcept
{
    return a.timeSamples < b.timeSamples;
}
} // namespace

float quantizeAutomationValue (AutomationParam param, float v01) noexcept
{
    const float clamped = std::clamp (v01, 0.0f, 1.0f);
    if (isContinuousParam (param))
        return clamped;
    return clamped >= 0.5f ? 1.0f : 0.0f;
}

int insertAutomationPoint (std::vector<AutomationPoint>& points, const AutomationPoint& point)
{
    const auto index = std::upper_bound (points.begin(), points.end(), point, earlier) - points.begin();
    points.insert (points.begin() + index, point);
    return static_cast<int> (index);
}

int moveAutomationPoint (std::vector<AutomationPoint>& points, int index, std::int64_t time, float value)
{
    if (index < 0 || index >= static_cast<int> (points.size()))
        return -1;
    auto moved = points[static_cast<std::size_t> (index)];
    moved.timeSamples = time;
    moved.value = value;
    points.erase (points.begin() + index);
    return insertAutomationPoint (points, moved);
}

void paintAutomationStep (std::vector<AutomationPoint>& points, AutomationStroke& stroke,
                          std::int64_t time, float value, float bpm, float x, float stepPixels)
{
    if (stroke.started && std::abs (x - stroke.lastX) < stepPixels)
    {
        const auto last = std::find_if (points.begin(), points.end(), [&stroke] (const AutomationPoint& p)
                                        { return p.timeSamples == stroke.lastTime; });
        if (last != points.end())
        {
            last->value = value;
            last->recordedAtBPM = bpm;
        }
        return;
    }

    // The ends of the swept band survive: the start is the last point laid and the
    // end is the point this step lays.
    if (stroke.started)
    {
        const auto lo = std::min (stroke.lastTime, time);
        const auto hi = std::max (stroke.lastTime, time);
        points.erase (std::remove_if (points.begin(), points.end(), [lo, hi] (const AutomationPoint& p)
                                      { return p.timeSamples > lo && p.timeSamples < hi; }),
                      points.end());
    }

    const auto exact = std::find_if (points.begin(), points.end(), [time] (const AutomationPoint& p)
                                     { return p.timeSamples == time; });
    if (exact != points.end())
    {
        exact->value = value;
        exact->recordedAtBPM = bpm;
    }
    else
    {
        AutomationPoint point;
        point.timeSamples = time;
        point.value = value;
        point.recordedAtBPM = bpm;
        insertAutomationPoint (points, point);
    }

    stroke.started = true;
    stroke.lastTime = time;
    stroke.lastX = x;
}

AutomationMode automationModeAfterEdit (AutomationMode mode) noexcept
{
    return mode == AutomationMode::Off || mode == AutomationMode::Write ? AutomationMode::Read : mode;
}
} // namespace duskstudio
