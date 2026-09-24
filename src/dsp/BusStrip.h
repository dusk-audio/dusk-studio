#pragma once

#include "BusToneEq.h"
#include "../foundation/IntDelayLine.h"
#include "../foundation/SmoothedValue.h"
#include "../foundation/StereoOversampler.h"
#include "../session/Session.h"
#include <atomic>

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include <core/UniversalCompressorDSP.hpp>
#endif

namespace duskstudio
{
// Bus strip: Tone EQ + highpass -> bus compressor -> pan -> fader -> meter.
// The EQ is BusToneEq, a saturation-free digital tone control. The comp is the
// donor UniversalCompressorDSP in Bus mode, whose parameter setters are atomic,
// so updateCompParameters writes lock-free from the audio thread.
//
// Buses are subgroups (16 channels -> 4 buses -> master). They do NOT host
// plugins - that responsibility lives on the AUX return lanes accessed via
// the AUX stage UI.
class BusStrip
{
public:
    BusStrip() = default;

    // oversamplingFactor: 1 (native, default), 2 or 4. Drives the per-bus
    // Dusk Studio-side oversampler that this strip applies around the
    // compressor. The comp core's internal-oversampling path is intentionally
    // never engaged - the external wrapper covers the only saturating stage,
    // and doubling oversampling would compound.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const BusParams& params) noexcept;

    // Applies all bus DSP to L/R in place. Caller has already applied the
    // SIP gate (mute/solo) before invoking.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // How far the bus delays its output at the prepared oversampling factor,
    // comp on or off; 0 at 1x.
    int getOversamplingLatencySamples() const noexcept { return osLatencySamples; }

private:
    const BusParams* paramsRef = nullptr;
    dusk::audio::SmoothedValue<float> faderGain { 1.0f };
    dusk::audio::SmoothedValue<float> panGainL  { 1.0f };
    dusk::audio::SmoothedValue<float> panGainR  { 1.0f };

    // Linear and native-rate: it never aliases, so it always runs outside
    // the comp's oversampler and adds no latency.
    BusToneEq toneEq;
    void updateEqParameters() noexcept;

#if DUSKSTUDIO_HAS_DUSK_DSP
    duskaudio::UniversalCompressorDSP busComp;
    // Max samples per busComp.processBlock call (the oversampled prepare block
    // size - the core degrades to dry passthrough beyond it); the process
    // chunk loops split anything larger.
    int compMaxBlock = 0;

    // Per-bus Dusk Studio-side oversampler wrapping the comp. Its saturation
    // aliases hard at native rate; the bus EQ is linear, so only the comp
    // needs the wrap.
    dusk::audio::StereoOversampler oversampler;
    int osFactor = 1;

    // When the comp is bypassed we skip the oversampler (the bus EQ is linear
    // and never aliases). The oversampler's FIR round trip imposes 23 (2x) /
    // 26.5 (4x) native samples of latency though, so delay the skip path by
    // that amount to keep this bus time-aligned with the rest of the mix
    // regardless of comp on/off. Integer delay - the 0.5-sample rounding
    // residual at 4x is inaudible.
    static constexpr int kMaxOsLatency = 32;
    dusk::audio::IntDelayLine osSkipDelayL;
    dusk::audio::IntDelayLine osSkipDelayR;
    int osLatencySamples = 0;

    // The skip ring is fed on EVERY block at OS factors > 1 (comp-on blocks
    // push and discard) so a comp-off toggle emits continuous delayed dry
    // instead of a zero-filled gap while the ring refills. The oversampler is
    // reset on the comp-on edge so its FIR ramps in from silence, not a stale
    // tail from the last time the comp ran.
    bool prevCompOsActive { false };

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
