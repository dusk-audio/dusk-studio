#pragma once

#include "BuiltinUnit.h"

#include <memory>

namespace duskaudio { class TapeEchoDSP; }

namespace duskstudio::builtin
{
// Three-head tape echo with a spring tank, over the donor tape-echo core. The
// core is framework-free C++17 and its setters are relaxed atomic stores it
// snapshots once per block, so the whole parameter surface is a straight
// forward at the top of process().
//
// The core runs a fixed 4x oversampler on its FET preamp stage. It is not a
// factor this unit chooses: there is no hook to disable it, the group delay it
// costs is compensated inside the tape delay so the unit still reports zero
// latency, and it band-limits the preamp's asymmetric clipper, which would
// alias badly at base rate. It is not a duplicate of the strip's own
// oversampler either, which wraps EQ and comp downstream of the insert.
class DelayUnit final : public BuiltinUnit
{
public:
    DelayUnit();
    ~DelayUnit() override;

    void prepare (double sampleRate, int maxBlockFrames) override;
    void process (float* left, float* right, int numFrames,
                  const dusk::MidiBuffer* midi) noexcept override;

private:
    enum ParamIndex
    {
        kEchoLevel = 0, kDryLevel, kRepeatRate, kIntensity, kMode,
        kReverbLevel, kBass, kTreble, kInputGain, kWowFlutter, kTapeAge,
        kNumParams
    };

    std::unique_ptr<duskaudio::TapeEchoDSP> core;
    int maxFrames = 0;
};
} // namespace duskstudio::builtin
