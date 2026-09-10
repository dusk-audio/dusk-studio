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
const ParamInfo kTapeParams[] =
{
    { "machine",     "Machine",      0.0f,     1.0f,     0.0f     },
    { "speed",       "Speed",        0.0f,     3.0f,     1.0f     },
    { "type",        "Tape",         0.0f,     3.0f,     0.0f     },
    { "signal_path", "Path",         0.0f,     3.0f,     0.0f     },
    { "eq_standard", "EQ",           0.0f,     1.0f,     0.0f     },
    { "input",       "Input",      -12.0f,    12.0f,     0.0f     },
    { "bias",        "Bias",         0.0f,   100.0f,    50.0f     },
    { "calibration", "Calibration",  0.0f,     3.0f,     0.0f     },
    { "output",      "Output",     -12.0f,    12.0f,     0.0f     },
    { "hpf",         "Low Cut",     20.0f,   500.0f,    20.0f     },
    { "lpf",         "High Cut",  3000.0f, 20000.0f, 20000.0f     },
    { "wow",         "Wow",          0.0f,   100.0f,     0.0f     },
    { "flutter",     "Flutter",      0.0f,   100.0f,     0.0f     },
    { "noise",       "Noise",        0.0f,   100.0f,     0.0f     },
    { "auto_cal",    "Auto Cal",     0.0f,     1.0f,     1.0f     },
    { "auto_comp",   "Auto Comp",    0.0f,     1.0f,     1.0f     },
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

void TapeUnit::process (float* left, float* right, int numFrames) noexcept
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
