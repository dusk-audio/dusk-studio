#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace duskstudio
{
// MIDI controller infrastructure. Lets the user bind an external CC or note
// to a target inside Dusk Studio (transport, fader, mute, solo, arm, master fader)
// via a "MIDI Learn" workflow. Bindings persist in the session JSON so a
// user's pedal / fader-bank setup travels with the project.
//
// v1 scope (this file):
//   • CC and Note triggers. NRPN / pitch-bend out of scope.
//   • Per-track + per-master targets. Buses / aux sends not yet wired.
//   • Note triggers fire on press only (NoteOn velocity > 0). Note Off and
//     CC release are ignored - latching foot-switches would need a release
//     mode and we ship without that for now.

enum class MidiBindingTrigger : int
{
    CC   = 0,    // dataNumber = controller number (0..127)
    Note = 1,    // dataNumber = note number (0..127)
};

enum class MidiBindingTarget : int
{
    None = 0,                // sentinel; never appears in a stored binding

    TransportPlay     = 1,
    TransportStop     = 2,
    TransportRecord   = 3,
    TransportToggle   = 4,   // play if stopped, stop if rolling

    TrackFader        = 100, // continuous; CC value 0..127 -> faderDb range
    TrackPan          = 101, // continuous; -1..+1
    TrackMute         = 102, // discrete on press
    TrackSolo         = 103, // discrete on press
    TrackArm          = 104, // discrete on press
    TrackAuxSend      = 105, // continuous; targetIndex packs track + aux
                              // (track * kNumAuxSends + auxIdx)
    TrackHpfFreq      = 106, // continuous; targetIndex = track
    TrackEqGain       = 107, // continuous; targetIndex packs track + band
                              // (track * 4 + band, band 0=LF 1=LM 2=HM 3=HF)
    // Mode-aware compressor bindings. The strip has 3 comp models
    // (Opto/FET/VCA); each has its own threshold + makeup knob with a
    // different range. These targets bind the LOGICAL knob: the apply
    // path reads `compMode` per block and writes to the matching
    // per-mode atom so a single binding survives mode flips.
    TrackCompThresh   = 108, // continuous; targetIndex = track
    TrackCompMakeup   = 109, // continuous; targetIndex = track
    TrackPluginParam  = 110, // continuous; targetIndex = track,
                              // paramIndex (separate field) = plugin
                              // parameter slot in the loaded instance.

    BusFader          = 150, // continuous; targetIndex = bus 0..kNumBuses-1
    BusPan            = 151, // continuous; -1..+1
    BusMute           = 152, // discrete on press
    BusSolo           = 153, // discrete on press

    AuxLaneFader      = 160, // continuous; targetIndex = lane 0..kNumAuxLanes-1
    AuxLaneMute       = 161, // discrete on press

    MasterFader       = 200, // continuous
};

// True when the target's value is set continuously from the CC value
// (0..127). False = discrete on-press toggle / trigger.
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
        || t == MidiBindingTarget::BusFader
        || t == MidiBindingTarget::BusPan
        || t == MidiBindingTarget::AuxLaneFader
        || t == MidiBindingTarget::MasterFader;
}

// True when the target needs a strip index. Track + bus targets both
// use targetIndex; the apply path disambiguates via the enum value.
// False = global target (transport / master / etc.).
constexpr bool needsTrackIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackFader
        || t == MidiBindingTarget::TrackPan
        || t == MidiBindingTarget::TrackMute
        || t == MidiBindingTarget::TrackSolo
        || t == MidiBindingTarget::TrackArm;
}

// True when the target needs a bus index (0..kNumBuses-1).
constexpr bool needsBusIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::BusFader
        || t == MidiBindingTarget::BusPan
        || t == MidiBindingTarget::BusMute
        || t == MidiBindingTarget::BusSolo;
}

// True when the target needs an aux-lane index (0..kNumAuxLanes-1).
constexpr bool needsAuxLaneIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::AuxLaneFader
        || t == MidiBindingTarget::AuxLaneMute;
}

// True when the target packs two indices (track + aux-lane) into
// targetIndex via packTrackAux(). The apply path decodes back to the
// (track, aux) pair before reading the right atom.
constexpr bool needsPackedTrackAuxIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackAuxSend;
}

// Packing helpers for TrackAuxSend. Bus + aux range fits within the
// 0..255 budget for targetIndex (kNumTracks=16 x kNumAuxSends=4 = 64).
// kNumAuxSendsLanes is the multiplier - hard-coded so the helper stays
// header-only (Session.h pulls in too much).
constexpr int kPackedAuxLanes = 4;
constexpr int packTrackAux (int track, int aux) noexcept
{
    return track * kPackedAuxLanes + aux;
}
constexpr int unpackTrackAuxTrack (int packed) noexcept { return packed / kPackedAuxLanes; }
constexpr int unpackTrackAuxLane  (int packed) noexcept { return packed % kPackedAuxLanes; }

// Same packing shape for TrackEqGain: 4 bands per track (LF / LM / HM / HF).
// Range fits in targetIndex (kNumTracks 16 * 4 = 64).
constexpr int kPackedEqBands = 4;
constexpr int packTrackEqBand (int track, int band) noexcept
{
    return track * kPackedEqBands + band;
}
constexpr int unpackTrackEqTrack (int packed) noexcept { return packed / kPackedEqBands; }
constexpr int unpackTrackEqBand  (int packed) noexcept { return packed % kPackedEqBands; }

// True when the target packs a (track, band) pair like TrackEqGain.
constexpr bool needsPackedTrackEqIndex (MidiBindingTarget t) noexcept
{
    return t == MidiBindingTarget::TrackEqGain;
}

struct MidiBinding
{
    int channel = 0;                 // 0 = any; 1..16 = filter
    int dataNumber = 0;              // CC# or note# (0..127)
    MidiBindingTrigger trigger = MidiBindingTrigger::CC;
    MidiBindingTarget  target  = MidiBindingTarget::None;
    int targetIndex = 0;             // track index for per-strip targets
    // Secondary index used only for TrackPluginParam: which parameter
    // slot inside the loaded plugin instance. Filled at learn-resolve
    // time from PluginSlot::getLastTouchedParamIndex. Zero / unused
    // for every other target.
    int paramIndex = 0;

    // True when this binding is live (target != None and dataNumber valid).
    bool isValid() const noexcept
    {
        return target != MidiBindingTarget::None
            && dataNumber >= 0 && dataNumber <= 127
            && channel   >= 0 && channel    <= 16;
    }

    // Equality on (channel, dataNumber, trigger). Used for "is this MIDI
    // event one this binding cares about" matching at the audio thread,
    // and for de-duping in the learn workflow (a learn capture replaces
    // any pre-existing binding on the same source).
    bool sourceMatches (int ch, int dn, MidiBindingTrigger tg) const noexcept
    {
        if (trigger != tg) return false;
        if (dataNumber != dn) return false;
        if (channel != 0 && channel != ch) return false;
        return true;
    }
};

// Pending-transport-action signal: audio thread sets one of these when a
// binding hits a transport target; message-thread timer drains it and
// calls the corresponding engine method (engine.play / stop / record are
// not RT-safe). Stored as int in std::atomic<int> on Session.
enum class PendingTransportAction : int
{
    None    = 0,
    Play    = 1,
    Stop    = 2,
    Record  = 3,
    Toggle  = 4,
};

// Learn-pending signal: UI sets a target descriptor; audio thread sees a
// MIDI message and stamps a capture; message-thread timer drains the
// capture and appends a binding. Packed into a single atomic<int> with
// the target enum in the high bits and the track index (0..255 - the
// mask is 0xff; only 0..15 are actually used today) in the low 8.
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

// Captured MIDI source from the audio thread, drained on the message
// thread. Atomically packed into a single int64: 8 bits trigger + 8 bits
// channel + 8 bits dataNumber + 1 bit "valid". The audio thread CAS-stores
// this only when learnPending is set; the message thread loads it,
// processes, and clears.
constexpr juce::int64 packLearnCapture (MidiBindingTrigger tg, int ch, int dn) noexcept
{
    return ((juce::int64) 1 << 32)         // valid flag
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

// Display label for a target - used by the right-click menu and the
// "MIDI: Ch 1 CC 23" readout. Track-targeted entries don't include the
// index here; the menu builder appends it.
const char* nameForTarget (MidiBindingTarget t) noexcept;

// Full human-readable description of a binding's TARGET, including
// resolved indices (e.g. "Track 3 fader", "Bus 2 mute", "AUX 1 return",
// "Track 4 EQ LM gain"). For TrackPluginParam, looks up the loaded
// plugin via the engine and resolves the paramIndex to a parameter
// name when possible; falls back to "Track N plugin param M" when no
// plugin is loaded or the index is out of range. Engine reference
// optional - pass nullptr for callers that don't need plugin-param
// resolution (the rest still resolves cleanly).
class AudioEngine;
juce::String describeBindingTarget (const MidiBinding& b,
                                     const AudioEngine* engine);

// Source description: "Ch - CC 23", "Ch 1 Note 60", etc. Single line,
// channel "-" for omni.
juce::String describeBindingSource (const MidiBinding& b);

// JSON serialize / deserialize for a bindings preset file. Same wire
// format the session serializer uses for the embedded `midi_bindings`
// array, wrapped in a top-level object with a `format_version` field so
// future schema changes can detect old files. UI uses these to export
// the current binding set to a .json file for sharing across sessions
// / machines and to import one back.
juce::String serializeBindingsPreset (const std::vector<MidiBinding>& binds);
std::vector<MidiBinding> deserializeBindingsPreset (const juce::String& json);

class Session;
} // namespace duskstudio

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
namespace midilearn
{
// Shared helper - shows a right-click menu on `target` for the given
// (target, index) pair. Three items: "MIDI Learn" sets the learn-pending
// atom; "MIDI: Ch X CC/Note Y" is informational and shows the current
// binding when one exists; "Forget binding" removes it. Centralised so
// every surface (TransportBar, ChannelStripComponent, MasterStripComponent)
// reads identically and stays in sync as the binding model evolves.
void showLearnMenu (juce::Component& target,
                    Session& session,
                    MidiBindingTarget kind,
                    int index = 0);
} // namespace midilearn
} // namespace duskstudio

