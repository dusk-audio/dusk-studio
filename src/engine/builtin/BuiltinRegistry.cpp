#include "BuiltinRegistry.h"

#include "UtilityUnit.h"

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
const std::vector<LegacyParam> kSynthKnobParams
{
    { "mode",          "mode" },
    { "master_vol",    "masterVol" },
    { "master_tune",   "masterTune" },
    { "pb_range",      "pbRange" },
    { "portamento",    "portaTime" },
    { "unison_voices", "unisonVoices" },
    { "unison_detune", "unisonDetune" },
    { "osc1_wave",     "osc1Wave" },
    { "osc1_level",    "osc1Level" },
    { "osc2_wave",     "osc2Wave" },
    { "osc2_level",    "osc2Level" },
    { "osc2_detune",   "osc2Detune" },
    { "osc2_semi",     "osc2Semi" },
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
} // namespace duskstudio::builtin
