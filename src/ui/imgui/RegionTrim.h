#pragma once

#include "../../session/Session.h"

#include <algorithm>
#include <cstdint>

// What the audio editor's trim handles leave of a region, from the region as the
// drag found it and the file sample under the pointer. Reading from the drag's
// start, not the region as the drag has left it, lets a handle go back out past
// where the drag has pulled it.
namespace duskstudio::imgui::trim
{
inline void fitFades (AudioRegion& region)
{
    region.fadeInSamples = std::clamp<std::int64_t> (region.fadeInSamples, 0, region.lengthInSamples);
    region.fadeOutSamples = std::clamp<std::int64_t> (region.fadeOutSamples, 0,
                                                      region.lengthInSamples - region.fadeInSamples);
}

// The start slides along the file, so the audio under it stays put. It stops at the
// file's first sample, at the timeline's start, and a sample short of the end.
inline AudioRegion trimmedStart (const AudioRegion& origin, std::int64_t fileSample)
{
    const auto earliest = std::max<std::int64_t> (0, origin.sourceOffset - origin.timelineStart);
    const auto offset = std::clamp (fileSample, earliest, origin.sourceOffset + origin.lengthInSamples - 1);
    const auto delta = offset - origin.sourceOffset;
    auto region = origin;
    region.sourceOffset = offset;
    region.lengthInSamples -= delta;
    region.timelineStart += delta;
    fitFades (region);
    return region;
}

// The end stops a sample past the start and, when the file's length is known
// (`fileFrames` above zero), at the file's last sample.
inline AudioRegion trimmedEnd (const AudioRegion& origin, std::int64_t fileSample, std::int64_t fileFrames)
{
    auto end = std::max (fileSample, origin.sourceOffset + 1);
    if (fileFrames > 0)
        end = std::min (end, std::max (fileFrames, origin.sourceOffset + 1));
    auto region = origin;
    region.lengthInSamples = end - origin.sourceOffset;
    fitFades (region);
    return region;
}
} // namespace duskstudio::imgui::trim
