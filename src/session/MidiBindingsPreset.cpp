#include "MidiBindings.h"

#include "../foundation/Json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace duskstudio
{
namespace
{
constexpr int kPresetFormatVersion = 1;
}

std::string serializeBindingsPreset (const std::vector<MidiBinding>& binds)
{
    // Preset wire format: { format_version: 1, bindings: [ {channel,
    // data, trigger, target, target_idx, param_idx}, ... ] }. Keeps
    // the per-entry shape identical to the session serializer's
    // embedded array so future code can share the same parser without
    // duplicating field names.
    nlohmann::json root;
    root["format_version"] = kPresetFormatVersion;
    auto arr = nlohmann::json::array();
    for (const auto& b : binds)
    {
        nlohmann::json o;
        o["channel"]     = b.channel;
        o["data"]        = b.dataNumber;
        o["trigger"]     = (int) b.trigger;
        o["target"]      = (int) b.target;
        o["target_idx"]  = b.targetIndex;
        o["param_idx"]   = b.paramIndex;
        o["button_mode"] = (int) b.buttonMode;
        arr.push_back (std::move (o));
    }
    root["bindings"] = std::move (arr);
    return root.dump (2);
}

std::optional<std::vector<MidiBinding>> deserializeBindingsPreset (const std::string& json)
{
    // Parses the format produced by serializeBindingsPreset. Returns
    // nullopt on a malformed file / wrong schema; returns an empty
    // vector for a well-formed but intentionally empty preset (valid
    // "clear all bindings" preset). Per-entry isValid() filter drops
    // garbage entries without rejecting the whole preset.
    std::vector<MidiBinding> out;
    const auto parsed = nlohmann::json::parse (json, nullptr, false);
    if (! parsed.is_object()) return std::nullopt;
    // A file from a later version can carry the same key names with different
    // meanings; parsing it as far as it happens to fit would import bindings
    // the user never made. Only the version this build writes is read.
    if (! parsed.contains ("format_version") || ! parsed["format_version"].is_number_integer()
        || parsed["format_version"].get<int>() != kPresetFormatVersion)
        return std::nullopt;
    if (! parsed.contains ("bindings") || ! parsed["bindings"].is_array()) return std::nullopt;

    for (const auto& v : parsed["bindings"])
    {
        if (! v.is_object()) continue;
        MidiBinding b;
        b.channel    = std::clamp (dusk::json::getInt (v, "channel", 0), 0, 16);
        b.dataNumber = std::clamp (dusk::json::getInt (v, "data", 0), 0, 127);
        const int rawTrig = dusk::json::getInt (v, "trigger", (int) MidiBindingTrigger::CC);
        switch (rawTrig)
        {
            case (int) MidiBindingTrigger::Note:       b.trigger = MidiBindingTrigger::Note;       break;
            case (int) MidiBindingTrigger::PitchBend:  b.trigger = MidiBindingTrigger::PitchBend;  break;
            case (int) MidiBindingTrigger::MmcCommand: b.trigger = MidiBindingTrigger::MmcCommand; break;
            default:                                   b.trigger = MidiBindingTrigger::CC;         break;
        }
        const int rawTgt = dusk::json::getInt (v, "target", (int) MidiBindingTarget::None);
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
            // Bank-relative variants - were silently dropped, breaking preset
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
        b.targetIndex = dusk::json::getInt (v, "target_idx", 0);
        b.paramIndex  = dusk::json::getInt (v, "param_idx", 0);
        if (v.contains ("button_mode"))
            b.buttonMode = (MidiButtonMode) std::clamp (dusk::json::getInt (v, "button_mode", 0), 0, 1);
        if (b.isValid()) out.push_back (b);
    }
    return out;
}
} // namespace duskstudio
