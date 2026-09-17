#pragma once

#include <algorithm>
#include <cstdint>

namespace duskstudio
{
// Left edge of a timeline view, in samples, that keeps the playhead in sight
// without touching the zoom. The view holds still while the playhead sits
// inside it short of a small right-hand margin; otherwise the page turns so
// the playhead lands a quarter of the way in.
inline std::int64_t followPlayheadScroll (std::int64_t playhead,
                                          std::int64_t scroll,
                                          std::int64_t visibleSamples) noexcept
{
    if (visibleSamples <= 0)
        return scroll;

    const std::int64_t rightMargin = visibleSamples / 32;
    if (playhead >= scroll && playhead < scroll + visibleSamples - rightMargin)
        return scroll;

    return std::max<std::int64_t> (0, playhead - visibleSamples / 4);
}

// The tape strip's unzoomed window: at least a minute, or the content plus a
// fifth of blank tape past it.
inline double tapeStripUnzoomedSeconds (double contentSeconds) noexcept
{
    return std::max (60.0, contentSeconds * 1.20);
}

// Zoom factor that makes content ending at contentSeconds fill the strip. With
// nothing to fit it is the unzoomed window, not the tightest zoom the clamp
// allows, which would leave a new session showing two seconds of tape.
inline float tapeStripFitZoom (double contentSeconds) noexcept
{
    if (! (contentSeconds > 0.0))
        return 1.0f;
    const double fit = tapeStripUnzoomedSeconds (contentSeconds) / std::max (0.001, contentSeconds);
    return std::clamp ((float) fit, 0.1f, 32.0f);
}
} // namespace duskstudio
