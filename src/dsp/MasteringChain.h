#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include "BrickwallLimiter.h"
#include "LoudnessMeter.h"
#include "MasteringDigitalEq.h"
#include "../session/Session.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "UniversalCompressor.h"
#endif

namespace duskstudio
{
// Mastering-stage signal chain: Tube EQ → Bus comp → Brickwall limiter.
// Reads its parameters from MasteringParams (a sibling of MasterBusParams);
// the same DSP cores are used so the sound carries from the live master
// into mastering, with the limiter as the new final stage.
//
// Tape saturation is intentionally NOT in this chain. Mastering happens
// AFTER the bounce, where the master tape sat (if engaged in Mixing) is
// already baked in. A dedicated mastering tape stage is a future option.
class MasteringChain
{
public:
    MasteringChain() = default;

    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const MasteringParams& params) noexcept;

    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // Reset the integrated LUFS history - used when the user clicks
    // "Reset" in the mastering view, or when a new mix is loaded so the
    // integrated reading reflects only the current source.
    void resetLoudness();

    int getLatencySamples() const noexcept { return limiter.getLatencySamples(); }

    // UI-side accessors. The Mastering view embeds the UniversalCompressor's
    // own AudioProcessorEditor for the Multi-Comp panel and drives the
    // limiter atomics through a custom editor. Returns null when the donor
    // DSP isn't compiled in (DUSKSTUDIO_HAS_DUSK_DSP=0 build).
#if DUSKSTUDIO_HAS_DUSK_DSP
    UniversalCompressor* getCompProcessor() noexcept { return &busComp; }
#else
    juce::AudioProcessor* getCompProcessor() noexcept { return nullptr; }
#endif
    BrickwallLimiter&    getLimiter() noexcept { return limiter; }

private:
    const MasteringParams* paramsRef = nullptr;

    BrickwallLimiter      limiter;
    LoudnessMeter         loudnessMeter;
    MasteringDigitalEq    digitalEq;   // replaces the Tube EQ in the master strip

    void updateEqParameters() noexcept;
    void updateLimiterParameters() noexcept;

#if DUSKSTUDIO_HAS_DUSK_DSP
    UniversalCompressor         busComp;
    juce::MidiBuffer            compMidi;
    juce::AudioBuffer<float>    compStereoBuffer;

    std::atomic<float>* compModeAtom        = nullptr;
    std::atomic<float>* compBypassAtom      = nullptr;
    std::atomic<float>* compMixAtom         = nullptr;
    std::atomic<float>* compAutoMakeupAtom  = nullptr;
    std::atomic<float>* compBusThreshAtom   = nullptr;
    std::atomic<float>* compBusRatioAtom    = nullptr;
    std::atomic<float>* compBusAttackAtom   = nullptr;
    std::atomic<float>* compBusReleaseAtom  = nullptr;
    std::atomic<float>* compBusMakeupAtom   = nullptr;
    std::atomic<float>* compBusMixAtom      = nullptr;

    void bindCompParams();
    void updateCompParameters() noexcept;

    static inline void storeAtom (std::atomic<float>* a, float v) noexcept
    {
        if (a != nullptr) a->store (v, std::memory_order_relaxed);
    }
#endif

    int preparedBlockSize = 0;

    // VU-RMS smoother state - 300 ms tau on the audio thread, same as
    // BusStrip / MasterBus. Drives the mastering analog VU.
    double sampleRateForMeter = 44100.0;
    float  vuRmsLinL = 0.0f;
    float  vuRmsLinR = 0.0f;
    // RMS smoothing coefficient precomputed for the prepared block size so the
    // per-block meter path skips std::exp on the common (full-block) callback.
    int    meterBlockSize = 0;
    float  meterRmsAlpha  = 0.0f;
};
} // namespace duskstudio
