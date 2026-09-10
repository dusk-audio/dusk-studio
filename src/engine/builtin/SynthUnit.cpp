#include "SynthUnit.h"

#include "../../foundation/ScopedNoDenormals.h"

#include <MultiSynthDSP.hpp>

#include <algorithm>

namespace duskstudio::builtin
{
namespace
{
// Ranges and defaults are the donor's own parameter table, so a freshly loaded
// unit is its init patch.
const char* const kModes[] = { "Cosmos", "Oracle", "Mono", "Modular", "Prism", "Acid" };
const char* const kWaves[] = { "Saw", "Square", "Triangle", "Sine", "Noise" };

template <int N>
constexpr int countOf (const char* const (&)[N]) { return N; }

const ParamInfo kSynthParams[] =
{
    { "mode",          "Mode",          "Global", "",   0.0f,   5.0f,   0.0f,  ParamKind::Choice,
      kModes, countOf (kModes) },
    { "master_vol",    "Volume",        "Global", "dB", -60.0f, 6.0f,   0.0f,  ParamKind::Continuous },
    { "master_tune",   "Tune",          "Global", "ct", -100.0f, 100.0f, 0.0f, ParamKind::Continuous },
    { "pb_range",      "PB Range",      "Global", "st", 1.0f,  24.0f,   2.0f,  ParamKind::Continuous },
    { "portamento",    "Glide",         "Global", "s",  0.0f,   2.0f,   0.0f,  ParamKind::Continuous },
    { "unison_voices", "Unison",        "Global", "",   1.0f,   8.0f,   1.0f,  ParamKind::Continuous },
    { "unison_detune", "Unison Detune", "Global", "ct", 0.0f,  50.0f,  10.0f,  ParamKind::Continuous },

    { "osc1_wave",     "Osc 1 Wave",    "Oscillators", "", 0.0f, 4.0f, 0.0f, ParamKind::Choice,
      kWaves, countOf (kWaves) },
    { "osc1_level",    "Osc 1 Level",   "Oscillators", "", 0.0f, 1.0f, 1.0f, ParamKind::Continuous },
    { "osc2_wave",     "Osc 2 Wave",    "Oscillators", "", 0.0f, 4.0f, 0.0f, ParamKind::Choice,
      kWaves, countOf (kWaves) },
    { "osc2_level",    "Osc 2 Level",   "Oscillators", "",   0.0f,  1.0f, 0.8f, ParamKind::Continuous },
    { "osc2_detune",   "Osc 2 Detune",  "Oscillators", "ct", -50.0f, 50.0f, 7.0f, ParamKind::Continuous },
    { "osc2_semi",     "Osc 2 Semi",    "Oscillators", "st", -24.0f, 24.0f, 0.0f, ParamKind::Continuous },
    { "sub_level",     "Sub",           "Oscillators", "",   0.0f,  1.0f, 0.5f, ParamKind::Continuous },
    { "noise_level",   "Noise",         "Oscillators", "",   0.0f,  1.0f, 0.0f, ParamKind::Continuous },

    { "cutoff",        "Cutoff",        "Filter", "Hz", 20.0f, 20000.0f, 8000.0f, ParamKind::Continuous },
    { "resonance",     "Resonance",     "Filter", "",    0.0f,     1.0f,    0.3f, ParamKind::Continuous },
    { "filter_env",    "Filter Env",    "Filter", "",   -1.0f,     1.0f,    0.5f, ParamKind::Continuous },

    { "amp_attack",    "Amp A",         "Envelopes", "s", 0.001f, 10.0f, 0.01f, ParamKind::Continuous },
    { "amp_decay",     "Amp D",         "Envelopes", "s", 0.001f, 10.0f, 0.2f,  ParamKind::Continuous },
    { "amp_sustain",   "Amp S",         "Envelopes", "",  0.0f,    1.0f, 0.8f,  ParamKind::Continuous },
    { "amp_release",   "Amp R",         "Envelopes", "s", 0.001f, 10.0f, 0.3f,  ParamKind::Continuous },
    { "filt_attack",   "Filt A",        "Envelopes", "s", 0.001f, 10.0f, 0.01f, ParamKind::Continuous },
    { "filt_decay",    "Filt D",        "Envelopes", "s", 0.001f, 10.0f, 0.3f,  ParamKind::Continuous },
    { "filt_sustain",  "Filt S",        "Envelopes", "",  0.0f,    1.0f, 0.4f,  ParamKind::Continuous },
    { "filt_release",  "Filt R",        "Envelopes", "s", 0.001f, 10.0f, 0.5f,  ParamKind::Continuous },
};

// The unit's parameter order onto the core's flat index.
constexpr int kCoreIndex[] =
{
    msynth::pMode, msynth::pMasterVol, msynth::pMasterTune, msynth::pPbRange,
    msynth::pPortaTime, msynth::pUnisonVoices, msynth::pUnisonDetune,
    msynth::pOsc1Wave, msynth::pOsc1Level, msynth::pOsc2Wave, msynth::pOsc2Level,
    msynth::pOsc2Detune, msynth::pOsc2Semi, msynth::pSubLevel, msynth::pNoiseLevel,
    msynth::pFilterCutoff, msynth::pFilterRes, msynth::pFilterEnvAmt,
    msynth::pAmpA, msynth::pAmpD, msynth::pAmpS, msynth::pAmpR,
    msynth::pFiltA, msynth::pFiltD, msynth::pFiltS, msynth::pFiltR,
};
static_assert (sizeof (kCoreIndex) / sizeof (kCoreIndex[0])
                   == sizeof (kSynthParams) / sizeof (kSynthParams[0]),
               "every exposed parameter needs its core index");
} // namespace

SynthUnit::SynthUnit()
    : BuiltinUnit (kSynthParams, kNumParams),
      core (std::make_unique<msynth::MultiSynthDSP>())
{
}

SynthUnit::~SynthUnit() = default;

void SynthUnit::prepare (double sampleRate, int maxBlockFrames)
{
    maxFrames = std::max (0, maxBlockFrames);
    core->prepare (sampleRate, maxFrames);
    core->reset();
}

void SynthUnit::applyMidi (const dusk::MidiBuffer& midi) noexcept
{
    for (const auto meta : midi)
    {
        const auto* d = meta.data;
        if (d == nullptr || meta.numBytes < 2) continue;
        const int d1 = d[1] & 0x7F;
        const int d2 = meta.numBytes > 2 ? (d[2] & 0x7F) : 0;

        switch (d[0] & 0xF0)
        {
            case 0x90:
                if (d2 > 0)
                {
                    core->noteOn (d1, (float) d2 / 127.0f);
                    break;
                }
                // Note-on at velocity 0 is a note-off.
                [[fallthrough]];
            case 0x80:
                core->noteOff (d1);
                break;
            case 0xA0:
                core->polyAftertouch (d1, (float) d2 / 127.0f);
                break;
            case 0xD0:
                core->aftertouch ((float) d1 / 127.0f);
                break;
            case 0xE0:
                // 14-bit wheel, 0-centred and normalised to -1..1.
                core->pitchBend ((float) (((d2 << 7) | d1) - 8192) / 8192.0f);
                break;
            case 0xB0:
                switch (d1)
                {
                    case 1:   core->modWheel ((float) d2 / 127.0f); break;
                    case 64:  core->sustainPedal (d2 >= 64); break;
                    case 120: core->allSoundOff(); break;
                    case 123: core->allNotesOff(); break;
                    default: break;
                }
                break;
            default:
                break;
        }
    }
}

void SynthUnit::process (float* left, float* right, int numFrames,
                         const dusk::MidiBuffer* midi) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (left == nullptr || right == nullptr || numFrames <= 0 || numFrames > maxFrames)
        return;

    for (int i = 0; i < kNumParams; ++i)
        core->setParameter (kCoreIndex[i], paramValue (i));

    if (midi != nullptr)
        applyMidi (*midi);

    core->processBlock (left, right, numFrames);
}
} // namespace duskstudio::builtin
