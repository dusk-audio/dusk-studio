#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/RegionTrim.h"

using namespace duskstudio;
using namespace duskstudio::imgui::trim;

namespace
{
// File samples [12000, 36000) of a 48000-sample file, played from 60000.
AudioRegion trimmedRegion()
{
    AudioRegion region;
    region.timelineStart = 60000;
    region.sourceOffset = 12000;
    region.lengthInSamples = 24000;
    region.fadeInSamples = 1000;
    region.fadeOutSamples = 2000;
    return region;
}
} // namespace

TEST_CASE ("A trim handle goes back out past the region it started from", "[editor][trim]")
{
    const auto origin = trimmedRegion();

    SECTION ("the start reveals the audio before it")
    {
        const auto region = trimmedStart (origin, 4000);
        CHECK (region.sourceOffset == 4000);
        CHECK (region.timelineStart == 52000);
        CHECK (region.lengthInSamples == 32000);
        CHECK (region.timelineStart - region.sourceOffset == origin.timelineStart - origin.sourceOffset);
    }
    SECTION ("the end reveals the audio after it, up to the file's end")
    {
        CHECK (trimmedEnd (origin, 44000, 48000).lengthInSamples == 32000);
        CHECK (trimmedEnd (origin, 90000, 48000).lengthInSamples == 36000);
        CHECK (trimmedEnd (origin, 90000, 0).lengthInSamples == 78000);
    }
}

TEST_CASE ("A trim handle stops at the file, the timeline and a sample of audio", "[editor][trim]")
{
    auto origin = trimmedRegion();

    CHECK (trimmedStart (origin, -500).sourceOffset == 0);
    CHECK (trimmedStart (origin, 1'000'000).lengthInSamples == 1);
    CHECK (trimmedEnd (origin, 0, 48000).lengthInSamples == 1);

    // Near the timeline's start, the start stops there before the file's does.
    origin.timelineStart = 5000;
    const auto region = trimmedStart (origin, 0);
    CHECK (region.timelineStart == 0);
    CHECK (region.sourceOffset == 7000);

    // Fades shrink with the region and come back as it grows within one drag.
    const auto squeezed = trimmedEnd (trimmedRegion(), 12500, 48000);
    CHECK (squeezed.fadeInSamples + squeezed.fadeOutSamples <= squeezed.lengthInSamples);
    const auto restored = trimmedEnd (trimmedRegion(), 36000, 48000);
    CHECK (restored.fadeInSamples == 1000);
    CHECK (restored.fadeOutSamples == 2000);
}
