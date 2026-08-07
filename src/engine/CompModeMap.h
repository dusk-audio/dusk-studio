#pragma once

#include "../session/Session.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace duskstudio
{
namespace comp
{
// A channel strip shows one set of comp controls - THRESHOLD, RATIO,
// ATTACK, RELEASE, MAKEUP - over three modes that share neither the
// parameter behind a control nor its range. Everything that has to land on
// "whatever the active mode reads" routes through here, so the MCU
// encoders, the MIDI bindings, the comp editor and the GR-meter handle
// cannot drift apart: a value written into the wrong mode's domain is
// either inaudible - the param that mode never reads - or wildly off.
//
// Every range below is the donor's, taken from UniversalCompressorDSP.hpp's
// setter contracts, not from what some knob happens to sweep.
//
// applyTrackComp*Db / trackComp*Db are exact inverses, and the binding frac
// pairs sitting on them are the shape FaderBindingMap.h uses for fader
// targets: a binding's apply and its soft-takeover read-back must stay
// inverses or pickup latches at the wrong position.
//
// Relative controls (the MCU V-pots) take their base from the read-back
// rather than any stored value of their own, so a nudge continues from what
// the strip is actually playing no matter who wrote it last - UI knob, MIDI
// binding, a comp-mode switch, or a session saved by an older build.
//
// Every public entry point loads compMode exactly once and carries it
// through the detail layer. The mode is written from the message thread
// while these run on the audio thread, so a second load could pick up a
// different mode - clamping against one domain and storing into another
// puts an out-of-range value in front of the DSP.
//
// RT-safe: no allocation, no locks, relaxed loads and stores per the param
// convention. Callable from the audio thread (McuReceiver, the binding
// apply and read-back) and the message thread alike.

// Makeup. Opto's GAIN is the donor's 0..100 hardware dial - 50 is unity,
// one unit is 0.8 dB, so the span is +/-40 dB. FET and VCA carry dB
// straight over their -20..20 output params.
constexpr float kOptoGainUnityPct = 50.0f;
constexpr float kOptoGainPctPerDb =  1.25f;   // 1 / 0.8 dB per dial unit
constexpr float kOptoMakeupMaxDb  = 40.0f;    // Opto dial span
constexpr float kOutMakeupMaxDb   = 20.0f;    // FET / VCA output params

// Threshold. The donor gives Opto no threshold at all - the optical cell's
// PEAK REDUCTION dial is the control a threshold moves, and it runs the
// other way, so Dusk's convention (shared with the GR-meter handle) is that
// 0 dB is no reduction and -60 dB is the dial hard over. FET and VCA carry
// dB straight over the donor's own -60..0 dBFS and -38..+12 dB.
constexpr float kOptoPeakRedPctPerDb = 100.0f / 60.0f;
constexpr float kOptoThreshMinDb = -60.0f, kOptoThreshMaxDb =  0.0f;
constexpr float kFetThreshMinDb  = -60.0f, kFetThreshMaxDb  =  0.0f;
constexpr float kVcaThreshMinDb  = -38.0f, kVcaThreshMaxDb  = 12.0f;

// Ratio and timing. Opto has none of the three: the optical curve is its
// own ratio and the photocell's program-dependent lag is its attack and
// release, so the donor exposes no parameter for any of them and the comp
// editor hides those knobs in Opto. Controls that fan out through here
// no-op in that mode rather than writing a param it never reads.
constexpr float kFetAttackMinMs   =  0.02f, kFetAttackMaxMs   =   80.0f;
constexpr float kVcaAttackMinMs   =  0.1f,  kVcaAttackMaxMs   =   50.0f;
constexpr float kFetReleaseMinMs  = 50.0f,  kFetReleaseMaxMs  = 1100.0f;
constexpr float kVcaReleaseMinMs  = 10.0f,  kVcaReleaseMaxMs  = 5000.0f;
constexpr float kVcaRatioMin      =  1.0f,  kVcaRatioMax      =  120.0f;
constexpr int   kFetRatioMaxIndex = 4;      // 0=4:1 1=8:1 2=12:1 3=20:1 4=All
// FET's ratio is a switch, so a continuous knob needs a sweep to pick rungs
// from rather than a parameter range; 1..20 covers the printed markings.
constexpr float kFetRatioKnobMin  =  1.0f,  kFetRatioKnobMax  =   20.0f;

// Push-to-reset lands on the mode's own default, so a reset in FET does not
// leave the strip holding VCA's timings. These mirror ChannelStripParams'
// initialisers - comp_mode_fanout.cpp pins them against a fresh strip.
constexpr int   kFetRatioDefaultIndex  =   0;
constexpr float kFetAttackDefaultMs    =   0.2f;
constexpr float kFetReleaseDefaultMs   = 400.0f;
constexpr float kVcaRatioDefault       =   4.0f;
constexpr float kVcaAttackDefaultMs    =   1.0f;
constexpr float kVcaReleaseDefaultMs   = 100.0f;

struct Domain
{
    float lo;
    float hi;
};

inline int compModeOf (const ChannelStripParams& strip) noexcept
{
    return std::clamp (strip.compMode.load (std::memory_order_relaxed), 0, 2);
}

inline bool compModeHasTimingControls (const ChannelStripParams& strip) noexcept
{
    return compModeOf (strip) != 0;
}

inline float fracToValue (Domain d, float frac) noexcept
{
    return d.lo + frac * (d.hi - d.lo);
}

inline float valueToFrac (Domain d, float value) noexcept
{
    return d.hi > d.lo ? std::clamp ((value - d.lo) / (d.hi - d.lo), 0.0f, 1.0f)
                       : 0.0f;
}

// Mode-taking cores. The public wrappers below are the only callers that
// touch the compMode atom; everything here works from the mode they read.
namespace detail
{
inline Domain makeupDomain (int mode) noexcept
{
    const float span = mode == 0 ? kOptoMakeupMaxDb : kOutMakeupMaxDb;
    return { -span, span };
}

inline Domain thresholdDomain (int mode) noexcept
{
    switch (mode)
    {
        case 0:  return { kOptoThreshMinDb, kOptoThreshMaxDb };
        case 1:  return { kFetThreshMinDb,  kFetThreshMaxDb  };
        default: return { kVcaThreshMinDb,  kVcaThreshMaxDb  };
    }
}

inline Domain attackDomain (int mode) noexcept
{
    switch (mode)
    {
        case 1:  return { kFetAttackMinMs, kFetAttackMaxMs };
        case 2:  return { kVcaAttackMinMs, kVcaAttackMaxMs };
        default: return { 0.0f, 0.0f };
    }
}

inline Domain releaseDomain (int mode) noexcept
{
    switch (mode)
    {
        case 1:  return { kFetReleaseMinMs, kFetReleaseMaxMs };
        case 2:  return { kVcaReleaseMinMs, kVcaReleaseMaxMs };
        default: return { 0.0f, 0.0f };
    }
}

inline Domain ratioKnobDomain (int mode) noexcept
{
    switch (mode)
    {
        case 1:  return { kFetRatioKnobMin, kFetRatioKnobMax };
        case 2:  return { kVcaRatioMin,     kVcaRatioMax     };
        default: return { 0.0f, 0.0f };
    }
}

inline float makeupDb (const ChannelStripParams& strip, int mode) noexcept
{
    switch (mode)
    {
        case 0:
            return (std::clamp (strip.compOptoGain.load (std::memory_order_relaxed),
                                0.0f, 100.0f)
                        - kOptoGainUnityPct) / kOptoGainPctPerDb;
        case 1:
            return std::clamp (strip.compFetOutput.load (std::memory_order_relaxed),
                               -kOutMakeupMaxDb, kOutMakeupMaxDb);
        default:
            return std::clamp (strip.compVcaOutput.load (std::memory_order_relaxed),
                               -kOutMakeupMaxDb, kOutMakeupMaxDb);
    }
}

inline void storeMakeupDb (ChannelStripParams& strip, int mode, float db) noexcept
{
    const float outDb = std::clamp (db, -kOutMakeupMaxDb, kOutMakeupMaxDb);
    switch (mode)
    {
        case 0:
            strip.compOptoGain.store (
                std::clamp (kOptoGainUnityPct + db * kOptoGainPctPerDb, 0.0f, 100.0f),
                std::memory_order_relaxed);
            break;
        case 1:  strip.compFetOutput.store (outDb, std::memory_order_relaxed); break;
        default: strip.compVcaOutput.store (outDb, std::memory_order_relaxed); break;
    }
}

inline float thresholdDb (const ChannelStripParams& strip, int mode) noexcept
{
    const auto domain = thresholdDomain (mode);
    switch (mode)
    {
        case 0:
            return std::clamp (-std::clamp (strip.compOptoPeakRed.load (std::memory_order_relaxed),
                                            0.0f, 100.0f) / kOptoPeakRedPctPerDb,
                               domain.lo, domain.hi);
        case 1:
            return std::clamp (strip.compFetThresholdDb.load (std::memory_order_relaxed),
                               domain.lo, domain.hi);
        default:
            return std::clamp (strip.compVcaThreshDb.load (std::memory_order_relaxed),
                               domain.lo, domain.hi);
    }
}

inline void storeThresholdDb (ChannelStripParams& strip, int mode, float db) noexcept
{
    const auto domain = thresholdDomain (mode);
    const float v = std::clamp (db, domain.lo, domain.hi);
    switch (mode)
    {
        case 0:
            strip.compOptoPeakRed.store (std::clamp (-v * kOptoPeakRedPctPerDb, 0.0f, 100.0f),
                                         std::memory_order_relaxed);
            break;
        case 1:  strip.compFetThresholdDb.store (v, std::memory_order_relaxed); break;
        default: strip.compVcaThreshDb.store (v, std::memory_order_relaxed); break;
    }
}
} // namespace detail

//==============================================================================
// Makeup
//==============================================================================
inline Domain makeupDomainFor (const ChannelStripParams& strip) noexcept
{
    return detail::makeupDomain (compModeOf (strip));
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
    detail::storeMakeupDb (strip, compModeOf (strip), db);
}

inline float trackCompMakeupDb (const ChannelStripParams& strip) noexcept
{
    return detail::makeupDb (strip, compModeOf (strip));
}

inline float makeupBindingFracToDb (const ChannelStripParams& strip, float frac) noexcept
{
    return fracToValue (makeupDomainFor (strip), frac);
}

inline float makeupBindingFrac (const ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    return valueToFrac (detail::makeupDomain (mode), detail::makeupDb (strip, mode));
}

//==============================================================================
// Threshold
//==============================================================================
inline Domain thresholdDomainFor (const ChannelStripParams& strip) noexcept
{
    return detail::thresholdDomain (compModeOf (strip));
}

inline float thresholdDbToOptoPeakRedPct (float db) noexcept
{
    return std::clamp (-db * kOptoPeakRedPctPerDb, 0.0f, 100.0f);
}

inline float optoPeakRedPctToThresholdDb (float pct) noexcept
{
    return -std::clamp (pct, 0.0f, 100.0f) / kOptoPeakRedPctPerDb;
}

inline void applyTrackCompThresholdDb (ChannelStripParams& strip, float db) noexcept
{
    detail::storeThresholdDb (strip, compModeOf (strip), db);
}

inline float trackCompThresholdDb (const ChannelStripParams& strip) noexcept
{
    return detail::thresholdDb (strip, compModeOf (strip));
}

// Neutral is the top of the domain, not zero: VCA's ceiling is +12 dB and
// 0 dB there still compresses everything above 0 dBFS.
inline void resetTrackCompThreshold (ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    detail::storeThresholdDb (strip, mode, detail::thresholdDomain (mode).hi);
}

inline float thresholdBindingFracToDb (const ChannelStripParams& strip, float frac) noexcept
{
    return fracToValue (thresholdDomainFor (strip), frac);
}

inline float thresholdBindingFrac (const ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    return valueToFrac (detail::thresholdDomain (mode), detail::thresholdDb (strip, mode));
}

//==============================================================================
// Attack / release
//==============================================================================
inline Domain attackDomainFor (const ChannelStripParams& strip) noexcept
{
    return detail::attackDomain (compModeOf (strip));
}

inline Domain releaseDomainFor (const ChannelStripParams& strip) noexcept
{
    return detail::releaseDomain (compModeOf (strip));
}

inline void applyTrackCompAttackMs (ChannelStripParams& strip, float ms) noexcept
{
    const int mode = compModeOf (strip);
    const auto domain = detail::attackDomain (mode);
    switch (mode)
    {
        case 1:
            strip.compFetAttack.store (std::clamp (ms, domain.lo, domain.hi),
                                       std::memory_order_relaxed);
            break;
        case 2:
            strip.compVcaAttack.store (std::clamp (ms, domain.lo, domain.hi),
                                       std::memory_order_relaxed);
            break;
        default: break;
    }
}

inline float trackCompAttackMs (const ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    const auto domain = detail::attackDomain (mode);
    switch (mode)
    {
        case 1:  return std::clamp (strip.compFetAttack.load (std::memory_order_relaxed),
                                    domain.lo, domain.hi);
        case 2:  return std::clamp (strip.compVcaAttack.load (std::memory_order_relaxed),
                                    domain.lo, domain.hi);
        default: return 0.0f;
    }
}

inline void applyTrackCompReleaseMs (ChannelStripParams& strip, float ms) noexcept
{
    const int mode = compModeOf (strip);
    const auto domain = detail::releaseDomain (mode);
    switch (mode)
    {
        case 1:
            strip.compFetRelease.store (std::clamp (ms, domain.lo, domain.hi),
                                        std::memory_order_relaxed);
            break;
        case 2:
            strip.compVcaRelease.store (std::clamp (ms, domain.lo, domain.hi),
                                        std::memory_order_relaxed);
            break;
        default: break;
    }
}

inline float trackCompReleaseMs (const ChannelStripParams& strip) noexcept
{
    const int mode = compModeOf (strip);
    const auto domain = detail::releaseDomain (mode);
    switch (mode)
    {
        case 1:  return std::clamp (strip.compFetRelease.load (std::memory_order_relaxed),
                                    domain.lo, domain.hi);
        case 2:  return std::clamp (strip.compVcaRelease.load (std::memory_order_relaxed),
                                    domain.lo, domain.hi);
        default: return 0.0f;
    }
}

inline void resetTrackCompAttack (ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1: strip.compFetAttack.store (kFetAttackDefaultMs, std::memory_order_relaxed); break;
        case 2: strip.compVcaAttack.store (kVcaAttackDefaultMs, std::memory_order_relaxed); break;
        default: break;
    }
}

inline void resetTrackCompRelease (ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1: strip.compFetRelease.store (kFetReleaseDefaultMs, std::memory_order_relaxed); break;
        case 2: strip.compVcaRelease.store (kVcaReleaseDefaultMs, std::memory_order_relaxed); break;
        default: break;
    }
}

//==============================================================================
// Ratio
//==============================================================================
// FET's ratio is a five-position switch (4:1 / 8:1 / 12:1 / 20:1 / All,
// measured 3.85 / 7.4 / 12.5 / 21.5 / 21.5 in the donor, with All taking a
// different curve entirely) while VCA's is a continuous 1..120:1 sweep. No
// single scalar means the same thing in both - the two 21.5s alone make a
// numeric read-back ambiguous - so ratio gets no apply / read-back pair. A
// continuous knob picks a rung through the sweep ratioKnobDomainFor gives
// it; a relative control steps the switch a position at a time.
inline Domain ratioKnobDomainFor (const ChannelStripParams& strip) noexcept
{
    return detail::ratioKnobDomain (compModeOf (strip));
}

// One event carries however many detents arrived in the block, and an
// accelerated encoder can send 63 of them - enough to slam the switch from
// end to end. FET moves a single rung per event; VCA consumes the whole
// count, scaling by the caller's factor per detent the way the timing
// controls do, because 1..120:1 is a logarithmic range too.
inline void nudgeTrackCompRatio (ChannelStripParams& strip, int detents,
                                 float vcaFactorPerDetent) noexcept
{
    if (detents == 0) return;
    switch (compModeOf (strip))
    {
        case 1:
            strip.compFetRatio.store (
                std::clamp (strip.compFetRatio.load (std::memory_order_relaxed)
                                + (detents > 0 ? 1 : -1),
                            0, kFetRatioMaxIndex),
                std::memory_order_relaxed);
            break;
        case 2:
            strip.compVcaRatio.store (
                std::clamp (strip.compVcaRatio.load (std::memory_order_relaxed)
                                * std::pow (vcaFactorPerDetent, (float) detents),
                            kVcaRatioMin, kVcaRatioMax),
                std::memory_order_relaxed);
            break;
        default: break;
    }
}

inline void resetTrackCompRatio (ChannelStripParams& strip) noexcept
{
    switch (compModeOf (strip))
    {
        case 1: strip.compFetRatio.store (kFetRatioDefaultIndex, std::memory_order_relaxed); break;
        case 2: strip.compVcaRatio.store (kVcaRatioDefault, std::memory_order_relaxed); break;
        default: break;
    }
}
} // namespace comp
} // namespace duskstudio
