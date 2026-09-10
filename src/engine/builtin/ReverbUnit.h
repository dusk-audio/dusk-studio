#pragma once

#include "BuiltinUnit.h"

#include "../../foundation/SmoothedValue.h"

#include <memory>
#include <vector>

class DuskVerbEngine;

namespace duskstudio::builtin
{
// Algorithmic reverb over the donor DuskVerb engine, which is framework-free
// C++17 and already exposes exactly this unit's contract: prepare(rate, block)
// then process(L, R, n) in place. The engine renders fully wet, so the dry/wet
// blend and its dry scratch live here.
//
// The engine runs at the strip's base rate. It has no oversampling of its own
// and nothing in it aliases hard enough to want any, so there is no factor to
// drive.
class ReverbUnit final : public BuiltinUnit
{
public:
    ReverbUnit();
    ~ReverbUnit() override;

    void prepare (double sampleRate, int maxBlockFrames) override;
    void process (float* left, float* right, int numFrames) noexcept override;

private:
    enum ParamIndex
    {
        kMix = 0, kAlgorithm, kDecaySeconds, kSize, kPreDelayMs,
        kDamping, kWidth, kLoCutHz, kHiCutHz, kNumParams
    };

    // Push only what moved: setAlgorithm clears every tank, and the filter
    // setters retarget smoothers the engine reads once per block.
    void pushChangedParams() noexcept;

    std::unique_ptr<DuskVerbEngine> engine;
    dusk::audio::SmoothedValue<float> mix { 0.0f };

    std::vector<float> dryL, dryR;
    int maxFrames = 0;

    int   lastAlgorithm = -1;
    float lastDecay = 0.0f, lastSize = -1.0f, lastPreDelay = -1.0f;
    float lastDamping = -1.0f, lastWidth = -1.0f;
    float lastLoCut = -1.0f, lastHiCut = -1.0f;
};
} // namespace duskstudio::builtin
