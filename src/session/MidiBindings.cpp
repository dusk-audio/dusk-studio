#include "MidiBindings.h"
#include "Session.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include <algorithm>

namespace duskstudio
{
juce::String describeBindingSource (const MidiBinding& b)
{
    // MMC is sysex (no channel); render in the same shape as
    // midilearn::describeBinding so the bindings panel + readout
    // stay consistent.
    if (b.trigger == MidiBindingTrigger::MmcCommand)
    {
        static constexpr const char* names[] = {
            "?", "Stop", "Play", "DefPlay", "FFwd", "Rew",
            "RecStart", "RecStop", "?", "Pause"
        };
        const int cmd = b.dataNumber;
        const char* name = (cmd >= 1 && cmd <= 9) ? names[cmd] : "?";
        return juce::String ("MMC ") + name;
    }
    const auto chStr = b.channel == 0 ? juce::String ("Ch -")
                                       : "Ch " + juce::String (b.channel);
    if (b.trigger == MidiBindingTrigger::PitchBend)
        return chStr + " PitchBend";
    const auto kindStr = b.trigger == MidiBindingTrigger::CC
                            ? juce::String ("CC ") : juce::String ("Note ");
    return chStr + " " + kindStr + juce::String (b.dataNumber);
}

juce::String describeBindingTarget (const MidiBinding& b,
                                     const AudioEngine* engine)
{
    // Centralised so the menu, the readout, and the bindings panel
    // all render the same label for any binding.
    auto trk = [&] { return juce::String (b.targetIndex + 1); };
    switch (b.target)
    {
        case MidiBindingTarget::None:            return "(unbound)";
        case MidiBindingTarget::TransportPlay:   return "Transport: Play";
        case MidiBindingTarget::TransportStop:   return "Transport: Stop";
        case MidiBindingTarget::TransportRecord: return "Transport: Record";
        case MidiBindingTarget::TransportToggle: return "Transport: Play/Stop";
        case MidiBindingTarget::TrackFader:      return "Track " + trk() + " fader";
        case MidiBindingTarget::TrackPan:        return "Track " + trk() + " pan";
        case MidiBindingTarget::TrackMute:       return "Track " + trk() + " mute";
        case MidiBindingTarget::TrackSolo:       return "Track " + trk() + " solo";
        case MidiBindingTarget::TrackArm:        return "Track " + trk() + " arm";
        case MidiBindingTarget::TrackAuxSend:
        {
            const int track = unpackTrackAuxTrack (b.targetIndex);
            const int aux   = unpackTrackAuxLane  (b.targetIndex);
            return "Track " + juce::String (track + 1)
                 + " AUX " + juce::String (aux + 1) + " send";
        }
        case MidiBindingTarget::TrackHpfFreq:    return "Track " + trk() + " HPF";
        case MidiBindingTarget::TrackEqGain:
        {
            const int track = unpackTrackEqTrack (b.targetIndex);
            const int band  = unpackTrackEqBand  (b.targetIndex);
            static const char* kBandNames[] = { "LF", "LM", "HM", "HF" };
            const char* bandName = (band >= 0 && band < 4) ? kBandNames[band] : "?";
            return "Track " + juce::String (track + 1)
                 + " EQ " + juce::String (bandName) + " gain";
        }
        case MidiBindingTarget::TrackEqFreq:
        {
            const int track = unpackTrackEqTrack (b.targetIndex);
            const int band  = unpackTrackEqBand  (b.targetIndex);
            static const char* kBandNames[] = { "LF", "LM", "HM", "HF" };
            const char* bandName = (band >= 0 && band < 4) ? kBandNames[band] : "?";
            return "Track " + juce::String (track + 1)
                 + " EQ " + juce::String (bandName) + " freq";
        }
        case MidiBindingTarget::TrackEqQ:
        {
            const int track = unpackTrackEqTrack (b.targetIndex);
            const int band  = unpackTrackEqBand  (b.targetIndex);
            static const char* kBandNames[] = { "LF", "LM", "HM", "HF" };
            const char* bandName = (band >= 0 && band < 4) ? kBandNames[band] : "?";
            return "Track " + juce::String (track + 1)
                 + " EQ " + juce::String (bandName) + " Q";
        }
        case MidiBindingTarget::TrackCompThresh: return "Track " + trk() + " comp threshold";
        case MidiBindingTarget::TrackCompMakeup: return "Track " + trk() + " comp makeup";
        case MidiBindingTarget::TrackPluginParam:
        {
            // Try to resolve the parameter's name via the engine. If
            // no plugin is loaded or paramIndex is out of range, fall
            // back to the index so the user still sees something
            // identifiable (and can right-click → Remove it).
            juce::String paramName;
            if (engine != nullptr
                && b.targetIndex >= 0
                && b.targetIndex < Session::kNumTracks)
            {
                const auto& slot = engine->getChannelStrip (b.targetIndex)
                                          .getPluginSlot();
                if (auto* instance = slot.getInstance())
                {
                    const auto& params = instance->getParameters();
                    if (b.paramIndex >= 0 && b.paramIndex < params.size())
                        if (auto* p = params[b.paramIndex])
                            paramName = p->getName (32);
                }
            }
            if (paramName.isEmpty())
                paramName = "param " + juce::String (b.paramIndex);
            return "Track " + trk() + " " + paramName;
        }
        // Bank-relative variants. targetIndex is a POSITION 0..kBankSize-1
        // (or packed pos*N+sub for AuxSend/EqGain). Display "Bank pos N"
        // so the bindings panel reads the right grammar.
        case MidiBindingTarget::TrackFaderBank:       return "Bank pos " + trk() + " fader (banked)";
        case MidiBindingTarget::TrackPanBank:         return "Bank pos " + trk() + " pan (banked)";
        case MidiBindingTarget::TrackMuteBank:        return "Bank pos " + trk() + " mute (banked)";
        case MidiBindingTarget::TrackSoloBank:        return "Bank pos " + trk() + " solo (banked)";
        case MidiBindingTarget::TrackArmBank:         return "Bank pos " + trk() + " arm (banked)";
        case MidiBindingTarget::TrackAuxSendBank:
        {
            const int pos = b.targetIndex / kPackedAuxLanes;
            const int aux = b.targetIndex % kPackedAuxLanes;
            return "Bank pos " + juce::String (pos + 1)
                 + " AUX " + juce::String (aux + 1) + " send (banked)";
        }
        case MidiBindingTarget::TrackHpfFreqBank:     return "Bank pos " + trk() + " HPF (banked)";
        case MidiBindingTarget::TrackEqGainBank:
        {
            const int pos  = b.targetIndex / kPackedEqBands;
            const int band = b.targetIndex % kPackedEqBands;
            static const char* kBandNames[] = { "LF", "LM", "HM", "HF" };
            const char* bandName = (band >= 0 && band < 4) ? kBandNames[band] : "?";
            return "Bank pos " + juce::String (pos + 1)
                 + " EQ " + juce::String (bandName) + " gain (banked)";
        }
        case MidiBindingTarget::TrackEqFreqBank:
        case MidiBindingTarget::TrackEqQBank:
        {
            const int pos  = b.targetIndex / kPackedEqBands;
            const int band = b.targetIndex % kPackedEqBands;
            static const char* kBandNames[] = { "LF", "LM", "HM", "HF" };
            const char* bandName = (band >= 0 && band < 4) ? kBandNames[band] : "?";
            const char* knob = (b.target == MidiBindingTarget::TrackEqFreqBank) ? " freq" : " Q";
            return "Bank pos " + juce::String (pos + 1)
                 + " EQ " + juce::String (bandName) + juce::String (knob) + " (banked)";
        }
        case MidiBindingTarget::TrackCompThreshBank:  return "Bank pos " + trk() + " comp threshold (banked)";
        case MidiBindingTarget::TrackCompMakeupBank:  return "Bank pos " + trk() + " comp makeup (banked)";
        case MidiBindingTarget::TrackPluginParamBank: return "Bank pos " + trk() + " plugin param (banked)";

        case MidiBindingTarget::BusFader:        return "Bus " + trk() + " fader";
        case MidiBindingTarget::BusPan:          return "Bus " + trk() + " pan";
        case MidiBindingTarget::BusMute:         return "Bus " + trk() + " mute";
        case MidiBindingTarget::BusSolo:         return "Bus " + trk() + " solo";
        case MidiBindingTarget::AuxLaneFader:    return "AUX " + trk() + " return";
        case MidiBindingTarget::AuxLaneMute:     return "AUX " + trk() + " mute";
        case MidiBindingTarget::AuxPluginParam:  return "AUX " + trk() + " plugin param "
                                                        + juce::String (b.paramIndex + 1);
        case MidiBindingTarget::MasterFader:     return "Master fader";

        // Mirror the grammar of the track / bus / master labels so the
        // bindings panel + right-click menu read consistently.
        case MidiBindingTarget::TrackEqEnabled:     return "Track " + trk() + " EQ on/off";
        case MidiBindingTarget::TrackCompEnabled:   return "Track " + trk() + " comp on/off";
        case MidiBindingTarget::TrackInsertBypass:  return "Track " + trk() + " insert bypass";

        case MidiBindingTarget::TrackAuxSendPrePost:
        {
            const int track = unpackTrackAuxTrack (b.targetIndex);
            const int aux   = unpackTrackAuxLane  (b.targetIndex);
            return "Track " + juce::String (track + 1)
                 + " AUX " + juce::String (aux + 1) + " pre/post";
        }

        case MidiBindingTarget::BusEqGain:
        {
            const int bus  = unpackBusEqBus  (b.targetIndex);
            const int band = unpackBusEqBand (b.targetIndex);
            static const char* kBusBandNames[] = { "LF", "MID", "HF" };
            const char* bandName = (band >= 0 && band < kBusEqBands)
                                       ? kBusBandNames[band] : "?";
            return "Bus " + juce::String (bus + 1)
                 + " EQ " + juce::String (bandName) + " gain";
        }

        case MidiBindingTarget::MasterEqLfBoost:    return "Master EQ low boost";
        case MidiBindingTarget::MasterEqHfBoost:    return "Master EQ high boost";
        case MidiBindingTarget::MasterCompThresh:   return "Master comp threshold";
        case MidiBindingTarget::MasterCompMakeup:   return "Master comp makeup";
        case MidiBindingTarget::MasterCompRatio:    return "Master comp ratio";
    }
    return "(unknown target)";
}

const char* nameForTarget (MidiBindingTarget t) noexcept
{
    switch (t)
    {
        case MidiBindingTarget::None:            return "(none)";
        case MidiBindingTarget::TransportPlay:   return "Play";
        case MidiBindingTarget::TransportStop:   return "Stop";
        case MidiBindingTarget::TransportRecord: return "Record";
        case MidiBindingTarget::TransportToggle: return "Play/Stop toggle";
        case MidiBindingTarget::TrackFader:      return "Track fader";
        case MidiBindingTarget::TrackPan:        return "Track pan";
        case MidiBindingTarget::TrackMute:       return "Track mute";
        case MidiBindingTarget::TrackSolo:       return "Track solo";
        case MidiBindingTarget::TrackArm:        return "Track arm";
        case MidiBindingTarget::TrackAuxSend:    return "AUX send";
        case MidiBindingTarget::TrackHpfFreq:    return "HPF cutoff";
        case MidiBindingTarget::TrackEqGain:     return "EQ band gain";
        case MidiBindingTarget::TrackEqFreq:     return "EQ band frequency";
        case MidiBindingTarget::TrackEqQ:        return "EQ band Q";
        case MidiBindingTarget::TrackCompThresh: return "Comp threshold";
        case MidiBindingTarget::TrackCompMakeup: return "Comp makeup";
        case MidiBindingTarget::TrackPluginParam: return "Plugin parameter";
        case MidiBindingTarget::TrackFaderBank:       return "Track fader (banked)";
        case MidiBindingTarget::TrackPanBank:         return "Track pan (banked)";
        case MidiBindingTarget::TrackMuteBank:        return "Track mute (banked)";
        case MidiBindingTarget::TrackSoloBank:        return "Track solo (banked)";
        case MidiBindingTarget::TrackArmBank:         return "Track arm (banked)";
        case MidiBindingTarget::TrackAuxSendBank:     return "AUX send (banked)";
        case MidiBindingTarget::TrackHpfFreqBank:     return "HPF cutoff (banked)";
        case MidiBindingTarget::TrackEqGainBank:      return "EQ band gain (banked)";
        case MidiBindingTarget::TrackEqFreqBank:      return "EQ band frequency (banked)";
        case MidiBindingTarget::TrackEqQBank:         return "EQ band Q (banked)";
        case MidiBindingTarget::TrackCompThreshBank:  return "Comp threshold (banked)";
        case MidiBindingTarget::TrackCompMakeupBank:  return "Comp makeup (banked)";
        case MidiBindingTarget::TrackPluginParamBank: return "Plugin parameter (banked)";
        case MidiBindingTarget::BusFader:        return "Bus fader";
        case MidiBindingTarget::BusPan:          return "Bus pan";
        case MidiBindingTarget::BusMute:         return "Bus mute";
        case MidiBindingTarget::BusSolo:         return "Bus solo";
        case MidiBindingTarget::AuxLaneFader:    return "AUX return";
        case MidiBindingTarget::AuxLaneMute:     return "AUX mute";
        case MidiBindingTarget::AuxPluginParam:  return "AUX plugin parameter";
        case MidiBindingTarget::MasterFader:     return "Master fader";

        // Short category names for the bindings-panel section headers
        // + the right-click menu.
        case MidiBindingTarget::TrackEqEnabled:     return "Track EQ on/off";
        case MidiBindingTarget::TrackCompEnabled:   return "Track comp on/off";
        case MidiBindingTarget::TrackInsertBypass:  return "Track insert bypass";
        case MidiBindingTarget::TrackAuxSendPrePost: return "AUX send pre/post";
        case MidiBindingTarget::BusEqGain:          return "Bus EQ gain";
        case MidiBindingTarget::MasterEqLfBoost:    return "Master EQ low boost";
        case MidiBindingTarget::MasterEqHfBoost:    return "Master EQ high boost";
        case MidiBindingTarget::MasterCompThresh:   return "Master comp threshold";
        case MidiBindingTarget::MasterCompMakeup:   return "Master comp makeup";
        case MidiBindingTarget::MasterCompRatio:    return "Master comp ratio";
    }
    return "?";
}

namespace midilearn
{
namespace
{
// Set by the UI layer (setLearnMenuShowHook) to render the learn menu
// in-window. Null until then → showLearnMenu uses the native popup.
LearnMenuShowFn& learnMenuShowHook()
{
    static LearnMenuShowFn hook;
    return hook;
}

juce::String describeBinding (const MidiBinding& b)
{
    if (b.trigger == MidiBindingTrigger::MmcCommand)
    {
        static constexpr const char* names[] = {
            "?", "Stop", "Play", "DefPlay", "FFwd", "Rew",
            "RecStart", "RecStop", "?", "Pause"
        };
        const int cmd = b.dataNumber;
        const char* name = (cmd >= 1 && cmd <= 9) ? names[cmd] : "?";
        return juce::String ("MMC ") + name;
    }
    auto chStr = b.channel == 0 ? juce::String ("Ch -")
                                : "Ch " + juce::String (b.channel);
    if (b.trigger == MidiBindingTrigger::PitchBend)
        return chStr + " PitchBend";
    auto kindStr = b.trigger == MidiBindingTrigger::CC ? "CC " : "Note ";
    return chStr + " " + kindStr + juce::String (b.dataNumber);
}
} // namespace

// Resolves an absolute per-track binding (kind, index) to its bank-relative
// equivalent (target + position-encoded index). Returns false for targets
// that don't have a banked variant (Bus / Aux-lane / Master / Transport).
// Position-in-bank = trackIndex % kBankSize so the same physical
// controller slot drives whichever of the 8 tracks is bank-active.
static bool tryGetBankedVariant (MidiBindingTarget kind, int absIndex,
                                  MidiBindingTarget& outTarget, int& outIndex) noexcept
{
    auto pos = [] (int trk) { return trk % Session::kBankSize; };
    switch (kind)
    {
        case MidiBindingTarget::TrackFader:       outTarget = MidiBindingTarget::TrackFaderBank;      outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackPan:         outTarget = MidiBindingTarget::TrackPanBank;        outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackMute:        outTarget = MidiBindingTarget::TrackMuteBank;       outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackSolo:        outTarget = MidiBindingTarget::TrackSoloBank;       outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackArm:         outTarget = MidiBindingTarget::TrackArmBank;        outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackHpfFreq:     outTarget = MidiBindingTarget::TrackHpfFreqBank;    outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackCompThresh:  outTarget = MidiBindingTarget::TrackCompThreshBank; outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackCompMakeup:  outTarget = MidiBindingTarget::TrackCompMakeupBank; outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackPluginParam: outTarget = MidiBindingTarget::TrackPluginParamBank;outIndex = pos (absIndex); return true;
        case MidiBindingTarget::TrackAuxSend:
        {
            const int trk = unpackTrackAuxTrack (absIndex);
            const int aux = unpackTrackAuxLane  (absIndex);
            outTarget = MidiBindingTarget::TrackAuxSendBank;
            outIndex  = packTrackAux (pos (trk), aux);
            return true;
        }
        case MidiBindingTarget::TrackEqGain:
        case MidiBindingTarget::TrackEqFreq:
        case MidiBindingTarget::TrackEqQ:
        {
            const int trk  = unpackTrackEqTrack (absIndex);
            const int band = unpackTrackEqBand  (absIndex);
            outTarget = kind == MidiBindingTarget::TrackEqGain ? MidiBindingTarget::TrackEqGainBank
                      : kind == MidiBindingTarget::TrackEqFreq ? MidiBindingTarget::TrackEqFreqBank
                                                               : MidiBindingTarget::TrackEqQBank;
            outIndex  = packTrackEqBand (pos (trk), band);
            return true;
        }
        default: return false;
    }
}

// Discrete (toggle) targets — those that show "Button mode" in the
// learn menu so the user can switch a latching D-type controller from
// the default rising-edge fire to fire-on-every-message.
static bool isToggleTarget (MidiBindingTarget t) noexcept
{
    switch (t)
    {
        case MidiBindingTarget::TrackMute:
        case MidiBindingTarget::TrackSolo:
        case MidiBindingTarget::TrackArm:
        case MidiBindingTarget::TrackMuteBank:
        case MidiBindingTarget::TrackSoloBank:
        case MidiBindingTarget::TrackArmBank:
        case MidiBindingTarget::BusMute:
        case MidiBindingTarget::BusSolo:
        case MidiBindingTarget::AuxLaneMute:
        // Discrete on/off targets — latch-friendly, so the learn menu
        // offers Press/Toggle button modes for them.
        case MidiBindingTarget::TrackEqEnabled:
        case MidiBindingTarget::TrackCompEnabled:
        case MidiBindingTarget::TrackInsertBypass:
        case MidiBindingTarget::TrackAuxSendPrePost:
        // One-shot transport actions are idempotent (Play while
        // already playing is a no-op; same for Stop / Record), so
        // exposing Press/Toggle lets latching CC buttons on cheap
        // controllers fire each physical press. TransportToggle is
        // deliberately excluded: its "play if stopped, stop if
        // rolling" semantics would flip twice per click in Toggle
        // mode (127 then 0), netting a no-op.
        case MidiBindingTarget::TransportPlay:
        case MidiBindingTarget::TransportStop:
        case MidiBindingTarget::TransportRecord:
            return true;
        default:
            return false;
    }
}

void showLearnMenu (juce::Component& target,
                    Session& session,
                    MidiBindingTarget kind,
                    int index)
{
    // Resolve the bank-relative equivalent of this (kind, index) for the
    // optional banked MIDI Learn item. Empty / false when the target
    // isn't per-track (no banking for transport / bus / master).
    MidiBindingTarget bankedKind  = MidiBindingTarget::None;
    int               bankedIndex = -1;
    const bool        hasBanked   = tryGetBankedVariant (kind, index,
                                                            bankedKind, bankedIndex);

    // Find existing bindings — absolute first, then banked. Both can
    // coexist (user could bind two different controllers, one each).
    const MidiBinding* existingAbsolute = nullptr;
    const MidiBinding* existingBanked   = nullptr;
    for (const auto& b : session.midiBindings.current())
    {
        if (existingAbsolute == nullptr && b.target == kind && b.targetIndex == index)
            existingAbsolute = &b;
        if (hasBanked && existingBanked == nullptr
            && b.target == bankedKind && b.targetIndex == bankedIndex)
            existingBanked = &b;
        if (existingAbsolute != nullptr && (! hasBanked || existingBanked != nullptr))
            break;
    }
    const MidiBinding* existing = existingAbsolute != nullptr ? existingAbsolute
                                                              : existingBanked;

    juce::PopupMenu m;
    m.addSectionHeader (nameForTarget (kind));
    m.addItem ("MIDI Learn (this track)...", true, false,
        [&session, kind, index]
        {
            session.midiLearnPending.store (packLearnTarget (kind, index),
                                              std::memory_order_relaxed);
            session.midiLearnCapture.store (0, std::memory_order_relaxed);
        });
    if (hasBanked)
    {
        const int posDisplay =
            (bankedKind == MidiBindingTarget::TrackAuxSendBank)
                ? (bankedIndex / kPackedAuxLanes) + 1
            : (bankedKind == MidiBindingTarget::TrackEqGainBank
               || bankedKind == MidiBindingTarget::TrackEqFreqBank
               || bankedKind == MidiBindingTarget::TrackEqQBank)
                ? (bankedIndex / kPackedEqBands) + 1
                : bankedIndex + 1;
        const auto banklabel = "MIDI Learn (follow bank, position "
                              + juce::String (posDisplay) + ")...";
        m.addItem (banklabel, true, false,
            [&session, bankedKind, bankedIndex]
            {
                session.midiLearnPending.store (packLearnTarget (bankedKind, bankedIndex),
                                                  std::memory_order_relaxed);
                session.midiLearnCapture.store (0, std::memory_order_relaxed);
            });
    }
    auto addExistingControls = [&] (const MidiBinding* eb,
                                      MidiBindingTarget ek,
                                      int eidx,
                                      juce::String labelSuffix)
    {
        if (eb == nullptr) return;
        const auto boundLabel = "Bound" + labelSuffix + ": " + describeBinding (*eb);
        m.addItem (boundLabel, false, false, []{});

        // Button-mode submenu for discrete (toggle) targets. Lets the
        // user flip a binding from rising-edge (default — fires once on
        // press 127, releases on 0) to fire-on-every-message for
        // latching controllers whose physical press alternates 127/0
        // each click (e.g. Panorama T6 in some modes).
        if (isToggleTarget (ek))
        {
            const auto mode = eb->buttonMode;
            juce::PopupMenu modeMenu;
            modeMenu.addItem (juce::CharPointer_UTF8 ("Press (momentary 127 \xe2\x86\x92 0)"),
                                true, mode == MidiButtonMode::Press,
                [&session, ek, eidx]
                {
                    session.midiBindings.mutate ([ek, eidx] (std::vector<MidiBinding>& binds)
                    {
                        for (auto& x : binds)
                            if (x.target == ek && x.targetIndex == eidx)
                                x.buttonMode = MidiButtonMode::Press;
                    });
                });
            modeMenu.addItem (juce::CharPointer_UTF8 ("Toggle (latching 127 \xe2\x86\x94 0)"),
                                true, mode == MidiButtonMode::Toggle,
                [&session, ek, eidx]
                {
                    session.midiBindings.mutate ([ek, eidx] (std::vector<MidiBinding>& binds)
                    {
                        for (auto& x : binds)
                            if (x.target == ek && x.targetIndex == eidx)
                                x.buttonMode = MidiButtonMode::Toggle;
                    });
                });
            m.addSubMenu ("Button mode" + labelSuffix, modeMenu);
        }

        m.addItem ("Forget binding" + labelSuffix, true, false,
            [&session, ek, eidx]
            {
                session.midiBindings.mutate ([ek, eidx] (std::vector<MidiBinding>& binds)
                {
                    binds.erase (std::remove_if (binds.begin(), binds.end(),
                        [ek, eidx] (const MidiBinding& x)
                        {
                            return x.target == ek && x.targetIndex == eidx;
                        }), binds.end());
                });
            });
    };
    addExistingControls (existingAbsolute, kind,       index,       " (this track)");
    addExistingControls (existingBanked,   bankedKind, bankedIndex, " (banked)");
    (void) existing;   // kept for symmetry / future "any binding?" callers

    // The UI installs the in-window renderer at startup; a native popup must
    // never ship (Wayland policy), so a missing hook is a programming error.
    if (auto& hook = learnMenuShowHook())
        hook (m, target);
    else
        jassertfalse;
}

void setLearnMenuShowHook (LearnMenuShowFn fn)
{
    learnMenuShowHook() = std::move (fn);
}
} // namespace midilearn

juce::String serializeBindingsPreset (const std::vector<MidiBinding>& binds)
{
    // Preset wire format: { format_version: 1, bindings: [ {channel,
    // data, trigger, target, target_idx, param_idx}, ... ] }. Keeps
    // the per-entry shape identical to the session serializer's
    // embedded array so future code can share the same parser without
    // duplicating field names.
    auto* root = new juce::DynamicObject();
    root->setProperty ("format_version", 1);
    juce::Array<juce::var> arr;
    for (const auto& b : binds)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("channel",     b.channel);
        o->setProperty ("data",        b.dataNumber);
        o->setProperty ("trigger",     (int) b.trigger);
        o->setProperty ("target",      (int) b.target);
        o->setProperty ("target_idx",  b.targetIndex);
        o->setProperty ("param_idx",   b.paramIndex);
        o->setProperty ("button_mode", (int) b.buttonMode);
        arr.add (juce::var (o));
    }
    root->setProperty ("bindings", arr);
    return juce::JSON::toString (juce::var (root), false /*allOnOneLine*/);
}

std::optional<std::vector<MidiBinding>> deserializeBindingsPreset (const juce::String& json)
{
    // Parses the format produced by serializeBindingsPreset. Returns
    // nullopt on a malformed file / wrong schema; returns an empty
    // vector for a well-formed but intentionally empty preset (valid
    // "clear all bindings" preset). Per-entry isValid() filter drops
    // garbage entries without rejecting the whole preset.
    std::vector<MidiBinding> out;
    auto parsed = juce::JSON::parse (json);
    if (! parsed.isObject()) return std::nullopt;
    auto* root = parsed.getDynamicObject();
    if (root == nullptr) return std::nullopt;
    const auto arr = root->getProperty ("bindings");
    if (! arr.isArray()) return std::nullopt;
    for (int i = 0; i < arr.size(); ++i)
    {
        auto v = arr[i];
        if (! v.isObject()) continue;
        MidiBinding b;
        b.channel    = juce::jlimit (0, 16,
            v.hasProperty ("channel") ? (int) v["channel"] : 0);
        b.dataNumber = juce::jlimit (0, 127,
            v.hasProperty ("data") ? (int) v["data"] : 0);
        const int rawTrig = v.hasProperty ("trigger") ? (int) v["trigger"]
                                                      : (int) MidiBindingTrigger::CC;
        switch (rawTrig)
        {
            case (int) MidiBindingTrigger::Note:       b.trigger = MidiBindingTrigger::Note;       break;
            case (int) MidiBindingTrigger::PitchBend:  b.trigger = MidiBindingTrigger::PitchBend;  break;
            case (int) MidiBindingTrigger::MmcCommand: b.trigger = MidiBindingTrigger::MmcCommand; break;
            default:                                   b.trigger = MidiBindingTrigger::CC;         break;
        }
        const int rawTgt = v.hasProperty ("target") ? (int) v["target"]
                                                    : (int) MidiBindingTarget::None;
        // Reject unknown target ints up front so a malformed / forward-
        // version preset never injects an out-of-range enum into the
        // bindings vector (apply / describe switches have fallbacks but
        // an unknown target is dead weight either way).
        switch ((MidiBindingTarget) rawTgt)
        {
            case MidiBindingTarget::None:
            case MidiBindingTarget::TransportPlay:
            case MidiBindingTarget::TransportStop:
            case MidiBindingTarget::TransportRecord:
            case MidiBindingTarget::TransportToggle:
            case MidiBindingTarget::TrackFader:
            case MidiBindingTarget::TrackPan:
            case MidiBindingTarget::TrackMute:
            case MidiBindingTarget::TrackSolo:
            case MidiBindingTarget::TrackArm:
            case MidiBindingTarget::TrackAuxSend:
            case MidiBindingTarget::TrackHpfFreq:
            case MidiBindingTarget::TrackEqGain:
            case MidiBindingTarget::TrackEqFreq:
            case MidiBindingTarget::TrackEqQ:
            case MidiBindingTarget::TrackCompThresh:
            case MidiBindingTarget::TrackCompMakeup:
            case MidiBindingTarget::TrackPluginParam:
            case MidiBindingTarget::BusFader:
            case MidiBindingTarget::BusPan:
            case MidiBindingTarget::BusMute:
            case MidiBindingTarget::BusSolo:
            case MidiBindingTarget::AuxLaneFader:
            case MidiBindingTarget::AuxLaneMute:
            case MidiBindingTarget::AuxPluginParam:
            case MidiBindingTarget::MasterFader:
            // Bank-relative variants — were silently dropped, breaking preset
            // round-trip for any banked binding.
            case MidiBindingTarget::TrackFaderBank:
            case MidiBindingTarget::TrackPanBank:
            case MidiBindingTarget::TrackMuteBank:
            case MidiBindingTarget::TrackSoloBank:
            case MidiBindingTarget::TrackArmBank:
            case MidiBindingTarget::TrackAuxSendBank:
            case MidiBindingTarget::TrackHpfFreqBank:
            case MidiBindingTarget::TrackEqGainBank:
            case MidiBindingTarget::TrackEqFreqBank:
            case MidiBindingTarget::TrackEqQBank:
            case MidiBindingTarget::TrackCompThreshBank:
            case MidiBindingTarget::TrackCompMakeupBank:
            case MidiBindingTarget::TrackPluginParamBank:
            // Likewise missing from the round-trip whitelist.
            case MidiBindingTarget::TrackEqEnabled:
            case MidiBindingTarget::TrackCompEnabled:
            case MidiBindingTarget::TrackInsertBypass:
            case MidiBindingTarget::TrackAuxSendPrePost:
            case MidiBindingTarget::BusEqGain:
            case MidiBindingTarget::MasterEqLfBoost:
            case MidiBindingTarget::MasterEqHfBoost:
            case MidiBindingTarget::MasterCompThresh:
            case MidiBindingTarget::MasterCompMakeup:
            case MidiBindingTarget::MasterCompRatio:
                b.target = (MidiBindingTarget) rawTgt;
                break;
            default:
                continue; // skip this entry, unknown target
        }
        b.targetIndex = v.hasProperty ("target_idx") ? (int) v["target_idx"] : 0;
        b.paramIndex  = v.hasProperty ("param_idx")  ? (int) v["param_idx"]  : 0;
        if (v.hasProperty ("button_mode"))
        {
            const int bm = juce::jlimit (0, 1, (int) v["button_mode"]);
            b.buttonMode = (MidiButtonMode) bm;
        }
        if (b.isValid()) out.push_back (b);
    }
    return out;
}
} // namespace duskstudio
