#include "TakeComp.h"

#include <algorithm>
#include <limits>
#include <string_view>

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

// Past nine digits a name is not a number the recorder handed out.
bool takeNumber (const std::string& name, std::uint64_t& number)
{
    constexpr std::string_view prefix = "Take ";
    if (name.size() <= prefix.size() || name.size() - prefix.size() > 9
        || name.compare (0, prefix.size(), prefix) != 0)
        return false;
    std::uint64_t n = 0;
    for (auto i = prefix.size(); i < name.size(); ++i)
    {
        if (name[i] < '0' || name[i] > '9') return false;
        n = n * 10 + (std::uint64_t) (name[i] - '0');
    }
    number = n;
    return true;
}

bool capturedAfter (const TakeProvenance& a, const TakeProvenance& b)
{
    return a.capturedAtMs > b.capturedAtMs
        || (a.capturedAtMs == b.capturedAtMs && a.loopPassOrdinal > b.loopPassOrdinal);
}

std::size_t adoptedTakeIndex (const Track& track, const TakeProvenance& provenance, TakeId placing)
{
    const auto& takes = track.takes;
    const auto at = provenance.capturedAtMs != 0
        ? std::find_if (takes.begin(), takes.end(), [&provenance] (const AudioTake& take)
          {
              return take.provenance.capturedAtMs != 0 && capturedAfter (take.provenance, provenance);
          })
        : std::find_if (takes.begin(), takes.end(), [placing] (const AudioTake& take)
          {
              return placing != 0 && take.id == placing;
          });
    return (std::size_t) (at - takes.begin());
}

void adoptRegionsNamingNoTake (Session& session, Track& track, std::int64_t start, std::int64_t end,
                               TakeId placing)
{
    if (end <= start) return;
    std::vector<std::size_t> plain;
    for (std::size_t i = 0; i < track.regions.size(); ++i)
    {
        const auto& r = track.regions[i];
        if (r.takeId == 0 && r.lengthInSamples > 0 && r.timelineStart < end
            && r.timelineStart + r.lengthInSamples > start)
            plain.push_back (i);
    }
    std::stable_sort (plain.begin(), plain.end(), [&track] (std::size_t a, std::size_t b)
    {
        return track.regions[a].timelineStart < track.regions[b].timelineStart;
    });

    for (const auto i : plain)
    {
        auto& region = track.regions[i];
        AudioTake take;
        take.id              = session.allocateTakeId();
        take.name            = nextTakeName (track);
        take.file            = region.file;
        take.timelineStart   = region.timelineStart;
        take.lengthInSamples = region.lengthInSamples;
        take.sourceOffset    = region.sourceOffset;
        take.numChannels     = region.numChannels;
        take.provenance      = region.provenance;
        region.takeId = take.id;
        const auto at = adoptedTakeIndex (track, take.provenance, placing);
        track.takes.insert (track.takes.begin() + (std::ptrdiff_t) at, std::move (take));
    }
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

void detail::carveRegions (Session& session, Track& track, std::int64_t start, std::int64_t end)
{
    adoptRegionsNamingNoTake (session, track, start, end, 0);
    carve (track.regions, start, end);
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

void promoteTakeRange (Session& session, Track& track, const AudioTake& take,
                       std::int64_t start, std::int64_t end)
{
    auto region = regionFromTake (take, start, end);
    if (! region) return;

    // Adopting inserts into track.takes, which may hold `take`: nothing reads
    // it past this point.
    const std::int64_t from = region->timelineStart;
    const std::int64_t to   = from + region->lengthInSamples;
    adoptRegionsNamingNoTake (session, track, from, to, region->takeId);
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

std::string nextTakeName (const Track& track)
{
    std::uint64_t highest = 0;
    for (const auto& take : track.takes)
    {
        std::uint64_t number = 0;
        if (takeNumber (take.name, number))
            highest = std::max (highest, number);
    }
    return "Take " + std::to_string (highest + 1);
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

std::vector<TakeId> takesCovering (const Track& track, std::int64_t start, std::int64_t end)
{
    std::vector<TakeId> covering;
    if (end <= start) return covering;
    for (auto it = track.takes.rbegin(); it != track.takes.rend(); ++it)
        if (it->lengthInSamples > 0 && it->timelineStart <= start && it->timelineStart + it->lengthInSamples >= end)
            covering.push_back (it->id);
    return covering;
}

TakeId steppedTake (const std::vector<TakeId>& covering, TakeId current, int step)
{
    const auto count = static_cast<int> (covering.size());
    if (count == 0 || step == 0) return 0;
    const auto found = std::find (covering.begin(), covering.end(), current);
    if (found == covering.end())
        return step > 0 ? covering.front() : covering.back();
    const int next = static_cast<int> (found - covering.begin()) + step;
    return next >= 0 && next < count ? covering[static_cast<std::size_t> (next)] : 0;
}

namespace
{
bool seamBetween (const AudioRegion& left, const AudioRegion& right)
{
    const auto overlap = left.timelineStart + left.lengthInSamples - right.timelineStart;
    return left.takeId != 0 && right.takeId != 0 && left.timelineStart < right.timelineStart
        && overlap >= 0 && overlap <= kPunchFadeSamples;
}

bool validSeam (const Track& track, CompSeam seam)
{
    const auto count = static_cast<int> (track.regions.size());
    return seam.left >= 0 && seam.left < count && seam.right >= 0 && seam.right < count && seam.left != seam.right;
}
} // namespace

std::optional<CompSeam> compSeamNear (const Track& track, std::int64_t at, std::int64_t tolerance)
{
    std::optional<CompSeam> nearest;
    std::int64_t best = std::numeric_limits<std::int64_t>::max();
    const auto& regs = track.regions;
    for (int l = 0; l < static_cast<int> (regs.size()); ++l)
        for (int r = 0; r < static_cast<int> (regs.size()); ++r)
        {
            if (l == r || ! seamBetween (regs[(std::size_t) l], regs[(std::size_t) r])) continue;
            const auto from = regs[(std::size_t) r].timelineStart;
            const auto to = regs[(std::size_t) l].timelineStart + regs[(std::size_t) l].lengthInSamples;
            const auto distance = at < from ? from - at : at > to ? at - to : 0;
            if (distance <= tolerance && distance < best)
            {
                best = distance;
                nearest = CompSeam { l, r };
            }
        }
    return nearest;
}

std::int64_t clampSeamShift (const Track& track, CompSeam seam, std::int64_t delta)
{
    if (! validSeam (track, seam)) return 0;
    const auto& left = track.regions[(std::size_t) seam.left];
    const auto& right = track.regions[(std::size_t) seam.right];
    const auto* leftTake = findTake (track, left.takeId);
    const auto* rightTake = findTake (track, right.takeId);

    // Each region keeps its fades and the crossfade inside it, and a sample besides, so
    // the right region always starts after the left one does.
    const auto overlap = left.timelineStart + left.lengthInSamples - right.timelineStart;
    const auto keep = [overlap] (const AudioRegion& r)
    { return std::max (r.fadeInSamples + r.fadeOutSamples, overlap) + 1; };

    // Later: the left region may read on to the end of its take.
    auto most = right.lengthInSamples - keep (right);
    most = std::min (most, leftTake == nullptr ? 0
                           : leftTake->sourceOffset + leftTake->lengthInSamples - (left.sourceOffset + left.lengthInSamples));
    // Earlier: the right region may read back to the start of its take, and no
    // further than the start of the timeline.
    auto least = keep (left) - left.lengthInSamples;
    least = std::max ({ least, rightTake == nullptr ? 0 : rightTake->sourceOffset - right.sourceOffset,
                        -right.timelineStart });
    if (least > most) return 0;
    return std::clamp (delta, least, most);
}

void shiftSeam (Track& track, CompSeam seam, std::int64_t delta)
{
    delta = clampSeamShift (track, seam, delta);
    if (delta == 0) return;
    auto& left = track.regions[(std::size_t) seam.left];
    auto& right = track.regions[(std::size_t) seam.right];
    left.lengthInSamples += delta;
    right.timelineStart += delta;
    right.sourceOffset += delta;
    right.lengthInSamples -= delta;
}
} // namespace duskstudio
