#include "TakeComp.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
struct CarvedEdges
{
    bool left  = false;
    bool right = false;
};

std::int64_t seamFadeFor (std::int64_t start, std::int64_t end)
{
    return std::max ((std::int64_t) 0, std::min (kPunchFadeSamples, (end - start) / 2));
}

// A carved region keeps its fade on the far side only as far as the new
// seam fade leaves room for, so the two ramps never overlap.
void fitFarFade (std::int64_t& farFade, std::int64_t length, std::int64_t seamFade)
{
    farFade = std::max ((std::int64_t) 0, std::min (farFade, length - seamFade));
}

CarvedEdges carve (std::vector<AudioRegion>& regs, std::int64_t start, std::int64_t end)
{
    CarvedEdges edges;
    if (end <= start) return edges;
    const std::int64_t fade = seamFadeFor (start, end);

    regs.erase (std::remove_if (regs.begin(), regs.end(),
                                [start, end] (const AudioRegion& r)
                                {
                                    return r.timelineStart >= start
                                        && r.timelineStart + r.lengthInSamples <= end;
                                }),
                regs.end());

    std::vector<AudioRegion> rightFragments;
    for (auto& ex : regs)
    {
        const auto exStart = ex.timelineStart;
        const auto exEnd   = ex.timelineStart + ex.lengthInSamples;
        if (exEnd <= start || exStart >= end) continue;

        const bool spansLeft  = exStart < start;
        const bool spansRight = exEnd   > end;

        // A seam reaches one fade into the range, but a region never grows past
        // its own ends to make one: the audio beyond them was never part of it.
        if (spansRight)
        {
            AudioRegion right = ex;
            right.timelineStart   = std::max (exStart, end - fade);
            right.sourceOffset    = ex.sourceOffset + (right.timelineStart - exStart);
            right.lengthInSamples = exEnd - right.timelineStart;
            right.fadeInSamples   = end - right.timelineStart;
            right.fadeInShape     = FadeShape::RaisedCosine;
            fitFarFade (right.fadeOutSamples, right.lengthInSamples, right.fadeInSamples);
            edges.right = true;
            if (! spansLeft)
            {
                ex = std::move (right);
                continue;
            }
            rightFragments.push_back (std::move (right));
        }

        const std::int64_t leftEnd = std::min (exEnd, start + fade);
        ex.lengthInSamples = leftEnd - exStart;
        ex.fadeOutSamples  = leftEnd - start;
        ex.fadeOutShape    = FadeShape::RaisedCosine;
        fitFarFade (ex.fadeInSamples, ex.lengthInSamples, ex.fadeOutSamples);
        edges.left = true;
    }
    for (auto& fragment : rightFragments)
        regs.push_back (std::move (fragment));
    return edges;
}
} // namespace

void carveRegions (std::vector<AudioRegion>& regions, std::int64_t start, std::int64_t end)
{
    carve (regions, start, end);
}

std::optional<AudioRegion> regionFromTake (const AudioTake& take, std::int64_t start, std::int64_t end)
{
    const std::int64_t from = std::max (start, take.timelineStart);
    const std::int64_t to   = std::min (end, take.timelineStart + take.lengthInSamples);
    if (to <= from) return std::nullopt;

    AudioRegion region;
    region.file            = take.file;
    region.timelineStart   = from;
    region.lengthInSamples = to - from;
    region.sourceOffset    = take.sourceOffset + (from - take.timelineStart);
    region.numChannels     = take.numChannels;
    region.provenance      = take.provenance;
    region.takeId          = take.id;
    return region;
}

void promoteTakeRange (Track& track, const AudioTake& take, std::int64_t start, std::int64_t end)
{
    auto region = regionFromTake (take, start, end);
    if (! region) return;

    const std::int64_t from = region->timelineStart;
    const std::int64_t to   = from + region->lengthInSamples;
    const auto edges = carve (track.regions, from, to);
    const std::int64_t fade = seamFadeFor (from, to);
    if (edges.left)
    {
        region->fadeInSamples = fade;
        region->fadeInShape   = FadeShape::RaisedCosine;
    }
    if (edges.right)
    {
        region->fadeOutSamples = fade;
        region->fadeOutShape   = FadeShape::RaisedCosine;
    }
    track.regions.push_back (std::move (*region));
}

std::vector<std::pair<std::int64_t, std::int64_t>> takeCoverage (const Track& track, TakeId id)
{
    std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    const auto* take = findTake (track, id);
    if (take == nullptr) return spans;
    const std::int64_t takeEnd = take->timelineStart + take->lengthInSamples;
    for (const auto& r : track.regions)
    {
        if (r.takeId != id || r.file != take->file) continue;
        const std::int64_t at   = take->timelineStart + (r.sourceOffset - take->sourceOffset);
        const std::int64_t from = std::max (at, take->timelineStart);
        const std::int64_t to   = std::min (at + r.lengthInSamples, takeEnd);
        if (to > from) spans.emplace_back (from, to);
    }
    std::sort (spans.begin(), spans.end());

    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for (const auto& span : spans)
    {
        if (! merged.empty() && span.first <= merged.back().second)
            merged.back().second = std::max (merged.back().second, span.second);
        else
            merged.push_back (span);
    }
    return merged;
}

const AudioTake* findTake (const Track& track, TakeId id)
{
    if (id == 0) return nullptr;
    for (const auto& take : track.takes)
        if (take.id == id) return &take;
    return nullptr;
}
} // namespace duskstudio
