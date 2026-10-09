#pragma once

#include "Session.h"
#include "../foundation/PlanarBuffer.h"

#include <cstdint>
#include <vector>

namespace duskstudio
{
// Both take the regions in the order playback sums them: by timeline start,
// equal starts in track order.

// True when the regions play one unbroken stretch of one file: each either
// carries on where the one before stops, or overlaps or meets what came before
// while reading the file on the same alignment (sourceOffset - timelineStart),
// as a comp section switched back to its own take does. Such regions join by
// lengthening the first, with nothing rendered.
bool regionsPlayOneFileStretch (const std::vector<AudioRegion>& regions);

struct JoinedFades
{
    std::int64_t fadeInSamples  = 0;
    std::int64_t fadeOutSamples = 0;
    FadeShape fadeInShape  = FadeShape::Linear;
    FadeShape fadeOutShape = FadeShape::Linear;
    bool fadeInAuto  = false;
    bool fadeOutAuto = false;
};

// Sums the regions into `mix` as playback plays them: each at its gain, muted
// ones silent, every fade and every overlap between neighbours shaped as
// PlaybackEngine shapes it. `mix` is sized by the caller to the joined span,
// whose first sample is the first region's start.
//
// The fade-in of the first region and the fade-out of the latest-ending one
// are left out of the mix and returned in `outer` for the joined region to
// carry, so they stay editable and are not applied twice. An outer fade that
// another region sounds under, or that an overlap lengthened, cannot be handed
// on that way: it goes into the mix and comes back as no fade.
//
// False when a region's audio cannot be read in full.
bool mixRegionsAsPlayed (const std::vector<AudioRegion>& regions,
                         dusk::audio::PlanarBuffer& mix, JoinedFades& outer);

// The channel count a join renders at: stereo when any region's file is.
// Playback reads each file by its own channel count, a mono one into both
// channels, and a mono track plays the left one, so a render this wide plays
// as its regions did on either kind of track, and keeps a stereo region's
// right channel for when the track turns stereo.
int channelsAsPlayed (const std::vector<AudioRegion>& regions);
} // namespace duskstudio
