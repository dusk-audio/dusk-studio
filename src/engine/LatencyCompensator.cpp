#include "LatencyCompensator.h"

namespace duskstudio
{
LatencyCompensator::LatencyCompensator()
{
    for (auto& t : targetLatency) t.store (0, std::memory_order_relaxed);
    // std::array default-constructs the lane lines with a 0-sample
    // maximum, which silently clamps setDelay to nothing.
    for (auto& d : auxDelay) d.setMaximumDelayInSamples (kMaxDelaySamples);
}

void LatencyCompensator::prepare (double sampleRate, int blockSize)
{
    const juce::dsp::ProcessSpec spec { sampleRate,
                                        (juce::uint32) juce::jmax (1, blockSize),
                                        2 };
    dryDelay.prepare (spec);
    dryDelay.setDelay (0.0f);
    for (auto& d : auxDelay)
    {
        d.prepare (spec);
        d.setDelay (0.0f);
    }

    appliedLatency.fill (0);
    appliedMax = 0;
}

void LatencyCompensator::setAuxLatency (int lane, int samples) noexcept
{
    if (lane < 0 || lane >= kNumLanes) return;
    targetLatency[(size_t) lane].store (juce::jlimit (0, kMaxDelaySamples, samples),
                                        std::memory_order_relaxed);
}

int LatencyCompensator::getAuxReturnDelay (int lane) const noexcept
{
    if (lane < 0 || lane >= kNumLanes) return 0;
    return appliedMax - appliedLatency[(size_t) lane];
}

void LatencyCompensator::applyTargets() noexcept
{
    bool changed = false;
    int  maxLat  = 0;
    for (size_t l = 0; l < (size_t) kNumLanes; ++l)
    {
        const int t = targetLatency[l].load (std::memory_order_relaxed);
        if (t != appliedLatency[l])
        {
            appliedLatency[l] = t;
            changed = true;
        }
        maxLat = juce::jmax (maxLat, t);
    }
    if (! changed) return;

    // Re-engaging after a fully-bypassed stretch: the lines weren't fed
    // while inactive, so their buffers hold stale audio from before the
    // bypass. Zero them (bounded fill, no allocation) so the first
    // compensated blocks emit silence instead of an old snippet.
    if (appliedMax == 0 && maxLat > 0)
    {
        dryDelay.reset();
        for (auto& d : auxDelay) d.reset();
    }

    appliedMax = maxLat;
    dryDelay.setDelay ((float) appliedMax);
    for (size_t l = 0; l < (size_t) kNumLanes; ++l)
        auxDelay[l].setDelay ((float) juce::jmax (0, appliedMax - appliedLatency[l]));
}

void LatencyCompensator::processDryPath (float* L, float* R, int numSamples) noexcept
{
    applyTargets();
    if (appliedMax == 0 || numSamples == 0) return;

    for (int i = 0; i < numSamples; ++i)
    {
        dryDelay.pushSample (0, L[i]);
        L[i] = dryDelay.popSample (0);
        dryDelay.pushSample (1, R[i]);
        R[i] = dryDelay.popSample (1);
    }
}

void LatencyCompensator::processAuxReturn (int lane, float* L, float* R, int numSamples) noexcept
{
    if (lane < 0 || lane >= kNumLanes) return;
    if (numSamples == 0) return;

    const int delay = appliedMax - appliedLatency[(size_t) lane];
    if (delay <= 0) return;

    auto& d = auxDelay[(size_t) lane];
    for (int i = 0; i < numSamples; ++i)
    {
        d.pushSample (0, L[i]);
        L[i] = d.popSample (0);
        d.pushSample (1, R[i]);
        R[i] = d.popSample (1);
    }
}
} // namespace duskstudio
