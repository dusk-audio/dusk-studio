#pragma once

#include "../foundation/IntDelayLine.h"
#include "../foundation/SmoothedValue.h"
#include "../foundation/StereoOversampler.h"
#include "../session/Session.h"
#include <atomic>

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include <dsp/FourKEQDSP.hpp>
  #include <core/UniversalCompressorDSP.hpp>
#endif

namespace duskstudio
{
// Phase 1a aux bus: 3-band EQ → bus compressor → pan → fader → meter.
// EQ uses FourKEQDSP's LF / LM / HF bands (with the LM band exposed as MID and
// the HM band fixed-zero). Comp uses UniversalCompressorDSP's Bus mode. Both
// cores are framework-free donor DSP; their parameter setters are atomic, so
// updateEqParameters / updateCompParameters write lock-free from the audio
// thread.
//
// Buses are subgroups (16 channels → 4 buses → master). They do NOT host
// plugins - that responsibility lives on the AUX return lanes accessed via
// the AUX stage UI.
class BusStrip
{
public:
    BusStrip() = default;

    // oversamplingFactor: 1 (native, default), 2 or 4. Drives the per-bus
    // Dusk Studio-side oversampler that this strip applies around the
    // compressor. The comp core's internal-oversampling path is intentionally
    // never engaged — the external wrapper covers the only saturating stage,
    // and doubling oversampling would compound.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const BusParams& params) noexcept;

    // Applies all bus DSP to L/R in place. Caller has already applied the
    // SIP gate (mute/solo) before invoking.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

private:
    const BusParams* paramsRef = nullptr;
    dusk::audio::SmoothedValue<float> faderGain { 1.0f };
    dusk::audio::SmoothedValue<float> panGainL  { 1.0f };
    dusk::audio::SmoothedValue<float> panGainR  { 1.0f };

#if DUSKSTUDIO_HAS_DUSK_DSP
    duskaudio::FourKEQDSP eq;
    // Only the three band gains vary at runtime (every other EQ param is fixed
    // in prepare); cache them so the per-block update pushes setters only on
    // change — see the ChannelStrip equivalent.
    struct EqGains { float lf = 0.0f, mid = 0.0f, hf = 0.0f; };
    EqGains lastEqGains {};
    duskaudio::UniversalCompressorDSP busComp;
    // Max samples per busComp.processBlock call (the oversampled prepare block
    // size — the core degrades to dry passthrough beyond it); the process
    // chunk loops split anything larger.
    int compMaxBlock = 0;

    // Per-bus Dusk Studio-side oversampler wrapping the comp. Its saturation
    // aliases hard at native rate; the bus EQ runs with saturation at zero
    // (linear), so only the comp needs the wrap.
    dusk::audio::StereoOversampler oversampler;
    int osFactor = 1;

    // When the comp is bypassed we skip the oversampler (the bus EQ is linear
    // and never aliases). The oversampler's FIR round trip imposes 23 (2x) /
    // 26.5 (4x) native samples of latency though, so delay the skip path by
    // that amount to keep this bus time-aligned with the rest of the mix
    // regardless of comp on/off. Integer delay — the 0.5-sample rounding
    // residual at 4x is inaudible.
    static constexpr int kMaxOsLatency = 32;
    dusk::audio::IntDelayLine osSkipDelayL;
    dusk::audio::IntDelayLine osSkipDelayR;
    int osLatencySamples = 0;

    // Skip the EQ filter entirely when the bus EQ is disengaged (it would
    // otherwise run at unity, burning cycles for nothing). Init true so the
    // first block with EQ off doesn't fire a spurious reset; reset on the
    // off→on edge clears stale filter state so re-enabling doesn't click.
    bool prevEqEnabled { true };

    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
#endif

    void updateGainTargets() noexcept;

    // VU-RMS smoother state. Block-RMS is integrated at 300 ms tau on the
    // audio thread so the published atom matches what TapeMachine writes
    // internally for its own VU - keeps mixer + TapeMachine meters in sync.
    double sampleRateForMeter = 44100.0;
    // Precomputed so the per-block meter path skips std::exp on the common
    // (full-block) callback; odd-sized blocks fall back to recomputing alpha.
    int    meterBlockSize = 0;
    float  meterRmsAlpha  = 0.0f;
    float  vuRmsLinL = 0.0f;
    float  vuRmsLinR = 0.0f;
};
} // namespace duskstudio
