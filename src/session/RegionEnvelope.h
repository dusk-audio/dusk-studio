#pragma once

#include "Session.h"

#include <algorithm>
#include <cstdint>

namespace duskstudio
{
// The gain a region's fades and its overlap with its neighbours give each of
// its samples, shared by playback and the join render so a joined file holds
// what its regions played. Playback reads it on the audio thread, so
// everything here stays inline and allocation-free.

// Fades longer than the region would multiply into a notch mid-region; they
// shrink in proportion so the ramps meet at one sample.
inline void fitFadesToLength (std::int64_t& fadeIn, std::int64_t& fadeOut, std::int64_t length) noexcept
{
    fadeIn  = std::max ((std::int64_t) 0, fadeIn);
    fadeOut = std::max ((std::int64_t) 0, fadeOut);
    const auto total = fadeIn + fadeOut;
    if (total > length && total > 0)
    {
        fadeIn  = (fadeIn * length) / total;
        fadeOut = length - fadeIn;
    }
}

// How far a region runs into the next one by timeline start; 0 when they do
// not overlap. Only neighbours in start order crossfade.
inline std::int64_t overlapWithNext (std::int64_t start, std::int64_t length,
                                     std::int64_t nextStart, std::int64_t nextLength) noexcept
{
    const auto end = start + length;
    if (end <= nextStart) return 0;
    return std::min (end - nextStart, std::min (length, nextLength));
}

struct RegionEnvelope
{
    std::int64_t fadeIn  = 0;
    std::int64_t fadeOut = 0;
    FadeShape fadeInShape  = FadeShape::Linear;
    FadeShape fadeOutShape = FadeShape::Linear;

    // sinceStart counts samples from the region's first, untilEnd the samples
    // left to its end, this one included.
    float gainAt (float regionGain, std::int64_t sinceStart, std::int64_t untilEnd) const noexcept
    {
        float gain = regionGain;
        if (fadeIn > 0 && sinceStart < fadeIn)
            gain *= applyFadeShape ((float) sinceStart / (float) fadeIn, fadeInShape);
        if (fadeOut > 0 && untilEnd < fadeOut)
            gain *= applyFadeShape ((float) untilEnd / (float) fadeOut, fadeOutShape);
        return gain;
    }

    // Where neither fade reaches in a run of numSamples whose first sample is
    // sinceStart and untilEnd as gainAt counts them: [first, last), counted
    // from the run's start, in which gainAt gives the region gain untouched.
    // Empty when the fades meet inside the run.
    struct Unfaded
    {
        std::int64_t first = 0;
        std::int64_t last  = 0;
    };

    Unfaded unfaded (std::int64_t sinceStart, std::int64_t untilEnd, std::int64_t numSamples) const noexcept
    {
        Unfaded run;
        run.first = std::clamp (fadeIn - sinceStart, (std::int64_t) 0, numSamples);
        run.last  = std::clamp (untilEnd - fadeOut + 1, run.first, numSamples);
        return run;
    }
};

// Each end fades over the longer of the region's own fade and its overlap with
// the neighbour there. The region's shape holds when its own fade is the
// longer; an overlap that outruns it ramps with equal power, so the pair sums
// to constant power.
inline RegionEnvelope regionEnvelope (std::int64_t fadeIn, FadeShape fadeInShape,
                                      std::int64_t fadeOut, FadeShape fadeOutShape,
                                      std::int64_t overlapPrev, std::int64_t overlapNext) noexcept
{
    RegionEnvelope e;
    e.fadeIn       = std::max (fadeIn, overlapPrev);
    e.fadeOut      = std::max (fadeOut, overlapNext);
    e.fadeInShape  = fadeIn >= overlapPrev ? fadeInShape : FadeShape::EqualPower;
    e.fadeOutShape = fadeOut >= overlapNext ? fadeOutShape : FadeShape::EqualPower;
    return e;
}
} // namespace duskstudio
