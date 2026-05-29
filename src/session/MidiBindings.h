#pragma once

#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

namespace duskstudio
{
// MIDI Learn bindings — bind external CC/Note/PB/MMC to a Dusk Studio
// target (transport / fader / mute / solo / arm / etc). Persist in
// session JSON so the user's controller setup travels with the project.
//
// v1: CC + Note + PB + MMC. NRPN out of scope. Notes fire on press only
// (NoteOn vel > 0); latching foot-switches with explicit release mode
// are deferred.

enum class MidiBindingTrigger : int
{
    CC         = 0,
    Note       = 1,
    PitchBend  = 2,   // dataNumber unused (per-channel). 14-bit 0..16383 —
                      // for MCU-style fader controllers.
    MmcCommand = 3,   // dataNumber = MMC code (1=stop, 2=play, 3=def-play,
                      // 4=ffwd, 5=rew, 6=recStart, 7=recStop, 9=pause).
                      // Sysex → no channel. One-shot press.
};

// Same wire stream from two different hardware button styles:
//   Momentary (B-type) : each press sends 127 then 0 — rising edge fires
//     once. Default.
//   Latching (D-type)  : each press alternates 127/0 — rising edge
//     fires every other click. Must fire on EVERY received message to
//     toggle once per click.
// User picks via right-click menu (can't tell them apart from the wire).
enum class MidiButtonMode : int
{
    Press  = 0,    // Rising edge only (val >= 64). Momentary.
    Toggle = 1,    // Fire on every message. Latching.
};

enum class MidiBindingTarget : int
{
    None = 0,

    TransportPlay     = 1,
    TransportStop     = 2,
    TransportRecord   = 3,
    TransportToggle   = 4,

    TrackFader        = 100,
    TrackPan          = 101,
    TrackMute         = 102,
    TrackSolo         = 103,
    TrackArm          = 104,
    TrackAuxSend      = 105, // targetIndex = track * kNumAuxSends + auxIdx
    TrackHpfFreq      = 106,
    TrackEqGain       = 107, // targetIndex = track * 4 + band (0=LF 1=LM 2=HM 3=HF)
    // Comp targets bind the LOGICAL knob — apply path reads compMode and
    // writes the matching per-mode atom so a single binding survives
    // mode flips (Opto/FET/VCA all have different ranges).
    TrackCompThresh   = 108,
    TrackCompMakeup   = 109,
    TrackPluginParam  = 110, // paramIndex (separate field) = plugin slot.

    // Bank-relative — targetIndex is a POSITION 0..kBankSize-1 (or
    // packed pos*N+sub). Resolved at audio time via
    //   absoluteTrack = activeBank * kBankSize + pos.
    // Lets one 8-fader controller drive whichever 8 of the 24 tracks
    // are in the active bank.
    TrackFaderBank        = 130,
    TrackPanBank          = 131,
    TrackMuteBank         = 132,
    TrackSoloBank         = 133,
    TrackArmBank          = 134,
    TrackAuxSendBank      = 135, // pos * kNumAuxSends + auxIdx
    TrackHpfFreqBank      = 136,
    TrackEqGainBank       = 137, // pos * kPackedEqBands + band
    TrackCompThreshBank   = 138,
    TrackCompMakeupBank   = 139,
    TrackPluginParamBank  = 140,

    BusFader          = 150,
    BusPan            = 151,
    BusMute           = 152,
    BusSolo           = 153,

    AuxLaneFader      = 160,
    AuxLaneMute       = 161,

    MasterFader       = 200,

    // ── H3 expansion (Phase 5a) ───────────────────────────────────────
    // Discrete toggles (buttons).
    TrackEqEnabled       = 210, // targetIndex = track
    TrackCompEnabled     = 211, // targetIndex = track
    TrackInsertBypass    = 212, // targetIndex = track — bypasses the
                                 // per-channel hardware insert slot.
    TrackAuxSendPrePost  = 213, // targetIndex = packTrackAux(track, aux).
                                 // Toggles between pre-fader and post-fader
                                 // routing for that (track, aux) pair.

    // Continuous bus EQ. targetIndex = bus * kBusEqBands + band.
    // BusStrip exposes LF / MID / HF gains (3-band). Frequencies are
    // fixed by BusStrip's tone-shaping spec — gain is the only knob
    // user-mapped from a CC.
    BusEqGain            = 220,

    // Master EQ — Pultec-style. Two main user knobs (low boost, high
    // boost). targetIndex unused.
    MasterEqLfBoost      = 230,
    MasterEqHfBoost      = 231,

    // Master compressor (Bus mode). targetIndex unused; one master.
    MasterCompThresh     = 240,
    MasterCompMakeup     = 241,
    MasterCompRatio      = 242,
};

// Helpers for the bus EQ packed index. Bus count × band count fits well
// inside targetIndex's 32-bit range; mirror the TrackEq pack/unpack
// pattern so future bus-aware targets stay consistent.
constexpr int kBusEqBands = 3;        // LF / MID / HF
constexpr int packBusEqBand (int bus, int band) noexcept
{
    return bus * kBusEqBands + band;
}
constexpr int unpackBusEqBus  (int packed) noexcept { return packed / kBusEqBands; }
constexpr int unpackBusEqBand (int packed) noexcept { return packed % kBusEqBands; }

constexpr bool isContinuousTarget (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackFader
        || t == MidiBindingTarget::TrackPan
        || t == MidiBindingTarget::TrackAuxSend
        || t == MidiBindingTarget::TrackHpfFreq
        || t == MidiBindingTarget::TrackEqGain
        || t == MidiBindingTarget::TrackCompThresh
        || t == MidiBindingTarget::TrackCompMakeup
        || t == MidiBindingTarget::TrackPluginParam
        || t == MidiBindingTarget::TrackFaderBank
        || t == MidiBindingTarget::TrackPanBank
        || t == MidiBindingTarget::TrackAuxSendBank
        || t == MidiBindingTarget::TrackHpfFreqBank
        || t == MidiBindingTarget::TrackEqGainBank
        || t == MidiBindingTarget::TrackCompThreshBank
        || t == MidiBindingTarget::TrackCompMakeupBank
        || t == MidiBindingTarget::TrackPluginParamBank
        || t == MidiBindingTarget::BusFader
        || t == MidiBindingTarget::BusPan
        || t == MidiBindingTarget::AuxLaneFader
        || t == MidiBindingTarget::MasterFader
        // H3 expansion: continuous bus + master targets.
        || t == MidiBindingTarget::BusEqGain
        || t == MidiBindingTarget::MasterEqLfBoost
        || t == MidiBindingTarget::MasterEqHfBoost
        || t == MidiBindingTarget::MasterCompThresh
        || t == MidiBindingTarget::MasterCompMakeup
        || t == MidiBindingTarget::MasterCompRatio;
}

constexpr bool isBankRelativeTarget (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackFaderBank
        || t == MidiBindingTarget::TrackPanBank
        || t == MidiBindingTarget::TrackMuteBank
        || t == MidiBindingTarget::TrackSoloBank
        || t == MidiBindingTarget::TrackArmBank
        || t == MidiBindingTarget::TrackAuxSendBank
        || t == MidiBindingTarget::TrackHpfFreqBank
        || t == MidiBindingTarget::TrackEqGainBank
        || t == MidiBindingTarget::TrackCompThreshBank
        || t == MidiBindingTarget::TrackCompMakeupBank
        || t == MidiBindingTarget::TrackPluginParamBank;
}

constexpr bool needsTrackIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackFader
        || t == MidiBindingTarget::TrackPan
        || t == MidiBindingTarget::TrackMute
        || t == MidiBindingTarget::TrackSolo
        || t == MidiBindingTarget::TrackArm
        // H3 expansion: per-track discrete toggles.
        || t == MidiBindingTarget::TrackEqEnabled
        || t == MidiBindingTarget::TrackCompEnabled
        || t == MidiBindingTarget::TrackInsertBypass;
}

constexpr bool needsBusIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::BusFader
        || t == MidiBindingTarget::BusPan
        || t == MidiBindingTarget::BusMute
        || t == MidiBindingTarget::BusSolo;
}

constexpr bool needsAuxLaneIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::AuxLaneFader
        || t == MidiBindingTarget::AuxLaneMute;
}

// Only the absolute-track variant. TrackAuxSendBank uses a smaller
// (kBankSize-based) range that callers bounds-check separately.
constexpr bool needsPackedTrackAuxIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackAuxSend
        || t == MidiBindingTarget::TrackAuxSendPrePost;
}

// 24 × 4 = 96, fits in targetIndex. Header-only — Session.h pulls in
// too much.
constexpr int kPackedAuxLanes = 4;
constexpr int packTrackAux (int track, int aux) noexcept
{
    return track * kPackedAuxLanes + aux;
}
constexpr int unpackTrackAuxTrack (int packed) noexcept { return packed / kPackedAuxLanes; }
constexpr int unpackTrackAuxLane  (int packed) noexcept { return packed % kPackedAuxLanes; }

// 24 × 4 = 96, fits in targetIndex.
constexpr int kPackedEqBands = 4;
constexpr int packTrackEqBand (int track, int band) noexcept
{
    return track * kPackedEqBands + band;
}
constexpr int unpackTrackEqTrack (int packed) noexcept { return packed / kPackedEqBands; }
constexpr int unpackTrackEqBand  (int packed) noexcept { return packed % kPackedEqBands; }

constexpr bool needsPackedTrackEqIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackEqGain;
}

// H3: BusEqGain stores bus * kBusEqBands + band in targetIndex.
constexpr bool needsPackedBusEqIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::BusEqGain;
}

struct MidiBinding
{
    int channel = 0;                 // 0 = any; 1..16 = filter
    int dataNumber = 0;              // CC# or note# (0..127)
    MidiBindingTrigger trigger = MidiBindingTrigger::CC;
    MidiBindingTarget  target  = MidiBindingTarget::None;
    int targetIndex = 0;
    // TrackPluginParam only — which parameter slot in the plugin.
    // Filled at learn-resolve from PluginSlot::getLastTouchedParamIndex.
    int paramIndex = 0;
    // Discrete (button) targets only.
    MidiButtonMode buttonMode = MidiButtonMode::Press;

    bool isValid() const noexcept
    {
        return target != MidiBindingTarget::None
            && dataNumber >= 0 && dataNumber <= 127
            && channel   >= 0 && channel    <= 16;
    }

    // Per-trigger matching:
    //   CC / Note  : channel (0 = wildcard) + dataNumber
    //   PitchBend  : channel only (no dataNumber)
    //   MmcCommand : dataNumber only (sysex, no channel)
    bool sourceMatches (int ch, int dn, MidiBindingTrigger tg) const noexcept
    {
        if (trigger != tg) return false;
        if (trigger == MidiBindingTrigger::PitchBend)
            return channel == 0 || channel == ch;
        if (dataNumber != dn) return false;
        if (trigger == MidiBindingTrigger::MmcCommand) return true;
        if (channel != 0 && channel != ch) return false;
        return true;
    }
};

// Audio thread sets when a binding hits transport; message-thread timer
// drains (engine.play/stop/record aren't RT-safe).
enum class PendingTransportAction : int
{
    None    = 0,
    Play    = 1,
    Stop    = 2,
    Record  = 3,
    Toggle  = 4,
};

// Packed into atomic<int>: target enum high bits, track index low 8.
// -1 = no learn pending.
constexpr int packLearnTarget (MidiBindingTarget t, int idx) noexcept
{
    return ((int) t << 8) | (idx & 0xff);
}
constexpr MidiBindingTarget unpackLearnTargetKind (int packed) noexcept
{
    return packed < 0 ? MidiBindingTarget::None
                      : (MidiBindingTarget) ((packed >> 8) & 0xffff);
}
constexpr int unpackLearnTargetIndex (int packed) noexcept
{
    return packed < 0 ? 0 : (packed & 0xff);
}

// Audio→message handoff. Packed int64: 8b trigger + 8b channel + 8b
// dataNumber + 1b valid. Audio CAS-stores when learnPending; message
// loads + clears.
constexpr juce::int64 packLearnCapture (MidiBindingTrigger tg, int ch, int dn) noexcept
{
    return ((juce::int64) 1 << 32)
         | ((juce::int64) (int) tg << 16)
         | ((juce::int64) (ch & 0xff) << 8)
         | ((juce::int64) (dn & 0xff));
}
constexpr bool learnCaptureIsValid (juce::int64 packed) noexcept
{
    return ((packed >> 32) & 1) != 0;
}
constexpr MidiBindingTrigger unpackLearnCaptureTrigger (juce::int64 packed) noexcept
{
    return (MidiBindingTrigger) ((packed >> 16) & 0xff);
}
constexpr int unpackLearnCaptureChannel (juce::int64 packed) noexcept
{
    return (int) ((packed >> 8) & 0xff);
}
constexpr int unpackLearnCaptureDataNumber (juce::int64 packed) noexcept
{
    return (int) (packed & 0xff);
}

const char* nameForTarget (MidiBindingTarget t) noexcept;

// Resolves indices ("Track 3 fader", "Bus 2 mute", "Track 4 EQ LM gain").
// For TrackPluginParam, looks up the loaded plugin and resolves
// paramIndex to a parameter name; nullptr engine falls back to "Track N
// plugin param M".
class AudioEngine;
juce::String describeBindingTarget (const MidiBinding& b,
                                     const AudioEngine* engine);

// "Ch - CC 23", "Ch 1 Note 60", etc.
juce::String describeBindingSource (const MidiBinding& b);

// Preset .json: top-level object with `format_version` + `midi_bindings`
// array (matches the embedded session form). std::nullopt = malformed /
// wrong schema; empty vector = well-formed "clear all".
juce::String serializeBindingsPreset (const std::vector<MidiBinding>& binds);
std::optional<std::vector<MidiBinding>> deserializeBindingsPreset (const juce::String& json);

class Session;
} // namespace duskstudio

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
namespace midilearn
{
// Right-click menu on `target` for (kind, index): "MIDI Learn" sets the
// learn-pending atom; "MIDI: Ch X CC/Note Y" is informational; "Forget
// binding" removes it. Centralised so TransportBar / ChannelStripComponent
// / MasterStripComponent all read identically.
void showLearnMenu (juce::Component& target,
                    Session& session,
                    MidiBindingTarget kind,
                    int index = 0);
} // namespace midilearn
} // namespace duskstudio
