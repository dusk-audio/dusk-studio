#pragma once

#include "Session.h"

#include <cstdint>
#include <optional>
#include <string>
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
//
// First, every region there that names no take (an imported file, a reversed
// or rendered region, a recording made before takes lived on the track)
// becomes a take of its own on the track, read from the region as it stands,
// with an id from the session. The region and whatever the carve leaves of it
// then name that take, so no audio the carve cuts away is left out of every
// take. Several are made in timeline order. The track's takes stay in
// recording order: a made take whose region has a capture time goes before the
// first take captured after it, a shared capture time ordered by loop pass;
// one without a capture time goes last.
namespace detail
{
void carveRegions (Session& session, Track& track, std::int64_t start, std::int64_t end);
} // namespace detail

// The part of the take inside [start, end), placed where the take plays it.
std::optional<AudioRegion> regionFromTake (const AudioTake& take, std::int64_t start, std::int64_t end);

// Replaces what the track plays over [start, end) with the take's audio there,
// carving as carveRegions does. A take made of a region without a capture time
// goes just before the take being placed, since its audio was on the track
// first, or last when that take is not on the track yet. `take` may be one of
// the track's own takes.
void promoteTakeRange (Session& session, Track& track, const AudioTake& take,
                       std::int64_t start, std::int64_t end);

// "Take N" for a new take on the track, N one past the highest number among
// its takes still named exactly "Take N". Renamed takes do not count.
std::string nextTakeName (const Track& track);

// The parts of the take the track plays, placed where the take lies on the
// timeline, sorted, with touching ranges merged. A region counts when it names
// the take and reads the take's file; a moved region counts at the part of the
// take its audio comes from, not where it now plays.
std::vector<std::pair<std::int64_t, std::int64_t>> takeCoverage (const Track& track, TakeId id);

const AudioTake* findTake (const Track& track, TakeId id);
} // namespace duskstudio
