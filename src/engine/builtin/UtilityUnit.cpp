#include "UtilityUnit.h"

#include "../../foundation/Decibels.h"
#include "../../foundation/ScopedNoDenormals.h"

#include <algorithm>

namespace duskstudio::builtin
{
namespace
{
constexpr float kMinGainDb = -60.0f;

const ParamInfo kUtilityParams[] =
{
    { "gain_db",  "Gain",     "dB", kMinGainDb, 24.0f,  0.0f,   false },
    { "polarity", "Polarity", "",   0.0f,       1.0f,   0.0f,   true  },
    { "width",    "Width",    "%",  0.0f,       200.0f, 100.0f, false },
    { "mono",     "Mono",     "",   0.0f,       1.0f,   0.0f,   true  },
};

constexpr double kRampSeconds = 0.02;
} // namespace

UtilityUnit::UtilityUnit()
    : BuiltinUnit (kUtilityParams, kNumParams)
{
}

void UtilityUnit::prepare (double sampleRate, int)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    gain    .reset (sr, kRampSeconds);
    width   .reset (sr, kRampSeconds);
    monoSum .reset (sr, kRampSeconds);
    polarity.reset (sr, kRampSeconds);

    const float db = getParam (kGainDb);
    gain.setCurrentAndTargetValue (db <= kMinGainDb ? 0.0f : dusk::audio::decibelsToGain (db));
    width.setCurrentAndTargetValue (getParam (kWidth) * 0.01f);
    monoSum.setCurrentAndTargetValue (getParam (kMonoSum) >= 0.5f ? 1.0f : 0.0f);
    polarity.setCurrentAndTargetValue (getParam (kPolarityInvert) >= 0.5f ? -1.0f : 1.0f);
}

void UtilityUnit::process (float* left, float* right, int numFrames) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (left == nullptr || right == nullptr || numFrames <= 0)
        return;

    const float db = paramValue (kGainDb);
    gain    .setTargetValue (db <= kMinGainDb ? 0.0f : dusk::audio::decibelsToGain (db));
    width   .setTargetValue (paramValue (kWidth) * 0.01f);
    monoSum .setTargetValue (paramValue (kMonoSum) >= 0.5f ? 1.0f : 0.0f);
    polarity.setTargetValue (paramValue (kPolarityInvert) >= 0.5f ? -1.0f : 1.0f);

    for (int i = 0; i < numFrames; ++i)
    {
        const float pol = polarity.getNextValue();
        const float w   = width.getNextValue();
        const float m   = monoSum.getNextValue();
        const float g   = gain.getNextValue();

        const float l = left[i] * pol;
        const float r = right[i] * pol;

        // Written as a two-tap matrix rather than mid +/- side so unity width
        // is bit-exact passthrough: the cross coefficient is exactly zero.
        const float direct = 0.5f * (1.0f + w);
        const float cross  = 0.5f * (1.0f - w);
        const float wideL = l * direct + r * cross;
        const float wideR = r * direct + l * cross;
        const float mid   = 0.5f * (l + r);

        left[i]  = ((1.0f - m) * wideL + m * mid) * g;
        right[i] = ((1.0f - m) * wideR + m * mid) * g;
    }
}
} // namespace duskstudio::builtin
