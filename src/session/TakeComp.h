#pragma once

#include "Session.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace duskstudio
{
// Seam crossfade where a take range meets the regions it cuts into, capped at
// half the new range so its two ramps never overlap.
constexpr std::int64_t kPunchFadeSamples = 64;

// Clears [start, end) for a new region: regions inside it go, a region that
// spans it splits in two, and one that overlaps an edge is trimmed. What is
// left reaches the seam fade into the range, or as far as it already went if
// that is less, and ramps out there with a raised cosine, so it crossfades
// against whatever lands there.
void carveRegions (std::vector<AudioRegion>& regions, std::int64_t start, std::int64_t end);

// The part of the take inside [start, end), placed where the take plays it.
std::optional<AudioRegion> regionFromTake (const AudioTake& take, std::int64_t start, std::int64_t end);

// Replaces what the track plays over [start, end) with the take's audio there.
void promoteTakeRange (Track& track, const AudioTake& take, std::int64_t start, std::int64_t end);

// The parts of the take the track plays, placed where the take lies on the
// timeline, sorted, with touching ranges merged. A region counts when it names
// the take and reads the take's file; a moved region counts at the part of the
// take its audio comes from, not where it now plays.
std::vector<std::pair<std::int64_t, std::int64_t>> takeCoverage (const Track& track, TakeId id);

const AudioTake* findTake (const Track& track, TakeId id);
} // namespace duskstudio
