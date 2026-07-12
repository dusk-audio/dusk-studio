#pragma once

#include "McuProtocol.h"   // kPitchBendMaxValue

#include <array>
#include <cstddef>

namespace duskstudio::mcu
{
// Fader taper for MCU-mode motorised faders (Mackie / Logic Control).
//
// Real MCU-compatible surfaces (Tascam Model 12, Mackie MCU / C4, Behringer
// X-Touch) silkscreen a fader scale with 0 dB near three-quarters of throw,
// fine resolution around unity, and a long compressed tail toward -inf. A
// straight linear-in-dB map lands 0 dB at ~89% of throw, so the printed
// scale and the motor position disagree everywhere but the endpoints.
//
// Below unity the breakpoints are sampled from Ardour's Mackie-surface fader
// law (`gain_to_slider_position`, pbd/dB.h: ((6*log2(g)+192)/198)^8), the
// de-facto open-source MCU calibration - it lands 0 dB at position 0.782.
// Above unity, 0..+12 dB maps linearly across the remaining top of throw so
// the surface spans the full ChannelStripParams fader range (kFaderMinDb
// = -100 .. kFaderMaxDb = +12) rather than the +6 dB Logic Control ceiling.
//
// The table is shared by McuController (dB -> pitch-bend, emit) and
// McuReceiver (pitch-bend -> dB, decode) so motor feedback and touch input
// ride one curve. Linear interpolation over shared, strictly-increasing
// breakpoints makes the two directions exact inverses within pitch-bend
// quantisation.
struct FaderTaperPoint { float db; int pb14; };

inline constexpr std::array<FaderTaperPoint, 21> kFaderTaper { {
    { -100.0f,     0 },
    {  -84.0f,   131 },
    {  -72.0f,   303 },
    {  -60.0f,   647 },
    {  -54.0f,   922 },
    {  -48.0f,  1294 },
    {  -42.0f,  1791 },
    {  -36.0f,  2448 },
    {  -30.0f,  3307 },
    {  -24.0f,  4418 },
    {  -18.0f,  5844 },
    {  -15.0f,  6697 },
    {  -12.0f,  7657 },
    {   -9.0f,  8735 },
    {   -6.0f,  9944 },
    {   -3.0f, 11297 },
    {    0.0f, 12808 },
    {    3.0f, 13702 },
    {    6.0f, 14596 },
    {    9.0f, 15489 },
    {   12.0f, 16383 },
} };

// dB -> 14-bit pitch-bend (0..kPitchBendMaxValue). Clamped to the taper's
// dB endpoints.
inline int faderDbToPitchBend14 (float db) noexcept
{
    const auto& t = kFaderTaper;
    if (db <= t.front().db) return t.front().pb14;
    if (db >= t.back().db)  return t.back().pb14;
    for (std::size_t i = 1; i < t.size(); ++i)
    {
        if (db <= t[i].db)
        {
            const float frac = (db - t[i - 1].db) / (t[i].db - t[i - 1].db);
            const float pb   = (float) t[i - 1].pb14
                             + frac * (float) (t[i].pb14 - t[i - 1].pb14);
            return (int) (pb + 0.5f);
        }
    }
    return t.back().pb14;
}

// 14-bit pitch-bend -> dB. Clamped to [0, kPitchBendMaxValue].
inline float pitchBend14ToFaderDb (int pb14) noexcept
{
    const auto& t = kFaderTaper;
    if (pb14 < 0) pb14 = 0;
    if (pb14 > kPitchBendMaxValue) pb14 = kPitchBendMaxValue;
    if (pb14 <= t.front().pb14) return t.front().db;
    if (pb14 >= t.back().pb14)  return t.back().db;
    for (std::size_t i = 1; i < t.size(); ++i)
    {
        if (pb14 <= t[i].pb14)
        {
            const float frac = (float) (pb14 - t[i - 1].pb14)
                             / (float) (t[i].pb14 - t[i - 1].pb14);
            return t[i - 1].db + frac * (t[i].db - t[i - 1].db);
        }
    }
    return t.back().db;
}

} // namespace duskstudio::mcu
