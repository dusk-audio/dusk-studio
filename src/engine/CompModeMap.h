#pragma once

#include "../session/CompParams.h"
#include "../session/Session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

namespace duskstudio
{
namespace comp
{
// Every compressor in the mixer is a Multi-Comp 2. A control that moves one,
// whether a knob on a strip, bus or master face, a threshold handle, a MIDI
// binding or a Mackie V-pot, names a role rather than a parameter: the mode
// that plays picks the plug-in parameter behind the role, and the role moves
// it across the plug-in's whole host range, the travel of the plug-in's own
// knob. Everything that has to land on "whatever the active mode reads"
// routes through here, so those controls cannot drift apart.
//
// Values are the session's CompParams: the plug-in's host domain, under its
// own symbols. Nothing here formats text; what a value reads as is the
// plug-in's to say (MultiCompStage::valueText).
//
// A role's fraction (0..1) is its position along the role's own direction. For
// the threshold that is a threshold's direction, up being less compression, in
// every mode: VCA and Bus have a threshold, while Opto's PEAK REDUCTION and
// FET's INPUT compress more as they turn up, so their threshold fraction runs
// against the host value. A binding's apply and its soft-takeover read-back
// are these two functions and stay exact inverses.
//
// Relative controls (the Mackie V-pots) start from the value held, whoever
// wrote it last: a knob, a binding, a mode switch or a session load.
//
// Every strip entry point loads compMode once and carries it through: the mode
// is written from the message thread while these run on the audio thread, and
// a second load could move a parameter the block's mode never reads.
//
// RT-safe: no allocation, no locks, relaxed loads and stores per the param
// convention. Callable from the audio thread (McuReceiver, the binding apply
// and read-back) and the message thread alike.

// Multi-Comp 2's modes as the mixer runs them: a strip's compMode picks one of
// the first three; the buses and the master run the Bus mode.
constexpr int kOptoMode = 0;
constexpr int kFetMode  = 1;
constexpr int kVcaMode  = 2;
constexpr int kBusMode  = 3;

enum class Role { Threshold, Makeup, Ratio, Attack, Release };

// A relative control crosses a continuous parameter's host range in this many
// detents and moves a stepped one a single step per event: an accelerated
// encoder packs up to 63 detents into one event, enough to slam a switch from
// end to end.
constexpr float kDetentsPerSweep = 100.0f;

// The threshold handle's span for Opto and FET, whose threshold role is not in
// dB: their host travel runs down the strip meter's 60 dB, more reduction or
// drive lower, as a lower threshold compresses more.
constexpr float kReversedThresholdSpanDb = 60.0f;

struct Domain
{
    float lo;
    float hi;
};

inline float fracToValue (Domain d, float frac) noexcept
{
    return d.lo + frac * (d.hi - d.lo);
}

inline float valueToFrac (Domain d, float value) noexcept
{
    return d.hi > d.lo ? std::clamp ((value - d.lo) / (d.hi - d.lo), 0.0f, 1.0f)
                       : 0.0f;
}

inline int compModeOf (const ChannelStripParams& strip) noexcept
{
    return std::clamp (strip.compMode.load (std::memory_order_relaxed), kOptoMode, kVcaMode);
}

struct RoleParam
{
    bool exists;
    CompParamId id;
    // The role's fraction runs against the host value.
    bool reversed;
};

// The plug-in parameter behind a role in a mode. Opto has no ratio, attack or
// release, VCA no attack or release: the plug-in exposes none.
constexpr RoleParam roleParam (int mode, Role role) noexcept
{
    using Id = CompParamId;
    switch (role)
    {
        case Role::Threshold:
            switch (mode)
            {
                case kOptoMode: return { true, Id::OptoPeakReduction, true };
                case kFetMode:  return { true, Id::FetInput, true };
                case kVcaMode:  return { true, Id::VcaThreshold, false };
                case kBusMode:  return { true, Id::BusThreshold, false };
                default: break;
            }
            break;
        case Role::Makeup:
            switch (mode)
            {
                case kOptoMode: return { true, Id::OptoGain, false };
                case kFetMode:  return { true, Id::FetOutput, false };
                case kVcaMode:  return { true, Id::VcaOutput, false };
                case kBusMode:  return { true, Id::BusMakeup, false };
                default: break;
            }
            break;
        case Role::Ratio:
            switch (mode)
            {
                case kFetMode:  return { true, Id::FetRatio, false };
                case kVcaMode:  return { true, Id::VcaCompression, false };
                case kBusMode:  return { true, Id::BusRatio, false };
                default: break;
            }
            break;
        case Role::Attack:
            switch (mode)
            {
                case kFetMode:  return { true, Id::FetAttack, false };
                case kBusMode:  return { true, Id::BusAttack, false };
                default: break;
            }
            break;
        case Role::Release:
            switch (mode)
            {
                case kFetMode:  return { true, Id::FetRelease, false };
                case kBusMode:  return { true, Id::BusRelease, false };
                default: break;
            }
            break;
    }
    return { false, Id::Mix, false };
}

constexpr bool modeHasRole (int mode, Role role) noexcept
{
    return roleParam (mode, role).exists;
}

inline Domain hostDomain (CompParamId id) noexcept
{
    const auto& spec = compParamSpec (id);
    return { spec.hostMin, spec.hostMax };
}

// The role's position, or -1 when the mode has no such control. A value held
// outside the host range reads as the end it is past.
inline float roleFrac (const CompParams& comp, int mode, Role role) noexcept
{
    const auto p = roleParam (mode, role);
    if (! p.exists) return -1.0f;
    const float t = valueToFrac (hostDomain (p.id), comp.get (p.id));
    return p.reversed ? 1.0f - t : t;
}

// A stepped parameter takes the step nearest the fraction. False, and nothing
// moved, when the mode has no such control.
inline bool applyRoleFrac (CompParams& comp, int mode, Role role, float frac) noexcept
{
    const auto p = roleParam (mode, role);
    if (! p.exists) return false;
    const float t = std::clamp (frac, 0.0f, 1.0f);
    comp.set (p.id, fracToValue (hostDomain (p.id), p.reversed ? 1.0f - t : t));
    return true;
}

// detents > 0 turns the role up its own direction.
inline bool nudgeRole (CompParams& comp, int mode, Role role, int detents) noexcept
{
    const auto p = roleParam (mode, role);
    if (! p.exists) return false;
    if (detents == 0) return true;
    const auto& spec = compParamSpec (p.id);
    const float sign = (detents > 0) != p.reversed ? 1.0f : -1.0f;
    const float held = clampCompParam (p.id, comp.get (p.id));
    const float step = spec.integer ? 1.0f
                                    : (float) std::abs (detents) * (spec.hostMax - spec.hostMin)
                                          / kDetentsPerSweep;
    comp.set (p.id, held + sign * step);
    return true;
}

// Back to the plug-in's default.
inline bool resetRole (CompParams& comp, int mode, Role role) noexcept
{
    const auto p = roleParam (mode, role);
    if (! p.exists) return false;
    comp.set (p.id, compParamSpec (p.id).hostDefault);
    return true;
}

// The threshold handle's scale: dB for VCA and Bus, whose parameter is a
// threshold, and 0 down to -kReversedThresholdSpanDb for Opto and FET.
inline Domain thresholdAxis (int mode) noexcept
{
    const auto p = roleParam (mode, Role::Threshold);
    return p.reversed ? Domain { -kReversedThresholdSpanDb, 0.0f } : hostDomain (p.id);
}

inline float thresholdAxisValue (const CompParams& comp, int mode) noexcept
{
    return fracToValue (thresholdAxis (mode), roleFrac (comp, mode, Role::Threshold));
}

inline void applyThresholdAxisValue (CompParams& comp, int mode, float value) noexcept
{
    applyRoleFrac (comp, mode, Role::Threshold, valueToFrac (thresholdAxis (mode), value));
}

//==============================================================================
// A channel strip, in the mode it plays.
//==============================================================================
inline float trackRoleFrac (const ChannelStripParams& strip, Role role) noexcept
{
    return roleFrac (strip.comp, compModeOf (strip), role);
}

inline bool applyTrackRoleFrac (ChannelStripParams& strip, Role role, float frac) noexcept
{
    return applyRoleFrac (strip.comp, compModeOf (strip), role, frac);
}

inline bool nudgeTrackRole (ChannelStripParams& strip, Role role, int detents) noexcept
{
    return nudgeRole (strip.comp, compModeOf (strip), role, detents);
}

inline bool resetTrackRole (ChannelStripParams& strip, Role role) noexcept
{
    return resetRole (strip.comp, compModeOf (strip), role);
}

inline float trackThresholdAxisValue (const ChannelStripParams& strip) noexcept
{
    return thresholdAxisValue (strip.comp, compModeOf (strip));
}

inline void applyTrackThresholdAxisValue (ChannelStripParams& strip, float value) noexcept
{
    applyThresholdAxisValue (strip.comp, compModeOf (strip), value);
}
} // namespace comp
} // namespace duskstudio

#include "CompModeMapLegacy.h"
