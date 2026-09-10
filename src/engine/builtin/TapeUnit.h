#pragma once

#include "BuiltinUnit.h"

#include <memory>

namespace duskaudio { class TapeMachineDSP; }

namespace duskstudio::builtin
{
// Per-channel tape colour over the donor Tape Machine 2 core, the same core the
// master bus runs. Framework-free C++17; its setters are relaxed atomic stores
// it snapshots once per block.
//
// The core anti-aliases locally around each nonlinearity and its own
// factorFromChoice is hardwired to 2x, so its oversampling parameter is inert
// by the donor's design. This unit therefore chooses nothing: there is no
// factor for the engine to drive here, and no internal oversampling for it to
// switch on.
class TapeUnit final : public BuiltinUnit
{
public:
    TapeUnit();
    ~TapeUnit() override;

    void prepare (double sampleRate, int maxBlockFrames) override;
    int  latencySamples() const noexcept override;
    void process (float* left, float* right, int numFrames) noexcept override;

private:
    enum ParamIndex
    {
        kMachine = 0, kSpeed, kType, kSignalPath, kEqStandard,
        kInputGainDb, kBias, kCalibration, kOutputGainDb,
        kHighpassHz, kLowpassHz, kWow, kFlutter, kNoise,
        kAutoCal, kAutoComp, kNumParams
    };

    static constexpr int kSignalPathThru = 3;

    std::unique_ptr<duskaudio::TapeMachineDSP> core;
    int maxFrames = 0;
};
} // namespace duskstudio::builtin
