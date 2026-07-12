#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <memory>
#include "../foundation/IntDelayLine.h"
#include "../foundation/SmoothedValue.h"
#include "../foundation/StereoOversampler.h"
#include "../session/Session.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "PluginProcessor.h"    // TapeMachine/Source - master tape emulation (full plugin processor + editor)
  #include <core/MultiQTube.hpp>             // multi-q - Pultec-style Tube EQ (framework-free donor core)
  #include <core/UniversalCompressorDSP.hpp> // multi-comp - Bus mode master comp (framework-free donor core)
#endif

namespace duskstudio
{
// Phase 1a master bus: Pultec-style Tube EQ -> bus compressor -> tape
// saturation -> master fader. Parameters come from session.master() via
// MasterBusParams; UI mutates the atomics, the audio thread reads them.
class MasterBus
{
public:
    MasterBus();

    // oversamplingFactor: 1 = native (default), 2 = 2× ox, 4 = 4× ox. Affects
    // the bus compressor's internal oversampling toggle and the tape sat
    // oversampler's stage count. Other values are clamped to 1.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const MasterBusParams& params) noexcept;

    void processInPlace (float* L, float* R, int numSamples) noexcept;

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Live access to the hosted TapeMachine processor. Used by the master
    // strip's gear button to spawn its editor on demand.
    TapeMachineAudioProcessor& getTapeProcessor() noexcept { return tape; }
#endif

private:
    const MasterBusParams* paramsRef = nullptr;
    dusk::audio::SmoothedValue<float> faderGain { 1.0f };

#if DUSKSTUDIO_HAS_DUSK_DSP
    TapeMachineAudioProcessor   tape;
    juce::AudioBuffer<float>    tapeStereoBuffer;    // pre-allocated; tape processBlock target
    juce::MidiBuffer            tapeMidi;            // unused but required by processBlock
    std::atomic<float>*         tapeBypassAtom = nullptr;

    // TAPE on/off crossfade. The donor hard-bypasses (early-returns, no ramp),
    // so toggling it would pop. We blend the pre-tape (dry) signal against the
    // processed (wet) output over 20 ms here instead. Tape is skipped entirely
    // once fully faded out, so a disengaged tape still costs ~nothing.
    juce::SmoothedValue<float>  tapeMix { 0.0f };    // 0 = dry, 1 = wet
    bool                        tapeMixPrimed = false;
    juce::AudioBuffer<float>    tapeDryBuffer;       // pre-allocated; holds dry during the fade
    duskaudio::MultiQTube             tubeEQ;
    duskaudio::UniversalCompressorDSP busComp;
    // Max samples per busComp.processBlock call (the oversampled prepare block
    // size - the core degrades to dry passthrough beyond it); the process chunk
    // loop splits anything larger. The tube EQ has no block-size limit.
    int compMaxBlock = 0;

    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
    // Lock-free atomic store, used for the tape stage's bypass APVTS atom.
    static inline void storeAtom (std::atomic<float>* a, float v) noexcept
    {
        if (a != nullptr) a->store (v, std::memory_order_relaxed);
    }
#endif

    int currentOxFactor      = 1;     // 1, 2 or 4 - set in prepare(); drives the
                                       // Dusk Studio-side oversampler around (TubeEQ
                                       // + comp) and the TapeMachine "oversampling"
                                       // APVTS choice atom. The comp core's internal
                                       // oversampling path is never engaged because
                                       // the Dusk Studio-side wrap handles it.

    // Master oversampler around (TubeEQ + bus comp). Both stages saturate (tube
    // + comp) and alias at native rate; running them at oversampled rate inside
    // this wrap suppresses it. TapeMachine has its own internal oversampling
    // (driven via APVTS) so it's processed at native rate AFTER this wrap.
    dusk::audio::StereoOversampler oversampler;

    // When EQ + comp are both bypassed the oversampler is skipped. Its FIR round
    // trip imposes 23 (2x) / 26.5 (4x) native samples of latency, so delay the
    // skip path by that amount to keep the master's latency invariant to the
    // EQ/comp toggle. Integer delay - the 0.5-sample residual at 4x is inaudible.
    static constexpr int kMaxOsLatency = 32;
    dusk::audio::IntDelayLine osSkipDelayL;
    dusk::audio::IntDelayLine osSkipDelayR;
    int osLatencySamples = 0;

    // The skip ring is fed on EVERY block at OS factors > 1 (wrap-on blocks
    // push and discard) so toggling EQ+comp off emits continuous delayed dry
    // instead of a zero-filled gap while the ring refills. The oversampler is
    // reset on the wrap-on edge so its FIR ramps in from silence, not a stale
    // tail from the last time the wrap ran.
    bool prevWrapActive { false };

    // juce::dsp integer delay line, retained for the tape crossfade dry-path PDC
    // below (the tape stage keeps its JUCE API).
    using OsDelayLine = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>;

    // Dry-path PDC for the tape crossfade. Tape adds its own oversampler
    // latency when engaged (0 at 1×); delaying the dry by the same amount keeps
    // the on/off blend phase-coherent (no comb mid-fade) and seamless (no
    // timing jump at the fade ends). Resolved in prepare from the donor's
    // reported latency; max sized to it. Fed every block at >0 latency so the
    // ring stays warm for the next toggle - a constant, sub-ms master latency.
    OsDelayLine tapeDryDelayL { 1 };
    OsDelayLine tapeDryDelayR { 1 };
    int tapeLatencySamples = 0;

    // VU-RMS smoother state - 300 ms tau on the audio thread so the
    // analog VU on the master strip reads the same level TapeMachine's
    // own VU shows on identical signals.
    double sampleRateForMeter = 44100.0;
    float  vuRmsLinL = 0.0f;
    float  vuRmsLinR = 0.0f;
    // RMS smoothing coefficient precomputed for the prepared block size so the
    // per-block meter path skips std::exp on the common (full-block) callback.
    int    meterBlockSize = 0;
    float  meterRmsAlpha  = 0.0f;
};
} // namespace duskstudio
