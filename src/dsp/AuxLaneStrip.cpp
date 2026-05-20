#include "AuxLaneStrip.h"
#include <cmath>

namespace duskstudio
{
void AuxLaneStrip::prepare (double sampleRate, int blockSize)
{
    constexpr double rampSeconds = 0.020;
    returnGain.reset (sampleRate, rampSeconds);
    returnGain.setCurrentAndTargetValue (1.0f);
    for (auto& s : slots)
        s.prepareToPlay (sampleRate, juce::jmax (1, blockSize));

    // Per-slot hardware insert + crossfade gate. Same 20 ms ramp used
    // by the channel strip so mode flips feel consistent across the
    // app.
    for (auto& hw : hardwareSlots) hw.prepare (sampleRate, blockSize);
    for (size_t s = 0; s < (size_t) kMaxPlugins; ++s)
    {
        activeInsertGain[s].reset (sampleRate, rampSeconds);
        activeInsertGain[s].setCurrentAndTargetValue (1.0f);
        activeInsertMode[s] = insertMode[s].load (std::memory_order_relaxed);
        if (activeInsertMode[s] == 0)
            activeInsertMode[s] = kInsertPlugin;   // preserve existing behaviour
    }
    insertScratchL.assign ((size_t) juce::jmax (1, blockSize), 0.0f);
    insertScratchR.assign ((size_t) juce::jmax (1, blockSize), 0.0f);
}

void AuxLaneStrip::bindHardwareInsert (int slotIdx, const HardwareInsertParams& params) noexcept
{
    jassert (slotIdx >= 0 && slotIdx < kMaxPlugins);
    hardwareSlots[(size_t) slotIdx].bind (params);
}

void AuxLaneStrip::updateGainTarget() noexcept
{
    if (paramsRef == nullptr) return;
    const float db = paramsRef->liveReturnLevelDb.load (std::memory_order_relaxed);
    // Same -inf-via-sentinel pattern as the channel strip's fader: anything
    // at or below the floor hard-mutes (avoids feeding reverb tails through
    // an inaudible-but-nonzero gain).
    const float gain = (db <= ChannelStripParams::kFaderInfThreshDb)
                         ? 0.0f
                         : juce::Decibels::decibelsToGain (db);
    returnGain.setTargetValue (gain);
}

void AuxLaneStrip::processStereoBlock (float* L, float* R, int numSamples,
                                          const float* const* deviceInputs,
                                          int   numDeviceInputs,
                                          float* const*       deviceOutputs,
                                          int   numDeviceOutputs) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;
    if (paramsRef == nullptr) return;

    // Defensive clamp - insertScratchL/R are sized to prepare()'s blockSize.
    // The audio host can hand us oversized blocks during transitions; we
    // refuse to allocate on the audio thread, so we cap the work we do here
    // and warn in debug builds. PluginSlot::processStereoBlock above is
    // already block-size-tolerant.
    jassert (numSamples <= (int) insertScratchL.size());
    const int safeSamples = juce::jmin (numSamples, (int) insertScratchL.size());

    // Mute path: clear in-place so the engine's accumulator into master
    // sees silence (the lane's buffer is reused across blocks). Reads
    // liveMute (post-automation) so automated mutes also drop the lane.
    if (paramsRef->liveMute.load (std::memory_order_relaxed))
    {
        std::memset (L, 0, sizeof (float) * (size_t) numSamples);
        std::memset (R, 0, sizeof (float) * (size_t) numSamples);
        paramsRef->meterPostL.store (-100.0f, std::memory_order_relaxed);
        paramsRef->meterPostR.store (-100.0f, std::memory_order_relaxed);
        return;
    }

    updateGainTarget();

    pluginMidiScratch.clear();

    // Each slot can be in plugin / hardware / empty mode independently.
    // The dispatcher runs whichever is active, crossfades to silence on
    // a mode flip, then ramps back up after the swap completes.
    for (int s = 0; s < kMaxPlugins; ++s)
    {
        const auto sIdx = (size_t) s;
        const int req = insertMode[sIdx].load (std::memory_order_acquire);
        if (req != activeInsertMode[sIdx])
        {
            if (activeInsertGain[sIdx].getCurrentValue() > 1.0e-4f)
            {
                activeInsertGain[sIdx].setTargetValue (0.0f);
            }
            else
            {
                activeInsertMode[sIdx] = req;
                if (activeInsertMode[sIdx] == kInsertHardware)
                    hardwareSlots[sIdx].resetTailsAndDelayLine();
                activeInsertGain[sIdx].setTargetValue (
                    activeInsertMode[sIdx] == kInsertEmpty ? 0.0f : 1.0f);
            }
        }
        else
        {
            activeInsertGain[sIdx].setTargetValue (
                activeInsertMode[sIdx] == kInsertEmpty ? 0.0f : 1.0f);
        }

        // Pre-insert stash for the crossfade gate.
        std::memcpy (insertScratchL.data(), L, sizeof (float) * (size_t) safeSamples);
        std::memcpy (insertScratchR.data(), R, sizeof (float) * (size_t) safeSamples);

        if (activeInsertMode[sIdx] == kInsertPlugin)
        {
            slots[sIdx].processStereoBlock (L, R, safeSamples, pluginMidiScratch);
        }
        else if (activeInsertMode[sIdx] == kInsertHardware)
        {
            hardwareSlots[sIdx].processStereoBlock (L, R, safeSamples,
                                                     deviceInputs, numDeviceInputs,
                                                     deviceOutputs, numDeviceOutputs);
        }

        // Blend pre vs post by the gate. An empty slot collapses to pre
        // entirely once the gate has ramped down.
        for (int i = 0; i < safeSamples; ++i)
        {
            const float g = activeInsertGain[sIdx].getNextValue();
            L[i] = (1.0f - g) * insertScratchL[(size_t) i] + g * L[i];
            R[i] = (1.0f - g) * insertScratchR[(size_t) i] + g * R[i];
        }
        // Smoother tail-advance on oversized host blocks so the gate's
        // counter stays aligned with the lane's return-gain smoother (which
        // ticks numSamples times further down).
        for (int i = safeSamples; i < numSamples; ++i)
            activeInsertGain[sIdx].getNextValue();
    }

    // Return level + meter peak.
    float postPeakL = 0.0f, postPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = returnGain.getNextValue();
        L[i] *= g;
        R[i] *= g;
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
    }

    const auto toDb = [] (float a)
    {
        return a > 1.0e-5f ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f;
    };
    paramsRef->meterPostL.store (toDb (postPeakL), std::memory_order_relaxed);
    paramsRef->meterPostR.store (toDb (postPeakR), std::memory_order_relaxed);
}
} // namespace duskstudio
