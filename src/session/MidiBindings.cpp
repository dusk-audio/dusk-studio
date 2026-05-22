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
        case MidiBindingTarget::BusFader:        return "Bus " + trk() + " fader";
        case MidiBindingTarget::BusPan:          return "Bus " + trk() + " pan";
        case MidiBindingTarget::BusMute:         return "Bus " + trk() + " mute";
        case MidiBindingTarget::BusSolo:         return "Bus " + trk() + " solo";
        case MidiBindingTarget::AuxLaneFader:    return "AUX " + trk() + " return";
        case MidiBindingTarget::AuxLaneMute:     return "AUX " + trk() + " mute";
        case MidiBindingTarget::MasterFader:     return "Master fader";
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
        case MidiBindingTarget::TrackCompThresh: return "Comp threshold";
        case MidiBindingTarget::TrackCompMakeup: return "Comp makeup";
        case MidiBindingTarget::TrackPluginParam: return "Plugin parameter";
        case MidiBindingTarget::BusFader:        return "Bus fader";
        case MidiBindingTarget::BusPan:          return "Bus pan";
        case MidiBindingTarget::BusMute:         return "Bus mute";
        case MidiBindingTarget::BusSolo:         return "Bus solo";
        case MidiBindingTarget::AuxLaneFader:    return "AUX return";
        case MidiBindingTarget::AuxLaneMute:     return "AUX mute";
        case MidiBindingTarget::MasterFader:     return "Master fader";
    }
    return "?";
}

namespace midilearn
{
namespace
{
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
        case MidiBindingTarget::BusMute:
        case MidiBindingTarget::BusSolo:
        case MidiBindingTarget::AuxLaneMute:
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
    // Find an existing binding for this (kind, index) pair so the menu
    // can show its source and offer "Forget".
    const MidiBinding* existing = nullptr;
    for (const auto& b : session.midiBindings.current())
    {
        if (b.target == kind && b.targetIndex == index) { existing = &b; break; }
    }

    juce::PopupMenu m;
    m.addSectionHeader (nameForTarget (kind));
    m.addItem ("MIDI Learn...", true, false,
        [&session, kind, index]
        {
            session.midiLearnPending.store (packLearnTarget (kind, index),
                                              std::memory_order_relaxed);
            session.midiLearnCapture.store (0, std::memory_order_relaxed);
        });
    if (existing != nullptr)
    {
        const auto label = "Bound: " + describeBinding (*existing);
        m.addItem (label, false, false, []{});

        // Button-mode submenu for discrete (toggle) targets. Lets the
        // user flip a binding from rising-edge (default — fires once on
        // press 127, releases on 0) to fire-on-every-message for
        // latching controllers whose physical press alternates 127/0
        // each click (e.g. Panorama T6 in some modes).
        if (isToggleTarget (kind))
        {
            const auto mode = existing->buttonMode;
            juce::PopupMenu modeMenu;
            modeMenu.addItem ("Press (momentary 127 → 0)",
                                true, mode == MidiButtonMode::Press,
                [&session, kind, index]
                {
                    session.midiBindings.mutate ([kind, index] (std::vector<MidiBinding>& binds)
                    {
                        for (auto& x : binds)
                            if (x.target == kind && x.targetIndex == index)
                                x.buttonMode = MidiButtonMode::Press;
                    });
                });
            modeMenu.addItem ("Toggle (latching 127 ↔ 0)",
                                true, mode == MidiButtonMode::Toggle,
                [&session, kind, index]
                {
                    session.midiBindings.mutate ([kind, index] (std::vector<MidiBinding>& binds)
                    {
                        for (auto& x : binds)
                            if (x.target == kind && x.targetIndex == index)
                                x.buttonMode = MidiButtonMode::Toggle;
                    });
                });
            m.addSubMenu ("Button mode", modeMenu);
        }

        m.addItem ("Forget binding", true, false,
            [&session, kind, index]
            {
                session.midiBindings.mutate ([kind, index] (std::vector<MidiBinding>& binds)
                {
                    binds.erase (std::remove_if (binds.begin(), binds.end(),
                        [kind, index] (const MidiBinding& x)
                        {
                            return x.target == kind && x.targetIndex == index;
                        }), binds.end());
                });
            });
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
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

std::vector<MidiBinding> deserializeBindingsPreset (const juce::String& json)
{
    // Parses the format produced by serializeBindingsPreset. Returns
    // empty on a malformed file or version mismatch - the caller surfaces
    // the failure to the user. Per-entry isValid() filter drops garbage
    // entries without rejecting the whole preset.
    std::vector<MidiBinding> out;
    auto parsed = juce::JSON::parse (json);
    if (! parsed.isObject()) return out;
    auto* root = parsed.getDynamicObject();
    if (root == nullptr) return out;
    const auto arr = root->getProperty ("bindings");
    if (! arr.isArray()) return out;
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
            case MidiBindingTarget::TrackCompThresh:
            case MidiBindingTarget::TrackCompMakeup:
            case MidiBindingTarget::TrackPluginParam:
            case MidiBindingTarget::BusFader:
            case MidiBindingTarget::BusPan:
            case MidiBindingTarget::BusMute:
            case MidiBindingTarget::BusSolo:
            case MidiBindingTarget::AuxLaneFader:
            case MidiBindingTarget::AuxLaneMute:
            case MidiBindingTarget::MasterFader:
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
