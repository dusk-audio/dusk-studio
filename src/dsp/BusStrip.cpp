#include "BusStrip.h"
#include "../foundation/Decibels.h"
#include "../foundation/ScopedNoDenormals.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace duskstudio
{
namespace
{
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kSqrt2  = 1.41421356237309504880f;
} // namespace

void BusStrip::bind (const BusParams& params) noexcept
{
    paramsRef = &params;
}

void BusStrip::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    sampleRateForMeter = sampleRate > 0.0 ? sampleRate : 44100.0;
    vuRmsLinL = vuRmsLinR = 0.0f;
    meterBlockSize = std::max (1, blockSize);
    meterRmsAlpha  = std::exp (-((float) meterBlockSize / (float) sampleRateForMeter) / 0.3f);
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);
    panGainL .reset (sampleRate, 0.020);
    panGainR .reset (sampleRate, 0.020);
    panGainL .setCurrentAndTargetValue (1.0f);
    panGainR .setCurrentAndTargetValue (1.0f);

    // Bus oversampling: wrap the comp externally. The core's own oversampling
    // path is never engaged because Dusk Studio does the up/downsample around
    // it — having the core oversample on top would compound. The EQ runs
    // linear (saturation zero), so it stays outside the wrap entirely.
    // 4x, 2x, or 1x (no oversampling).
    const int factor = (oversamplingFactor == 2 || oversamplingFactor == 4)
                            ? oversamplingFactor : 1;
    osFactor = factor;
    const int bsClamped = std::max (1, blockSize);
    oversampler.setFactor (factor);
    oversampler.prepare (bsClamped);
    osLatencySamples = (factor > 1)
        ? std::min (kMaxOsLatency, (int) std::lround (oversampler.latency()))
        : 0;

    osSkipDelayL.setMaximumDelayInSamples (kMaxOsLatency);
    osSkipDelayR.setMaximumDelayInSamples (kMaxOsLatency);
    osSkipDelayL.setDelay (osLatencySamples);
    osSkipDelayR.setDelay (osLatencySamples);
    osSkipDelayL.reset();
    osSkipDelayR.reset();

    const double prepSr = sampleRate * (double) factor;
    const int    prepBs = bsClamped * factor;

#if DUSKSTUDIO_HAS_DUSK_DSP
    // The bus EQ is linear tone-shaping (no saturation), so it never aliases
    // and is prepared at NATIVE rate — it always runs outside the oversampler.
    // Only the comp (below) is wrapped, and only when engaged.
    //
    // Bus EQ frequencies + gain range mirror Harrison Mixbus's mix-bus Tone EQ
    // exactly: LO 300 Hz shelf / MID 800 Hz Q0.7 bell / HI 2 kHz shelf,
    // +/-9 dB — wide, musical tone-shaping for subgroups. (NOT the narrower
    // Mixbus master-bus EQ, which is 90/300/4000 Hz +/-6 dB.) Everything but
    // the three band gains (updateEqParameters) is fixed here.
    eq.setHpfEnabled (false); eq.setHpfFreq (80.0f);
    eq.setLpfEnabled (false); eq.setLpfFreq (20000.0f);
    eq.setLfFreq (300.0f);    eq.setLfBell (false);   // shelf
    eq.setLmFreq (800.0f);    eq.setLmQ (0.7f);
    // HM unused; Bus EQ exposes only LF / MID / HF.
    eq.setHmGain (0.0f);      eq.setHmFreq (4000.0f); eq.setHmQ (0.7f);
    eq.setHfFreq (2000.0f);   eq.setHfBell (false);
    eq.setEqType (0);         // Brown (E-series voicing)
    eq.setSaturation (0.0f);
    eq.setInputGainDb (0.0f);
    eq.setOutputGainDb (0.0f);
    eq.setOversampling (0);   // 1x — linear chain, and the strip owns any OS
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.setLfGain (lastEqGains.lf);
    eq.setLmGain (lastEqGains.mid);
    eq.setHfGain (lastEqGains.hf);
    eq.prepare (sampleRate, bsClamped);
    eq.reset();

    busComp.setMode (3);            // Bus mode
    busComp.setMix (100.0f);
    busComp.setBusMix (100.0f);
    busComp.setAutoMakeup (false);
    // No injected analog hiss under signal: the core does not port the donor's
    // noise stage, so no explicit force-off is needed here (the JUCE donors in
    // MasterBus::bindCompParams still store noise_enable = 0).
    busComp.prepare (prepSr, prepBs);
    busComp.reset();
    compMaxBlock = prepBs;
#endif
}

#if DUSKSTUDIO_HAS_DUSK_DSP
void BusStrip::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;
    EqGains g;
    g.lf  = paramsRef->eqLfGainDb.load  (std::memory_order_relaxed);
    g.mid = paramsRef->eqMidGainDb.load (std::memory_order_relaxed);
    g.hf  = paramsRef->eqHfGainDb.load  (std::memory_order_relaxed);
    if (std::memcmp (&g, &lastEqGains, sizeof (g)) != 0)
    {
        eq.setLfGain (g.lf);
        eq.setLmGain (g.mid);
        eq.setHfGain (g.hf);
        lastEqGains = g;
    }
}

void BusStrip::updateCompParameters() noexcept
{
    if (paramsRef == nullptr) return;
    busComp.setBypass (! paramsRef->compEnabled.load (std::memory_order_relaxed));
    busComp.setBusThreshold (paramsRef->compThreshDb.load (std::memory_order_relaxed));
    // The donor's bus_ratio / bus_attack / bus_release are SSL-style stepped
    // Choice params, NOT continuous. Storing the raw knob value treated it as
    // an out-of-range index (ratio 4.0 -> 2:1, attack 10 ms -> 30 ms). Map to
    // the nearest discrete index — same as a real SSL bus comp's stepped knobs.
    //   bus_ratio:  0=2:1, 1=4:1, 2=10:1
    //   bus_attack: 0=0.1  1=0.3  2=1  3=3  4=10  5=30  (ms)
    const float ratio = paramsRef->compRatio.load (std::memory_order_relaxed);
    busComp.setBusRatio (ratio < 3.0f ? 0 : (ratio < 7.0f ? 1 : 2));
    const float atkMs = paramsRef->compAttackMs.load (std::memory_order_relaxed);
    busComp.setBusAttack (atkMs < 0.2f ? 0
                        : (atkMs < 0.65f ? 1
                        : (atkMs < 2.0f ? 2
                        : (atkMs < 6.5f ? 3
                        : (atkMs < 20.0f ? 4 : 5)))));
    // bus_release is a Choice {0.1s, 0.3s, 0.6s, 1.2s, Auto}; see
    // MasterBus::updateCompParameters for the mapping rationale.
    const bool autoRel = paramsRef->compReleaseAuto.load (std::memory_order_relaxed);
    const float relMs  = paramsRef->compReleaseMs.load   (std::memory_order_relaxed);
    busComp.setBusRelease (autoRel ? 4
                         : (relMs < 200.0f ? 0
                         : (relMs < 450.0f ? 1
                         : (relMs < 900.0f ? 2 : 3))));
    busComp.setBusMakeup (paramsRef->compMakeupDb.load (std::memory_order_relaxed));
}
#endif

void BusStrip::updateGainTargets() noexcept
{
    if (paramsRef == nullptr) return;

    // Reads liveFaderDb / livePan (post-automation), not the raw setpoints.
    // The engine's per-block bus automation routing writes these each block
    // (Off mirrors the manual value; Read/Touch carry the lane), so the
    // Off/Read/Write/Touch hand-off stays in the engine and this DSP stays
    // lane-agnostic — same grammar as MasterBus.
    const float faderDb = paramsRef->liveFaderDb.load (std::memory_order_relaxed);
    const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                       ? 0.0f
                       : dusk::audio::decibelsToGain (faderDb);
    faderGain.setTargetValue (gain);

    // Equal-power L/R balance - pan -1..1 → angle 0..pi/2.
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

    updateGainTargets();

#if DUSKSTUDIO_HAS_DUSK_DSP
    updateEqParameters();
    updateCompParameters();

    // EQ is linear and native-rate, so it runs outside the oversampler — but
    // only when engaged (a disabled EQ would otherwise run at unity, wasting
    // cycles). The comp is the only saturating stage; the oversampler wraps
    // just the comp and only when it's engaged. With comp off we delay the
    // signal by the oversampler latency so the bus stays aligned with comp-on
    // buses — EQ on/off is latency-free, so the delay logic is unaffected.
    const bool eqEnabled = paramsRef != nullptr
                        && paramsRef->eqEnabled.load (std::memory_order_relaxed);
    const bool compEnabled = paramsRef != nullptr
                          && paramsRef->compEnabled.load (std::memory_order_relaxed);

    if (eqEnabled && ! prevEqEnabled)
        eq.reset();                 // clear stale state so re-enabling doesn't click
    prevEqEnabled = eqEnabled;

    if (eqEnabled)
    {
        const float* eqIn[2]  = { L, R };
        float*       eqOut[2] = { L, R };
        eq.processBlock (eqIn, eqOut, 2, numSamples);
    }

    if (compEnabled && osFactor > 1)
    {
        // Oversample around the comp only — band-limits its saturation. The
        // comp was prepared at sampleRate × factor in prepare(). Contract:
        // numSamples must not exceed the blockSize passed to prepare — the
        // engine bails on host-oversized blocks before the strips run.
        auto up = oversampler.processSamplesUp (L, R, numSamples);
        for (int offset = 0; offset < up.numSamples; offset += compMaxBlock)
        {
            const int n = std::min (compMaxBlock, up.numSamples - offset);
            const float* compIn[2]  = { up.L + offset, up.R + offset };
            float*       compOut[2] = { up.L + offset, up.R + offset };
            busComp.processBlock (compIn, compOut, 2, n);
        }
        oversampler.processSamplesDown (L, R, numSamples);
    }
    else if (compEnabled)
    {
        // Native comp (factor == 1).
        for (int offset = 0; offset < numSamples; offset += compMaxBlock)
        {
            const int n = std::min (compMaxBlock, numSamples - offset);
            const float* compIn[2]  = { L + offset, R + offset };
            float*       compOut[2] = { L + offset, R + offset };
            busComp.processBlock (compIn, compOut, 2, n);
        }
    }
    else if (osFactor > 1 && osLatencySamples > 0)
    {
        // Comp off but an OS factor is active → the comp's oversampler was
        // skipped. Delay the EQ-only signal by its latency to hold alignment.
        for (int i = 0; i < numSamples; ++i)
        {
            osSkipDelayL.pushSample (L[i]);  L[i] = osSkipDelayL.popSample();
            osSkipDelayR.pushSample (R[i]);  R[i] = osSkipDelayR.popSample();
        }
    }

    if (paramsRef != nullptr)
        paramsRef->meterGrDb.store (compEnabled ? busComp.getGainReduction() : 0.0f,
                                     std::memory_order_relaxed);
#endif

    float postPeakL = 0.0f, postPeakR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
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
