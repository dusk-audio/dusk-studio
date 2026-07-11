#pragma once

#include "McuFaderTaper.h"

#include <algorithm>
#include <cmath>

namespace duskstudio
{
// Frac <-> dB mapping shared by the fader-dB MIDI-binding targets: TrackFader,
// BusFader, AuxLaneFader, MasterFader (and TrackFaderBank, which resolves to
// TrackFader before apply).
//
// CC / Note sources are generic 7-bit knobs with no printed scale, so they map
// linearly across -90..+12 dB. PitchBend sources are MCU-style motor faders
// whose silkscreened scale follows the Mackie taper (the same curve
// McuController / McuReceiver ride), so a PB-bound fader maps through it: 0 dB
// lands at ~3/4 of throw, matching the print, not the ~89% a linear-in-dB map
// produced. Bottom of a PB throw is -100 dB, below
// ChannelStripParams::kFaderInfThreshDb (-90), so a zeroed motor fader still
// hard-mutes the strip; the linear path bottoms at exactly -90 (also muted).
//
// The two directions must stay exact inverses: apply uses faderBindingFracToDb,
// soft-takeover pickup read-back uses faderBindingDbToFrac. Change one, change
// the other or pickup latches at the wrong position.
inline float faderBindingFracToDb (float frac, bool pitchBend) noexcept
{
    if (pitchBend)
        return mcu::pitchBend14ToFaderDb (
            (int) std::lround (frac * (float) mcu::kPitchBendMaxValue));
    return -90.0f + frac * (12.0f + 90.0f);
}

inline float faderBindingDbToFrac (float db, bool pitchBend) noexcept
{
    const float frac = pitchBend
        ? (float) mcu::faderDbToPitchBend14 (db) / (float) mcu::kPitchBendMaxValue
        : (db + 90.0f) / (12.0f + 90.0f);
    return std::clamp (frac, 0.0f, 1.0f);
}

} // namespace duskstudio
