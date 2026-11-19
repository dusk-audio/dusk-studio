#include "BusStrip.h"
#include "../foundation/Decibels.h"
#include "../foundation/ScopedNoDenormals.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace duskstudio
{
namespace
{
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kSqrt2  = 1.41421356237309504880f;
const CompStageBlock kNoCompStage {};
} // namespace

void BusStrip::bind (const BusParams& params) noexcept
{
    paramsRef = &params;
}

void BusStrip::prepare (double sampleRate, int blockSize)
{
    sampleRateForMeter = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
    drainQuietTarget = (int) std::min ((double) std::numeric_limits<int>::max(),
                                     std::max (128.0, std::ceil (sampleRateForMeter * 0.020)));
    drainQuietRemaining = 0;
    vuRmsLinL = vuRmsLinR = 0.0f;
    meterBlockSize = std::max (1, blockSize);
    meterRmsAlpha  = std::exp (-((float) meterBlockSize / (float) sampleRateForMeter) / 0.3f);
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);
    panGainL .reset (sampleRate, 0.020);
    panGainR .reset (sampleRate, 0.020);
    panGainL .setCurrentAndTargetValue (1.0f);
    panGainR .setCurrentAndTargetValue (1.0f);

    // Seed the EQ from the session first so the opening block plays the
    // stored setting instead of gliding in from flat.
    updateEqParameters();
    toneEq.prepare (sampleRate);

#if DUSKSTUDIO_HAS_DUSK_DSP
    busComp.prepare (sampleRate, blockSize, 2);
    const int compLatency = busComp.latencySamples();
#else
    const int compLatency = 0;
#endif
    compAlign.prepare (blockSize, compLatency);
    // The quiet dwell outlasts what the compressor stage holds.
    drainQuietTarget = std::max (drainQuietTarget, 2 * compLatency + 1);
}

void BusStrip::discardPendingDrain() noexcept
{
    if (! hasPendingDrain()) return;
    toneEq.reset();
    compAlign.discard();
    drainQuietRemaining = 0;
    vuRmsLinL = vuRmsLinR = 0.0f;
    if (paramsRef != nullptr)
    {
        paramsRef->meterPostBusLDb.store (-100.0f, std::memory_order_relaxed);
        paramsRef->meterPostBusRDb.store (-100.0f, std::memory_order_relaxed);
        paramsRef->meterPostBusRmsL.store (0.0f, std::memory_order_relaxed);
        paramsRef->meterPostBusRmsR.store (0.0f, std::memory_order_relaxed);
        paramsRef->meterGrDb.store (0.0f, std::memory_order_relaxed);
    }
}

void BusStrip::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;
    // The EQ section's status light takes the highpass out with the bands, as
    // on the channel strip.
    BusToneEq::Targets t;
    t.eqOn = paramsRef->eqEnabled.load (std::memory_order_acquire);
    t.gainDb = { paramsRef->eqLfGainDb.load  (std::memory_order_relaxed),
                 paramsRef->eqMidGainDb.load (std::memory_order_relaxed),
                 paramsRef->eqHfGainDb.load  (std::memory_order_relaxed) };
    t.hpfOn = t.eqOn && paramsRef->hpfEnabled.load (std::memory_order_relaxed);
    t.hpfHz = paramsRef->hpfFreq.load (std::memory_order_relaxed);
    toneEq.setTargets (t);
}

void BusStrip::updateGainTargets() noexcept
{
    if (paramsRef == nullptr) return;

    // Reads liveFaderDb / livePan (post-automation), not the raw setpoints.
    // The engine's per-block bus automation routing writes these each block
    // (Off mirrors the manual value; Read/Touch carry the lane), so the
    // Off/Read/Write/Touch hand-off stays in the engine and this DSP stays
    // lane-agnostic - same grammar as MasterBus.
    const float faderDb = paramsRef->liveFaderDb.load (std::memory_order_relaxed);
    const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                       ? 0.0f
                       : dusk::audio::decibelsToGain (faderDb);
    faderGain.setTargetValue (gain);

    // Equal-power L/R balance - pan -1..1 -> angle 0..pi/2.
    const float p     = std::clamp (paramsRef->livePan.load (std::memory_order_relaxed),
                                    -1.0f, 1.0f);
    const float angle = (p + 1.0f) * (kHalfPi * 0.5f);
    panGainL.setTargetValue (std::cos (angle) * kSqrt2);
    panGainR.setTargetValue (std::sin (angle) * kSqrt2);
}

void BusStrip::processInPlace (float* L, float* R, int numSamples) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

    bool inputActive = false;
    for (int sample = 0; sample < numSamples; ++sample)
        if (std::abs (L[sample]) > 1.0e-6f || std::abs (R[sample]) > 1.0e-6f)
        {
            inputActive = true;
            break;
        }

    updateGainTargets();

    // A disengaged or flat EQ drops out of the path on its own once its last
    // move has settled.
    updateEqParameters();
    toneEq.process (L, R, numSamples);

    const auto& stage = compStage != nullptr ? *compStage : kNoCompStage;
#if DUSKSTUDIO_HAS_DUSK_DSP
    const bool compEnabled = paramsRef != nullptr
                          && paramsRef->compEnabled.load (std::memory_order_relaxed);
    if (paramsRef != nullptr && (compEnabled || compAlign.compressorRunning() || busComp.editorOpen()))
        busComp.pushParameters (paramsRef->comp, true, 0);
    compAlign.process (L, R, numSamples, stage, compEnabled, &busComp);

    if (paramsRef != nullptr)
        paramsRef->meterGrDb.store (compAlign.compressorRunning() ? busComp.gainReductionDb() : 0.0f,
                                     std::memory_order_relaxed);
#else
    compAlign.process (L, R, numSamples, stage, false, nullptr);
#endif

    float postPeakL = 0.0f, postPeakR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    bool outputActive = false;
    for (int i = 0; i < numSamples; ++i)
    {
        outputActive = outputActive || std::abs (L[i]) > 1.0e-6f || std::abs (R[i]) > 1.0e-6f;
        const float fg = faderGain.getNextValue();
        const float gL = panGainL.getNextValue() * fg;
        const float gR = panGainR.getNextValue() * fg;
        L[i] *= gL;
        R[i] *= gR;
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
        sumSqL += L[i] * L[i];
        sumSqR += R[i] * R[i];
    }
    // Measure before pan/fader so a closed fader cannot hide a live filter
    // tail. The quiet dwell also drains the full FIR beyond its group delay.
    drainQuietRemaining = inputActive || outputActive ? drainQuietTarget
        : std::max (0, drainQuietRemaining - numSamples);

    if (paramsRef != nullptr)
    {
        const auto toDb = [] (float a) { return a > 1.0e-5f
            ? dusk::audio::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostBusLDb.store (toDb (postPeakL), std::memory_order_relaxed);
        paramsRef->meterPostBusRDb.store (toDb (postPeakR), std::memory_order_relaxed);

        const float invN     = 1.0f / (float) std::max (1, numSamples);
        const float rmsBlkL  = std::sqrt (sumSqL * invN);
        const float rmsBlkR  = std::sqrt (sumSqR * invN);
        const float alpha    = (numSamples == meterBlockSize)
                                 ? meterRmsAlpha
                                 : std::exp (-((float) numSamples / (float) sampleRateForMeter) / 0.3f);   // 300 ms VU integration
        vuRmsLinL = alpha * vuRmsLinL + (1.0f - alpha) * rmsBlkL;
        vuRmsLinR = alpha * vuRmsLinR + (1.0f - alpha) * rmsBlkR;
        paramsRef->meterPostBusRmsL.store (vuRmsLinL, std::memory_order_relaxed);
        paramsRef->meterPostBusRmsR.store (vuRmsLinR, std::memory_order_relaxed);
    }
}
} // namespace duskstudio
