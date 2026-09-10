#include "TapeUnit.h"

#include "../../foundation/ScopedNoDenormals.h"

#include <core/TapeMachineDSP.hpp>

#include <algorithm>

namespace duskstudio::builtin
{
namespace
{
// The subset of the donor's parameter table a channel insert wants. Ranges and
// choice counts are the donor's; the advanced repro-head and program-band trims
// are left at their defaults.
const char* const kMachines[]    = { "Swiss", "American" };
const char* const kSpeeds[]      = { "7.5 IPS", "15 IPS", "30 IPS", "3.75 IPS" };
const char* const kTapes[]       = { "456", "GP9", "900", "250" };
const char* const kPaths[]       = { "Repro", "Sync", "Input", "Thru" };
const char* const kEqStandards[] = { "NAB", "CCIR" };
const char* const kCalibrations[]= { "+3 dB", "+6 dB", "+7.5 dB", "+9 dB" };

template <int N>
constexpr int countOf (const char* const (&)[N]) { return N; }

const ParamInfo kTapeParams[] =
{
    { "machine",     "Machine",     "Transport", "",   0.0f,  1.0f, 0.0f, ParamKind::Choice,
      kMachines, countOf (kMachines) },
    { "speed",       "Speed",       "Transport", "",   0.0f,  3.0f, 1.0f, ParamKind::Choice,
      kSpeeds, countOf (kSpeeds) },
    { "type",        "Tape",        "Transport", "",   0.0f,  3.0f, 0.0f, ParamKind::Choice,
      kTapes, countOf (kTapes) },
    { "signal_path", "Path",        "Transport", "",   0.0f,  3.0f, 0.0f, ParamKind::Choice,
      kPaths, countOf (kPaths) },
    { "eq_standard", "EQ",          "Transport", "",   0.0f,  1.0f, 0.0f, ParamKind::Choice,
      kEqStandards, countOf (kEqStandards) },
    { "input",       "Input",       "Level",     "dB", -12.0f, 12.0f, 0.0f, ParamKind::Continuous },
    { "bias",        "Bias",        "Level",     "%",   0.0f, 100.0f, 50.0f, ParamKind::Continuous },
    { "calibration", "Calibration", "Level",     "",    0.0f,  3.0f,  0.0f, ParamKind::Choice,
      kCalibrations, countOf (kCalibrations) },
    { "output",      "Output",      "Level",     "dB", -12.0f, 12.0f, 0.0f, ParamKind::Continuous },
    { "hpf",         "Low Cut",     "Tone",      "Hz",  20.0f, 500.0f, 20.0f, ParamKind::Continuous },
    { "lpf",         "High Cut",    "Tone",      "Hz", 3000.0f, 20000.0f, 20000.0f, ParamKind::Continuous },
    { "wow",         "Wow",         "Transport Noise", "%", 0.0f, 100.0f, 0.0f, ParamKind::Continuous },
    { "flutter",     "Flutter",     "Transport Noise", "%", 0.0f, 100.0f, 0.0f, ParamKind::Continuous },
    { "noise",       "Noise",       "Transport Noise", "%", 0.0f, 100.0f, 0.0f, ParamKind::Continuous },
    { "auto_cal",    "Auto Cal",    "Transport Noise", "",  0.0f, 1.0f, 1.0f, ParamKind::Toggle },
    { "auto_comp",   "Auto Comp",   "Transport Noise", "",  0.0f, 1.0f, 1.0f, ParamKind::Toggle },
};

int choice (float v) noexcept { return (int) (v + 0.5f); }
} // namespace

TapeUnit::TapeUnit()
    : BuiltinUnit (kTapeParams, kNumParams),
      core (std::make_unique<duskaudio::TapeMachineDSP>())
{
}

TapeUnit::~TapeUnit() = default;

void TapeUnit::prepare (double sampleRate, int maxBlockFrames)
{
    maxFrames = std::max (0, maxBlockFrames);
    core->prepare (sampleRate, maxFrames);
    core->reset();
}

int TapeUnit::latencySamples() const noexcept
{
    // The core reports its filter round trip unconditionally, but the Thru path
    // is a sample-exact passthrough that does not take it. Reporting the core's
    // figure there would have plugin delay compensation shift every other track
    // to match a delay this insert is not adding.
    if (choice (getParam (kSignalPath)) == kSignalPathThru) return 0;
    return core != nullptr ? core->latencySamples() : 0;
}

void TapeUnit::process (float* left, float* right, int numFrames,
                        const dusk::MidiBuffer*) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (left == nullptr || right == nullptr || numFrames <= 0 || numFrames > maxFrames)
        return;

    core->setTapeMachine  (choice (paramValue (kMachine)));
    core->setTapeSpeed    (choice (paramValue (kSpeed)));
    core->setTapeType     (choice (paramValue (kType)));
    core->setSignalPath   (choice (paramValue (kSignalPath)));
    core->setEqStandard   (choice (paramValue (kEqStandard)));
    core->setInputGainDb  (paramValue (kInputGainDb));
    core->setBias         (paramValue (kBias));
    core->setCalibration  (choice (paramValue (kCalibration)));
    core->setOutputGainDb (paramValue (kOutputGainDb));
    core->setHighpassHz   (paramValue (kHighpassHz));
    core->setLowpassHz    (paramValue (kLowpassHz));
    core->setWow          (paramValue (kWow));
    core->setFlutter      (paramValue (kFlutter));
    core->setNoiseAmount  (paramValue (kNoise));
    core->setAutoCal      (paramValue (kAutoCal) >= 0.5f);
    core->setAutoComp     (paramValue (kAutoComp) >= 0.5f);

    float* io[2] = { left, right };
    core->processBlock (io, io, 2, numFrames);
}
} // namespace duskstudio::builtin
