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
const char* const kModes[] =
{
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12",
};

const ParamInfo kDelayParams[] =
{
    { "echo",        "Echo",          "Mix",  "", 0.0f,  1.0f, 0.0f, ParamKind::Continuous },
    { "dry",         "Dry",           "Mix",  "", 0.0f,  1.0f, 1.0f, ParamKind::Continuous },
    { "reverb",      "Reverb",        "Mix",  "", 0.0f,  1.0f, 0.0f, ParamKind::Continuous },
    { "mode",        "Head Mode",     "Tape", "", 1.0f, 12.0f, 1.0f, ParamKind::Choice,
      kModes, (int) (sizeof (kModes) / sizeof (kModes[0])) },
    { "repeat_rate", "Repeat Rate",   "Tape", "", 0.0f,  1.0f, 0.5f, ParamKind::Continuous },
    { "intensity",   "Intensity",     "Tape", "", 0.0f,  1.0f, 0.4f, ParamKind::Continuous },
    { "input",       "Input",         "Tape", "", 0.0f,  1.0f, 0.5f, ParamKind::Continuous },
    { "wow_flutter", "Wow & Flutter", "Tape", "", 0.0f,  1.0f, 0.5f, ParamKind::Continuous },
    { "tape_age",    "Tape Age",      "Tape", "", 0.0f,  1.0f, 0.0f, ParamKind::Continuous },
    { "bass",        "Bass",          "Tone", "", -1.0f, 1.0f, 0.0f, ParamKind::Continuous },
    { "treble",      "Treble",        "Tone", "", -1.0f, 1.0f, 0.0f, ParamKind::Continuous },
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

void DelayUnit::process (float* left, float* right, int numFrames,
                         const dusk::MidiBuffer*) noexcept
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
