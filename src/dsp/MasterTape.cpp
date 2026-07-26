#include "MasterTape.h"
#include "../session/Session.h"

#include <core/TapeMachineDSP.hpp>

#include <algorithm>

namespace duskstudio
{
struct MasterTape::Impl
{
    duskaudio::TapeMachineDSP core;
};

MasterTape::MasterTape() : impl (std::make_unique<Impl>()) {}
MasterTape::~MasterTape() = default;

void MasterTape::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    impl->core.setOversampling ((oversamplingFactor == 4) ? 2
                              : (oversamplingFactor == 2) ? 1
                                                          : 0);
    impl->core.prepare (sampleRate, std::max (1, blockSize));
    impl->core.reset();
}

int MasterTape::latencySamples() const noexcept
{
    return impl->core.latencySamples();
}

void MasterTape::pushParameters (const TapeParams& p) noexcept
{
    auto& c = impl->core;
    c.setTapeMachine  (p.machine.load      (std::memory_order_relaxed));
    c.setTapeSpeed    (p.speed.load        (std::memory_order_relaxed));
    c.setTapeType     (p.type.load         (std::memory_order_relaxed));
    c.setSignalPath   (p.signalPath.load   (std::memory_order_relaxed));
    c.setEqStandard   (p.eqStandard.load   (std::memory_order_relaxed));
    c.setCalibration  (p.calibration.load  (std::memory_order_relaxed));
    c.setInputGainDb  (p.inputGainDb.load  (std::memory_order_relaxed));
    c.setBias         (p.bias.load         (std::memory_order_relaxed));
    c.setHighpassHz   (p.highpassHz.load   (std::memory_order_relaxed));
    c.setLowpassHz    (p.lowpassHz.load    (std::memory_order_relaxed));
    c.setNoiseAmount  (p.noiseAmount.load  (std::memory_order_relaxed));
    c.setWow          (p.wow.load          (std::memory_order_relaxed));
    c.setFlutter      (p.flutter.load      (std::memory_order_relaxed));
    c.setOutputGainDb (p.outputGainDb.load (std::memory_order_relaxed));
    c.setAutoCal      (p.autoCal.load      (std::memory_order_relaxed));
    c.setAutoComp     (p.autoComp.load     (std::memory_order_relaxed));
}

void MasterTape::setBypass (bool shouldBypass) noexcept
{
    impl->core.setBypass (shouldBypass);
}

void MasterTape::processInPlace (float* L, float* R, int numSamples) noexcept
{
    // The core reads inputs[ch][n] before writing outputs[ch][n] at the same
    // index and guards its passthrough paths on inputs[ch] != outputs[ch], so
    // in-place is contractual.
    float* lr[2] = { L, R };
    impl->core.processBlock (lr, lr, 2, numSamples);
}

MasterTape::Vu MasterTape::getVu() const noexcept
{
    const auto& c = impl->core;
    return { c.getVuL(), c.getVuR(), c.getInVuL(), c.getInVuR() };
}
} // namespace duskstudio
