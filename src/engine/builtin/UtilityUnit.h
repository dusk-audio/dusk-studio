#pragma once

#include "BuiltinUnit.h"

#include "../../foundation/SmoothedValue.h"

namespace duskstudio::builtin
{
// Gain / polarity / stereo width / mono sum. Signal order is polarity ->
// width -> mono sum -> gain, so the width control is inert once the sum is on
// (the sum has already collapsed the side signal).
//
// At its defaults the unit is bit-exact unity: 0 dB, polarity off, 100 % width
// (mid/side reconstructs L and R exactly) and no mono sum.
class UtilityUnit final : public BuiltinUnit
{
public:
    UtilityUnit();

    void prepare (double sampleRate, int maxBlockFrames) override;
    void process (float* left, float* right, int numFrames) noexcept override;

private:
    enum ParamIndex { kGainDb = 0, kPolarityInvert, kWidth, kMonoSum, kNumParams };

    dusk::audio::SmoothedValue<float> gain { 1.0f };
    dusk::audio::SmoothedValue<float> width { 1.0f };
    dusk::audio::SmoothedValue<float> monoSum { 0.0f };
    dusk::audio::SmoothedValue<float> polarity { 1.0f };
};
} // namespace duskstudio::builtin
