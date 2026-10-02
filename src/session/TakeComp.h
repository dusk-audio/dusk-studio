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

// The takes whose audio covers all of [start, end), newest first as the lanes show
// them. Empty for an empty span.
std::vector<TakeId> takesCovering (const Track& track, std::int64_t start, std::int64_t end);

// The take `step` lanes below `current` among `covering` (above for a negative step),
// or 0 past either end. A current take not among them stands just above the first
// lane, so stepping down reaches the newest and stepping up the oldest.
TakeId steppedTake (const std::vector<TakeId>& covering, TakeId current, int step);

// The comp section at `at`: the span of the region playing there, the later-starting
// one inside a crossfade. Between regions it is the gap from the end of the one
// before to the start of the one after, open at either end past the first or last.
// What a click on a take lane puts that take over.
std::pair<std::int64_t, std::int64_t> compSectionAt (const Track& track, std::int64_t at);

// Two regions of the comp that meet where one take gives way to the next: both name
// a take, and the left one runs on past where the right one starts by no more than a
// seam fade. Indices into the track's regions.
struct CompSeam
{
    int left = -1;
    int right = -1;
};

// The seam whose crossfade lies within `tolerance` samples of `at`, the nearest when
// several do.
std::optional<CompSeam> compSeamNear (const Track& track, std::int64_t at, std::int64_t tolerance);

// `delta` cut to what both regions can give: neither reads past its take, and each
// keeps its fades and a sample besides.
std::int64_t clampSeamShift (const Track& track, CompSeam seam, std::int64_t delta);

// Moves the seam by `delta` samples: the left region's end and the right region's
// start move together, and the crossfade between them keeps its length and shape.
void shiftSeam (Track& track, CompSeam seam, std::int64_t delta);
} // namespace duskstudio
