#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>

namespace duskstudio
{
// Aux-return plugin delay compensation. A latent plugin chain on an aux
// lane makes the wet return arrive L samples after the dry signal it was
// sent from; this class delays the dry/bus mix by max(L) and each lane's
// return by max(L) - L so everything re-converges sample-aligned before
// the master strip.
//
// Threading: setAuxLatency publishes an atomic target from any thread;
// processDryPath (audio thread, once per block, BEFORE any
// processAuxReturn) applies pending targets, calling setDelay only when
// a value actually changed. When max(L) == 0 every call is a bit-exact
// no-op so latency-free sessions pay nothing.
class LatencyCompensator
{
public:
    static constexpr int kNumLanes        = 4;
    static constexpr int kMaxDelaySamples = 16384;

    LatencyCompensator();

    // Message thread, audio stopped. Idempotent.
    void prepare (double sampleRate, int blockSize);

    // Any thread. Clamped to [0, kMaxDelaySamples].
    void setAuxLatency (int lane, int samples) noexcept;

    int  getMaxAuxLatency() const noexcept { return appliedMax; }
    bool isActive()         const noexcept { return appliedMax > 0; }

    // Audio-thread view of the extra delay processAuxReturn would apply
    // to this lane (max - lane latency). Valid after this block's
    // processDryPath. Lets the engine's silence-skip path know whether a
    // lane's delay line still needs feeding/draining.
    int getAuxReturnDelay (int lane) const noexcept;

    // Delays L/R in place by max(L). Applies pending latency targets
    // first, so this must run once per block before any processAuxReturn.
    void processDryPath (float* L, float* R, int numSamples) noexcept;

    // Delays the lane's return in place by max(L) - lane latency.
    void processAuxReturn (int lane, float* L, float* R, int numSamples) noexcept;

private:
    using DelayLine =
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>;

    std::array<std::atomic<int>, kNumLanes> targetLatency {};

    // Audio-thread-owned applied state (mutated only in applyTargets).
    std::array<int, kNumLanes> appliedLatency {};
    int appliedMax = 0;

    DelayLine dryDelay { kMaxDelaySamples };
    std::array<DelayLine, kNumLanes> auxDelay;

    void applyTargets() noexcept;
};
} // namespace duskstudio
