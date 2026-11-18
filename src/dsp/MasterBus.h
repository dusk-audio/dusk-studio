#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include "CompStageAlign.h"
#include "../foundation/IntDelayLine.h"
#include "../foundation/SmoothedValue.h"
#include "../foundation/StereoOversampler.h"
#include "../session/Session.h"

#if DUSKSTUDIO_HAS_DUSK_DSP
  #include "MasterTape.h"                    // Tape Machine 2 - master tape emulation (built-in DAF plug-in)
  #include "MultiCompStage.h"                // Multi-Comp 2 - Bus mode master comp (built-in DAF plug-in)
  #include <core/MultiQTube.hpp>             // multi-q - Pultec-style Tube EQ (framework-free donor core)
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

    // oversamplingFactor: 1 = native (default), 2 = 2× ox, 4 = 4× ox. Sets the
    // oversampler around the tube EQ; the compressor and the tape keep their
    // own. Other values are clamped to 1.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const MasterBusParams& params) noexcept;

    void processInPlace (float* L, float* R, int numSamples) noexcept;

    // How far the EQ stage delays the master at the prepared oversampling
    // factor, engaged or not; 0 at 1x. The tape's delay is separate, and so is
    // the compressor stage's, which is the engine's (see setCompStage).
    int getOversamplingLatencySamples() const noexcept { return osLatencySamples; }

    // The master's compressor stage (see CompStageClock), which the engine
    // rewrites before each block. Unbound, the stage is off and the
    // compressor never runs.
    void setCompStage (const CompStageBlock* stage) noexcept { compStage = stage; }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // The master tape and compressor, for their editors. Message thread.
    MasterTape& getTape() noexcept { return tape; }
    MultiCompStage& getCompressor() noexcept { return busComp; }
#endif

private:
    const MasterBusParams* paramsRef = nullptr;
    dusk::audio::SmoothedValue<float> faderGain { 1.0f };

#if DUSKSTUDIO_HAS_DUSK_DSP
    MasterTape                  tape;
    // Max samples per tape.processInPlace call - the core's scratch is sized to
    // the prepared block, so the process loop chunks anything larger.
    int                         tapeMaxBlock = 1;

    // TAPE on/off crossfade. The core hard-bypasses (early-returns, no ramp),
    // so toggling it would pop. We blend the pre-tape (dry) signal against the
    // processed (wet) output over 20 ms here instead. Tape is skipped entirely
    // once fully faded out, so a disengaged tape still costs ~nothing.
    dusk::audio::SmoothedValue<float> tapeMix { 0.0f }; // 0 = dry, 1 = wet
    bool                        tapeMixPrimed = false;
    std::vector<float>          tapeDryL;
    std::vector<float>          tapeDryR;
    duskaudio::MultiQTube tubeEQ;
    // Runs after the EQ's wrap and before the tape; it oversamples itself.
    MultiCompStage busComp { CompStage::Bus };

    void updateEqParameters() noexcept;
#endif
    CompStageAlign compAlign;
    const CompStageBlock* compStage = nullptr;

    int currentOxFactor      = 1;     // 1, 2 or 4 - set in prepare(); drives the
                                       // Dusk Studio-side oversampler around the
                                       // TubeEQ.

    // Master oversampler around the TubeEQ, whose tube stage saturates and
    // aliases at native rate; running it at oversampled rate inside this wrap
    // suppresses it. The compressor and the tape core oversample internally, so
    // they're processed at native rate AFTER this wrap.
    dusk::audio::StereoOversampler oversampler;

    // When the EQ is bypassed the oversampler is skipped. Its FIR round trip
    // imposes 23 (2x) / 26.5 (4x) native samples of latency, so delay the skip
    // path by that amount to keep the master's latency invariant to the EQ
    // toggle. Integer delay - the 0.5-sample residual at 4x is inaudible.
    static constexpr int kMaxOsLatency = 32;
    dusk::audio::IntDelayLine osSkipDelayL;
    dusk::audio::IntDelayLine osSkipDelayR;
    int osLatencySamples = 0;

    // The skip ring is fed on EVERY block at OS factors > 1 (wrap-on blocks
    // push and discard) so toggling the EQ off emits continuous delayed dry
    // instead of a zero-filled gap while the ring refills. The oversampler is
    // reset on the wrap-on edge so its FIR ramps in from silence, not a stale
    // tail from the last time the wrap ran.
    bool prevWrapActive { false };

    // Dry-path PDC for the tape crossfade. Tape carries its own delay when
    // engaged; delaying the dry by the same amount keeps the on/off blend
    // phase-coherent (no comb mid-fade) and seamless (no timing jump at the
    // fade ends). Resolved in prepare from the core's processing-path figure,
    // which is what the master holds on every path, and max sized to it. Fed
    // every block so the ring stays warm for the next toggle - a constant,
    // sub-ms master latency.
    dusk::audio::IntDelayLine tapeDryDelayL;
    dusk::audio::IntDelayLine tapeDryDelayR;
    int tapeLatencySamples = 0;

public:
    // The master's own delay, which the tape stage holds whether the tape is
    // engaged or not, and on a passthrough signal path, so nothing the user
    // changes there jumps the timing. A render has to trim it, the same way it
    // trims the track and aux compensation.
    int getTapeLatencySamples() const noexcept { return tapeLatencySamples; }

private:

    // VU-RMS smoother state - 300 ms tau on the audio thread, the VU standard
    // integration time the analog meter on the master strip is calibrated to.
    double sampleRateForMeter = 44100.0;
    float  vuRmsLinL = 0.0f;
    float  vuRmsLinR = 0.0f;
    // RMS smoothing coefficient precomputed for the prepared block size so the
    // per-block meter path skips std::exp on the common (full-block) callback.
    int    meterBlockSize = 0;
    float  meterRmsAlpha  = 0.0f;
};
} // namespace duskstudio
