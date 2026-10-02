#include "BuiltinRegistry.h"

#include "UtilityUnit.h"
#include "../../foundation/Json.h"

namespace duskstudio::builtin
{
namespace
{
#if DUSKSTUDIO_HAS_DAF_UNITS
// The knob tape unit's controls, which Tape Machine 2 took over under its id.
const std::vector<LegacyParam> kTapeKnobParams
{
    { "machine",     "tapeMachine" },
    { "speed",       "tapeSpeed" },
    { "type",        "tapeType" },
    { "signal_path", "signalPath" },
    { "eq_standard", "eqStandard" },
    { "input",       "inputGain" },
    { "bias",        "bias" },
    { "calibration", "calibration" },
    { "output",      "outputGain" },
    { "hpf",         "highpassFreq" },
    { "lpf",         "lowpassFreq" },
    { "wow",         "wowAmount" },
    { "flutter",     "flutterAmount" },
    { "noise",       "noiseAmount" },
    { "auto_cal",    "autoCal" },
    { "auto_comp",   "autoComp" },
};

// The knob synth unit's controls, which Sunset Circuits took over under its id.
// Its core cast the mode, the waves, the unison count and the semitone offset to
// int, and bent by the PB range as it stood.
const std::vector<LegacyParam> kSynthKnobParams
{
    { "mode",          "mode",         true },
    { "master_vol",    "masterVol" },
    { "master_tune",   "masterTune" },
    { "pb_range",      "pbRange" },
    { "portamento",    "portaTime" },
    { "unison_voices", "unisonVoices", true },
    { "unison_detune", "unisonDetune" },
    { "osc1_wave",     "osc1Wave",     true },
    { "osc1_level",    "osc1Level" },
    { "osc2_wave",     "osc2Wave",     true },
    { "osc2_level",    "osc2Level" },
    { "osc2_detune",   "osc2Detune" },
    { "osc2_semi",     "osc2Semi",     true },
    { "sub_level",     "subLevel" },
    { "noise_level",   "noiseLevel" },
    { "cutoff",        "filterCutoff" },
    { "resonance",     "filterRes" },
    { "filter_env",    "filterEnvAmt" },
    { "amp_attack",    "ampA" },
    { "amp_decay",     "ampD" },
    { "amp_sustain",   "ampS" },
    { "amp_release",   "ampR" },
    { "filt_attack",   "filtA" },
    { "filt_decay",    "filtD" },
    { "filt_sustain",  "filtS" },
    { "filt_release",  "filtR" },
};
#endif
} // namespace

const std::vector<UnitInfo>& registry()
{
    static const std::vector<UnitInfo> units
    {
        { "dusk.builtin.utility", "Utility", "Fx|Utility", false,
          [] () -> std::unique_ptr<BuiltinUnit> { return std::make_unique<UtilityUnit>(); } },
#if DUSKSTUDIO_HAS_DAF_UNITS
        { "dusk.builtin.reverb", "DuskVerb 2", "Fx|Reverb", false, nullptr, &createDuskVerb2 },
        { "dusk.builtin.delay", "Tape Echo 2", "Fx|Delay", false, nullptr, &createTapeEcho2 },
        { "dusk.builtin.tape", "Tape Machine 2", "Fx|Distortion", false, nullptr,
          &createTapeMachine2, &kTapeKnobParams },
        { "dusk.builtin.synth", "Sunset", "Instrument|Synth", true, nullptr, &createSunset,
          &kSynthKnobParams },
#endif
    };
    return units;
}

const UnitInfo* findUnit (const std::string& id)
{
    for (const auto& unit : registry())
        if (id == unit.id) return &unit;
    return nullptr;
}

std::unique_ptr<BuiltinUnit> createUnit (const std::string& id)
{
    const auto* unit = findUnit (id);
    return unit != nullptr && unit->create != nullptr ? unit->create() : nullptr;
}

std::vector<int> knobUnitParamIndices (const std::string& unitId,
                                       const std::vector<std::uint8_t>& state)
{
    const auto* unit = findUnit (unitId);
    if (unit == nullptr || unit->legacyParams == nullptr || unit->createPlugin == nullptr
        || state.empty())
        return {};

    const auto root = dusk::json::Json::parse (
        std::string (state.begin(), state.end()), nullptr, /*allow_exceptions*/ false);
    if (! root.is_object() || dusk::json::getString (root, "id") != unitId
        || dusk::json::getInt (root, "version", 0) != 1)
        return {};

    const auto plugin = unit->createPlugin();
    if (plugin == nullptr) return {};
    const auto& params = plugin->params();

    std::vector<int> indices;
    for (const auto& knob : *unit->legacyParams)
    {
        int index = -1;
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].symbol == knob.symbol)
                index = (int) i;
        indices.push_back (index);
    }
    return indices;
}
} // namespace duskstudio::builtin
