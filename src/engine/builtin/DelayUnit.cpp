#include "DelayUnit.h"

#include "../../foundation/ScopedNoDenormals.h"

#include <TapeEchoDSP.hpp>

#include <algorithm>

namespace duskstudio::builtin
{
namespace
{
// Ranges and defaults are the donor's own parameter table, except that Echo and
// Reverb default to zero: an insert is transparent until the user asks for
// something, the same rule the Reverb unit's Mix follows.
const ParamInfo kDelayParams[] =
{
    { "echo",        "Echo",          0.0f,  1.0f,  0.0f },
    { "dry",         "Dry",           0.0f,  1.0f,  1.0f },
    { "repeat_rate", "Repeat Rate",   0.0f,  1.0f,  0.5f },
    { "intensity",   "Intensity",     0.0f,  1.0f,  0.4f },
    { "mode",        "Mode",          1.0f, 12.0f,  1.0f },
    { "reverb",      "Reverb",        0.0f,  1.0f,  0.0f },
    { "bass",        "Bass",         -1.0f,  1.0f,  0.0f },
    { "treble",      "Treble",       -1.0f,  1.0f,  0.0f },
    { "input",       "Input",         0.0f,  1.0f,  0.5f },
    { "wow_flutter", "Wow & Flutter", 0.0f,  1.0f,  0.5f },
    { "tape_age",    "Tape Age",      0.0f,  1.0f,  0.0f },
};
} // namespace

DelayUnit::DelayUnit()
    : BuiltinUnit (kDelayParams, kNumParams),
      core (std::make_unique<duskaudio::TapeEchoDSP>())
{
}

DelayUnit::~DelayUnit() = default;

void DelayUnit::prepare (double sampleRate, int maxBlockFrames)
{
    maxFrames = std::max (0, maxBlockFrames);
    core->prepare (sampleRate, maxFrames);
    core->reset();
}

void DelayUnit::process (float* left, float* right, int numFrames) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (left == nullptr || right == nullptr || numFrames <= 0 || numFrames > maxFrames)
        return;

    core->setEchoLevel   (paramValue (kEchoLevel));
    core->setDryLevel    (paramValue (kDryLevel));
    core->setRepeatRate  (paramValue (kRepeatRate));
    core->setIntensity   (paramValue (kIntensity));
    core->setMode        ((int) (paramValue (kMode) + 0.5f));
    core->setReverbLevel (paramValue (kReverbLevel));
    core->setBass        (paramValue (kBass));
    core->setTreble      (paramValue (kTreble));
    core->setInputGain   (paramValue (kInputGain));
    core->setWowFlutter  (paramValue (kWowFlutter));
    core->setTapeAge     (paramValue (kTapeAge));

    float* io[2] = { left, right };
    core->processBlock (io, io, 2, numFrames);
}
} // namespace duskstudio::builtin
