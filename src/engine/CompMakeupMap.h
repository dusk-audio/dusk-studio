#pragma once

#include "../session/Session.h"

#include <algorithm>
#include <atomic>

namespace duskstudio
{
namespace comp
{
// Channel-strip comp makeup is one user-facing dB control that has to land
// on whichever per-mode output param the DSP actually reads, and the modes
// do not share a range. Opto's GAIN is the donor's 0..100 hardware dial -
// 50 is unity, one unit is 0.8 dB, so the span is +/-40 dB (the donor's
// OptoGainMapping.h states the same contract UniversalCompressorDSP
// applies). FET and VCA carry dB straight, over +/-20. A dB figure written
// into the wrong domain lands wildly off, so both the value and its range
// come from the active mode.
//
// applyTrackCompMakeupDb and trackCompMakeupDb are exact inverses, and the
// binding frac pair sitting on them is the shape FaderBindingMap.h uses for
// fader targets: a binding's apply and its soft-takeover read-back must
// stay inverses or pickup latches at the wrong position.
//
// Relative controls (the MCU V-pot) take their base from trackCompMakeupDb
// rather than any stored dB, so a nudge continues from what the strip is
// actually playing no matter who wrote it last - UI knob, MIDI binding, a
// comp-mode switch, or a session saved by an older build.
//
// RT-safe: no allocation, no locks, relaxed stores per the param
// convention. Callable from the audio thread (McuReceiver, the binding
// apply and read-back) and the message thread alike.
constexpr float kOptoGainUnityPct = 50.0f;
constexpr float kOptoGainPctPerDb =  1.25f;   // 1 / 0.8 dB per dial unit
constexpr float kOptoMakeupMaxDb  = 40.0f;    // Opto dial span
constexpr float kOutMakeupMaxDb   = 20.0f;    // FET / VCA output params

struct MakeupDomain
{
    float minDb;
    float maxDb;
};

inline MakeupDomain makeupDomainFor (const ChannelStripParams& strip) noexcept
{
    const bool opto = std::clamp (strip.compMode.load (std::memory_order_relaxed), 0, 2) == 0;
    const float span = opto ? kOptoMakeupMaxDb : kOutMakeupMaxDb;
    return { -span, span };
}

inline float makeupDbToOptoGainPct (float db) noexcept
{
    return std::clamp (kOptoGainUnityPct + db * kOptoGainPctPerDb, 0.0f, 100.0f);
}

inline float optoGainPctToMakeupDb (float pct) noexcept
{
    return (std::clamp (pct, 0.0f, 100.0f) - kOptoGainUnityPct) / kOptoGainPctPerDb;
}

inline void applyTrackCompMakeupDb (ChannelStripParams& strip, float db) noexcept
{
    const float outDb = std::clamp (db, -kOutMakeupMaxDb, kOutMakeupMaxDb);
    switch (std::clamp (strip.compMode.load (std::memory_order_relaxed), 0, 2))
    {
        case 0:
            strip.compOptoGain.store (makeupDbToOptoGainPct (db),
                                      std::memory_order_relaxed);
            break;
        case 1:
            strip.compFetOutput.store (outDb, std::memory_order_relaxed);
            break;
        default:
            strip.compVcaOutput.store (outDb, std::memory_order_relaxed);
            break;
    }
}

inline float trackCompMakeupDb (const ChannelStripParams& strip) noexcept
{
    switch (std::clamp (strip.compMode.load (std::memory_order_relaxed), 0, 2))
    {
        case 0:
            return optoGainPctToMakeupDb (
                strip.compOptoGain.load (std::memory_order_relaxed));
        case 1:
            return std::clamp (strip.compFetOutput.load (std::memory_order_relaxed),
                               -kOutMakeupMaxDb, kOutMakeupMaxDb);
        default:
            return std::clamp (strip.compVcaOutput.load (std::memory_order_relaxed),
                               -kOutMakeupMaxDb, kOutMakeupMaxDb);
    }
}

inline float makeupBindingFracToDb (const ChannelStripParams& strip, float frac) noexcept
{
    const auto domain = makeupDomainFor (strip);
    return domain.minDb + frac * (domain.maxDb - domain.minDb);
}

inline float makeupBindingFrac (const ChannelStripParams& strip) noexcept
{
    const auto domain = makeupDomainFor (strip);
    return std::clamp ((trackCompMakeupDb (strip) - domain.minDb)
                           / (domain.maxDb - domain.minDb),
                       0.0f, 1.0f);
}
} // namespace comp
} // namespace duskstudio
