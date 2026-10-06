#pragma once

#include <cstdint>
#include <initializer_list>
#include <limits>

// How far apart the audio editor's ruler puts its bar numbers and time stamps. Both
// are thinned by the room a number needs, and by a ceiling on how many the span in
// view may hold, so a frame draws a bounded number of marks whatever the zoom, the
// tempo map or the sample rate make of that span.
namespace duskstudio::imgui::ruler
{
// The room a bar number needs, in design pixels, and a time stamp past the 30 s step.
constexpr double kMinMarkPixels = 64.0;
// The most marks one ruler draws.
constexpr std::int64_t kMaxMarks = 1024;

// Bars from one bar number to the next: the timeline ruler's ladder (every bar from
// 64 px a bar, then every 2nd, 4th, 8th and 16th as bars halve), carried on past 16.
inline std::int64_t barStep (double pixelsPerBar, std::int64_t barsInView) noexcept
{
    constexpr auto kLargest = std::numeric_limits<std::int64_t>::max() / 2;
    std::int64_t step = 1;
    if (pixelsPerBar > 0.0)
        while (static_cast<double> (step) * pixelsPerBar < kMinMarkPixels && step < kLargest)
            step *= 2;
    while (barsInView / step > kMaxMarks && step < kLargest)
        step *= 2;
    return step;
}

// Seconds from one time stamp to the next. 1, 5, 10 and 30 s change where the timeline
// ruler changes between them, which puts stamps as close as 40 px; past 30 s the steps
// are minutes and hours, each at least kMinMarkPixels apart.
inline double stampSeconds (double pixelsPerSecond, double secondsInView) noexcept
{
    constexpr double kLargest = 1.0e15;
    double step = 30.0;
    if (pixelsPerSecond >= 40.0)
        step = 1.0;
    else if (pixelsPerSecond >= 16.0)
        step = 5.0;
    else if (pixelsPerSecond >= 6.0)
        step = 10.0;
    else if (pixelsPerSecond > 0.0)
    {
        for (const double longer : { 60.0, 120.0, 300.0, 600.0, 1800.0, 3600.0 })
        {
            if (step * pixelsPerSecond >= kMinMarkPixels)
                break;
            step = longer;
        }
        while (step * pixelsPerSecond < kMinMarkPixels && step < kLargest)
            step *= 2.0;
    }
    while (secondsInView / step > static_cast<double> (kMaxMarks) && step < kLargest)
        step *= 2.0;
    return step;
}
} // namespace duskstudio::imgui::ruler
