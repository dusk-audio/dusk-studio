#pragma once

#include "BusToneEq.h"
#include "CompStageAlign.h"
#include "../foundation/SmoothedValue.h"
#include "../session/Session.h"
#include <atomic>

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "MultiCompStage.h"
#endif

namespace duskstudio
{
// Bus strip: Tone EQ + highpass -> bus compressor -> pan -> fader -> meter.
// The EQ is BusToneEq, a saturation-free digital tone control. The comp is
// Multi-Comp 2 in Bus mode, which takes the session's values lock-free at the
// top of each block.
//
// Buses are subgroups (16 channels -> 4 buses -> master). They do NOT host
// plugins - that responsibility lives on the AUX return lanes accessed via
// the AUX stage UI.
class BusStrip
{
public:
    BusStrip() = default;

    // The engine's Effect Oversampling does not reach a bus: its EQ is linear
    // and its compressor runs Multi-Comp 2's own 2x.
    void prepare (double sampleRate, int blockSize);
    void bind (const BusParams& params) noexcept;

    // Applies all bus DSP to L/R in place. Caller has already applied the
    // SIP gate (mute/solo) before invoking.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

    bool hasPendingDrain() const noexcept { return drainQuietRemaining > 0; }
    void discardPendingDrain() noexcept;

    // The buses' compressor stage (see CompStageClock), which the engine
    // rewrites before each block's bus pass. Unbound, the stage is off and
    // the compressor never runs.
    void setCompStage (const CompStageBlock* stage) noexcept { compStage = stage; }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // The bus compressor, for its editor. Message thread.
    MultiCompStage& getCompressor() noexcept { return busComp; }
#endif

private:
    const BusParams* paramsRef = nullptr;
    dusk::audio::SmoothedValue<float> faderGain { 1.0f };
    dusk::audio::SmoothedValue<float> panGainL  { 1.0f };
    dusk::audio::SmoothedValue<float> panGainR  { 1.0f };

    // Linear and native-rate: it never aliases and adds no latency.
    BusToneEq toneEq;
    void updateEqParameters() noexcept;

#if DUSKSTUDIO_HAS_DUSK_DSP
    MultiCompStage busComp { CompStage::Bus };
#endif
    CompStageAlign compAlign;
    const CompStageBlock* compStage = nullptr;

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
    int drainQuietRemaining = 0;
    int drainQuietTarget = 960;
};
} // namespace duskstudio
