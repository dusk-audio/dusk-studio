#pragma once

#include <juce_core/juce_core.h>
#include "Session.h"

namespace duskstudio::snap
{
    // Beat step in samples when bpm > 0. Returns 0 otherwise. Use this
    // when callers want to *skip* snapping entirely if there's no tempo
    // (e.g. snap-to-bar/beat for a brand-new region that has no prior
    // origin to drift from).
    inline juce::int64 beatSamples (double sampleRate, float bpm) noexcept
    {
        if (sampleRate <= 0.0 || bpm <= 0.0f) return 0;
        return (juce::int64) (sampleRate * 60.0 / (double) bpm);
    }

    // Beat step in samples when bpm > 0, otherwise 1 second. Returns 0
    // only when the sample rate itself is invalid. Use this for delta
    // drags where the user expects *some* snapping even without a
    // tempo (the existing TapeStrip drag/marker pattern).
    inline juce::int64 beatOrSecondSamples (double sampleRate, float bpm) noexcept
    {
        if (sampleRate <= 0.0) return 0;
        return (bpm > 0.0f)
            ? (juce::int64) (sampleRate * 60.0 / (double) bpm)
            : (juce::int64) sampleRate;
    }

    // Round a signed delta to the nearest multiple of `step`. Returns the
    // input unchanged when step <= 0. The half-step bias is sign-aware so
    // the delta is rounded toward zero by less than half a step in either
    // direction.
    inline juce::int64 snapDelta (juce::int64 delta, juce::int64 step) noexcept
    {
        if (step <= 0) return delta;
        return ((delta + (delta >= 0 ? step / 2 : -step / 2)) / step) * step;
    }

    // Round a non-negative absolute sample position to the nearest multiple
    // of `step`. Negative input or step <= 0 returns the input unchanged.
    inline juce::int64 snapAbsolute (juce::int64 sample, juce::int64 step) noexcept
    {
        if (step <= 0 || sample < 0) return sample;
        return ((sample + step / 2) / step) * step;
    }

    // Step in samples for a SnapResolution at the current tempo + time-sig.
    // Musical resolutions multiply the quarter-note step (sr * 60 / bpm) by
    // a fraction (eighth = 0.5, sixteenth = 0.25, etc.). Triplets are ×2/3,
    // dotted are ×3/2. Bar uses the active beatsPerBar so 3/4 vs 4/4 give
    // different bar widths. Timecode / MinSec / CD-Frames are sample-rate
    // multiples that don't depend on tempo. Returns 0 when sr <= 0 or
    // when a tempo-dependent step is requested with bpm <= 0 (caller
    // sees this as "snap disabled" since snapDelta/snapAbsolute no-op
    // on step <= 0).
    inline juce::int64 stepForResolution (SnapResolution r, double sampleRate,
                                           float bpm, int beatsPerBar) noexcept
    {
        if (sampleRate <= 0.0) return 0;
        const auto musical = [&] (float quartersPerStep) -> juce::int64
        {
            if (bpm <= 0.0f) return 0;
            const double quarterSamples = sampleRate * 60.0 / (double) bpm;
            return (juce::int64) (quarterSamples * (double) quartersPerStep);
        };
        switch (r)
        {
            case SnapResolution::Bar:             return musical ((float) juce::jmax (1, beatsPerBar));
            case SnapResolution::Half:            return musical (2.0f);
            case SnapResolution::Quarter:         return musical (1.0f);
            case SnapResolution::Eighth:          return musical (0.5f);
            case SnapResolution::Sixteenth:       return musical (0.25f);
            case SnapResolution::ThirtySecond:    return musical (0.125f);
            case SnapResolution::SixtyFourth:     return musical (0.0625f);
            case SnapResolution::OneTwentyEighth: return musical (0.03125f);
            case SnapResolution::HalfTriplet:     return musical (2.0f      * 2.0f / 3.0f);
            case SnapResolution::QuarterTriplet:  return musical (1.0f      * 2.0f / 3.0f);
            case SnapResolution::EighthTriplet:   return musical (0.5f      * 2.0f / 3.0f);
            case SnapResolution::SixteenthTriplet: return musical (0.25f     * 2.0f / 3.0f);
            case SnapResolution::ThirtySecondTrip: return musical (0.125f    * 2.0f / 3.0f);
            case SnapResolution::HalfDotted:      return musical (2.0f      * 1.5f);
            case SnapResolution::QuarterDotted:   return musical (1.5f);
            case SnapResolution::EighthDotted:    return musical (0.75f);
            case SnapResolution::SixteenthDotted: return musical (0.375f);
            case SnapResolution::Timecode:        return (juce::int64) (sampleRate / 25.0);
            case SnapResolution::MinSec:          return (juce::int64) sampleRate;
            case SnapResolution::CDFrames:        return (juce::int64) (sampleRate / 75.0);
        }
        return 0;
    }

    // Convenience: gate the delta-snap on session.snapToGrid using the
    // session's active SnapResolution. Falls back to a 1-second step when
    // the resolution is musical but no tempo is set, preserving the prior
    // "snap to something" guarantee callers relied on.
    inline juce::int64 snapDeltaToGrid (juce::int64 delta, const Session& s,
                                        double sampleRate) noexcept
    {
        if (! s.snapToGrid) return delta;
        const auto bpm = s.tempoBpm.load (std::memory_order_relaxed);
        const auto bpb = s.beatsPerBar.load (std::memory_order_relaxed);
        auto step = stepForResolution (s.snapResolution, sampleRate, bpm, bpb);
        if (step <= 0 && sampleRate > 0.0) step = (juce::int64) sampleRate;  // 1-second fallback
        return snapDelta (delta, step);
    }

    // Grid step in TICKS for a musical SnapResolution (tempo-independent: a
    // quarter is always kMidiTicksPerQuarter ticks). Returns 0 for the
    // sample-based resolutions (Timecode / MinSec / CDFrames), which are snapped
    // directly in samples instead.
    inline juce::int64 musicalTickStep (SnapResolution r, int beatsPerBar) noexcept
    {
        const double q = (double) kMidiTicksPerQuarter;
        const auto m = [&] (double quarters) -> juce::int64
            { return (juce::int64) std::llround (q * quarters); };
        switch (r)
        {
            case SnapResolution::Bar:              return m ((double) juce::jmax (1, beatsPerBar));
            case SnapResolution::Half:             return m (2.0);
            case SnapResolution::Quarter:          return m (1.0);
            case SnapResolution::Eighth:           return m (0.5);
            case SnapResolution::Sixteenth:        return m (0.25);
            case SnapResolution::ThirtySecond:     return m (0.125);
            case SnapResolution::SixtyFourth:      return m (0.0625);
            case SnapResolution::OneTwentyEighth:  return m (0.03125);
            case SnapResolution::HalfTriplet:      return m (2.0   * 2.0 / 3.0);
            case SnapResolution::QuarterTriplet:   return m (1.0   * 2.0 / 3.0);
            case SnapResolution::EighthTriplet:    return m (0.5   * 2.0 / 3.0);
            case SnapResolution::SixteenthTriplet: return m (0.25  * 2.0 / 3.0);
            case SnapResolution::ThirtySecondTrip: return m (0.125 * 2.0 / 3.0);
            case SnapResolution::HalfDotted:       return m (2.0   * 1.5);
            case SnapResolution::QuarterDotted:    return m (1.5);
            case SnapResolution::EighthDotted:     return m (0.75);
            case SnapResolution::SixteenthDotted:  return m (0.375);
            case SnapResolution::Timecode:
            case SnapResolution::MinSec:
            case SnapResolution::CDFrames:         return 0;
        }
        return 0;
    }

    // Convenience: gate the absolute-snap on session.snapToGrid using the
    // session's active SnapResolution. Skips snapping entirely when no valid
    // step is available (matches prior new-region behaviour). Musical
    // resolutions snap in tick space through the session tempo map, so the grid
    // follows tempo changes; the constant-tempo case is unchanged.
    inline juce::int64 snapAbsoluteToGrid (juce::int64 sample, const Session& s,
                                           double sampleRate) noexcept
    {
        if (! s.snapToGrid || sampleRate <= 0.0) return sample;
        const auto bpb = s.beatsPerBar.load (std::memory_order_relaxed);
        if (const auto tickStep = musicalTickStep (s.snapResolution, bpb); tickStep > 0)
        {
            const auto tick = s.samplesToTicks (sample, sampleRate);
            return s.ticksToSamples (snapAbsolute (tick, tickStep), sampleRate);
        }
        // Sample-based resolutions (timecode / minsec / cd-frames) are
        // tempo-independent absolute-time grids.
        const auto bpm = s.tempoBpm.load (std::memory_order_relaxed);
        return snapAbsolute (sample,
                              stepForResolution (s.snapResolution, sampleRate, bpm, bpb));
    }
}
