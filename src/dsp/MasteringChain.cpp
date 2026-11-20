#include "MasteringChain.h"
#include "../foundation/Decibels.h"
#include "../foundation/ScopedNoDenormals.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace duskstudio
{
void MasteringChain::bind (const MasteringParams& params) noexcept
{
    paramsRef = &params;
}

void MasteringChain::prepare (double sampleRate, int blockSize)
{
    const int bs = std::max (1, blockSize);
    sampleRateForMeter = sampleRate > 0.0 ? sampleRate : 44100.0;
    vuRmsLinL = vuRmsLinR = 0.0f;
    meterBlockSize = bs;
    meterRmsAlpha  = std::exp (-((float) bs / (float) sampleRateForMeter) / 0.3f);

    digitalEq.prepare (sampleRate, bs);
    digitalEq.reset();

#if DUSKSTUDIO_HAS_DUSK_DSP
    // The plug-in comes back at rest and the delay beside it empty, so the
    // first block, an offline render's included, starts both from silence.
    comp.prepare (sampleRate, bs, 2);
    const int compLatency = comp.latencySamples();
    compClock.prepare (sampleRate, compLatency);
    compClock.snap (true);
    compAlign.prepare (bs, compLatency);
    compLatencySamples.store (compLatency, std::memory_order_relaxed);
#endif

    const double initialLookaheadMs = (paramsRef != nullptr)
        ? (double) paramsRef->limiterLookaheadMs.load (std::memory_order_relaxed) : 2.0;
    limiter.prepare (sampleRate, bs, initialLookaheadMs);
    limiter.reset();

    loudnessMeter.prepare (sampleRate, bs);
    phaseFeed.prepare (sampleRateForMeter);

    scopeRingMask.store (0, std::memory_order_release);
    for (auto& sample : scopeRing)
        sample.store (0.0f, std::memory_order_relaxed);
    scopeWritePos.store (0, std::memory_order_relaxed);
    scopeSampleRate.store (sampleRateForMeter, std::memory_order_relaxed);
    scopeRingMask.store (kScopeRingSize - 1, std::memory_order_release);

    preparedBlockSize = bs;
}

void MasteringChain::pushScope (const float* L, const float* R, int n) noexcept
{
    const int mask = scopeRingMask.load (std::memory_order_acquire);
    if (mask == 0) return;
    long long wp = scopeWritePos.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        scopeRing[(std::size_t) (wp++ & mask)].store (0.5f * (L[i] + R[i]), std::memory_order_relaxed);
    scopeWritePos.store (wp, std::memory_order_release);
}

int MasteringChain::readScopeLatest (float* dest, int count) const noexcept
{
    if (count <= 0 || dest == nullptr) return 0;
    count = std::min (count, kScopeRingSize);
    const int mask = scopeRingMask.load (std::memory_order_acquire);
    if (mask == 0) return 0;   // matches pushScope; guards a mask-not-yet-stored race
    const long long wp = scopeWritePos.load (std::memory_order_acquire);
    const long long start = wp - count;
    for (int i = 0; i < count; ++i)
        dest[i] = scopeRing[(std::size_t) ((start + i) & mask)].load (std::memory_order_relaxed);
    return count;
}

void MasteringChain::resetLoudness()
{
    // Message-thread entry (MasteringView's Reset I button) while the audio
    // thread may be inside loudnessMeter.process() - defer, never reset
    // directly.
    loudnessMeter.requestReset();
}

void MasteringChain::restoreLoudnessHistory (const LoudnessMeter::History& history) noexcept
{
    loudnessMeter.restoreHistory (history);
    if (paramsRef != nullptr)
    {
        paramsRef->meterIntegratedLufs.store (loudnessMeter.getIntegratedLufs(), std::memory_order_relaxed);
        paramsRef->meterTruePeakDb.store     (loudnessMeter.getTruePeakDb(),     std::memory_order_relaxed);
    }
}

void MasteringChain::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;

    digitalEq.setEnabled (paramsRef->eqEnabled.load (std::memory_order_relaxed));
    for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
    {
        digitalEq.setBandFreq   (b, paramsRef->eqBandFreq  [b].load (std::memory_order_relaxed));
        digitalEq.setBandGainDb (b, paramsRef->eqBandGainDb[b].load (std::memory_order_relaxed));
        digitalEq.setBandQ      (b, paramsRef->eqBandQ     [b].load (std::memory_order_relaxed));
    }
}

void MasteringChain::updateLimiterParameters() noexcept
{
    if (paramsRef == nullptr) return;
    limiter.setEnabled    (paramsRef->limiterEnabled.load (std::memory_order_relaxed));
    limiter.setInputDriveDb (paramsRef->limiterDriveDb.load (std::memory_order_relaxed));
    limiter.setCeilingDb  (paramsRef->limiterCeilingDb.load (std::memory_order_relaxed));
    limiter.setReleaseMs  (paramsRef->limiterReleaseMs.load (std::memory_order_relaxed));
    limiter.setLookaheadMs (paramsRef->limiterLookaheadMs.load (std::memory_order_relaxed));
    limiter.setMode       (paramsRef->limiterMode.load (std::memory_order_relaxed));
    limiter.setStereoLink (paramsRef->limiterStereoLink.load (std::memory_order_relaxed));
}

void MasteringChain::processInPlace (float* L, float* R, int numSamples, bool capturePhase) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    if (numSamples == 0) return;
    // A host block larger than what prepare() sized the scratch for cannot be
    // processed in part: the smoothers and meters below would desync from the
    // audio. Clear and skip, as AuxLaneStrip does on the identical contract -
    // leaving the buffer untouched would leak the unprocessed mix to the output.
    assert (numSamples <= preparedBlockSize);
    if (numSamples > preparedBlockSize)
    {
        phaseFeed.markDiscontinuity();
        std::memset (L, 0, sizeof (float) * (size_t) numSamples);
        std::memset (R, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    // 5-band digital EQ - replaces the Tube EQ that the master strip
    // uses in Mixing. Mastering wants a clean parametric EQ.
    updateEqParameters();
    digitalEq.processInPlace (L, R, numSamples);

    // Feed the UI spectrum analyzer the post-EQ signal so EQ moves are
    // visible in the overlay (the analyzer lives on the mastering EQ panel).
    pushScope (L, R, numSamples);

#if DUSKSTUDIO_HAS_DUSK_DSP
    {
        // Out, the plug-in costs nothing: only the delay beside it runs. The
        // plug-in takes the session's values while it runs and while its
        // editor, which reads values back from it, is open; its own bypass
        // stays off, since being out is the delay's job.
        const bool compOn = paramsRef != nullptr
                         && paramsRef->compEnabled.load (std::memory_order_relaxed);
        if (paramsRef != nullptr && (compOn || compAlign.compressorRunning() || comp.editorOpen()))
            comp.pushParameters (paramsRef->comp, true, 0);
        compAlign.process (L, R, numSamples, compClock.block(), compOn, &comp);
        if (paramsRef != nullptr)
            paramsRef->meterCompGrDb.store (compAlign.compressorRunning() ? comp.gainReductionDb() : 0.0f,
                                             std::memory_order_relaxed);
    }
#endif

    updateLimiterParameters();
    limiter.processInPlace (L, R, numSamples);

    if (capturePhase)
        phaseFeed.push (L, R, numSamples);
    else
        phaseFeed.markDiscontinuity();

    if (paramsRef != nullptr)
        paramsRef->meterLimiterGrDb.store (limiter.getCurrentGrDb(),
                                            std::memory_order_relaxed);

    // Output meters - peak per channel post-limiter (equivalent to "true
    // peak" at sample resolution; ISP is a follow-up).
    float peakL = 0.0f, peakR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > peakL) peakL = aL;
        if (aR > peakR) peakR = aR;
        sumSqL += L[i] * L[i];
        sumSqR += R[i] * R[i];
    }
    if (paramsRef != nullptr)
    {
        const auto toDb = [] (float a) { return a > 1.0e-5f
            ? dusk::audio::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostMasterLDb.store (toDb (peakL), std::memory_order_relaxed);
        paramsRef->meterPostMasterRDb.store (toDb (peakR), std::memory_order_relaxed);

        const float invN    = 1.0f / (float) std::max (1, numSamples);
        const float rmsBlkL = std::sqrt (sumSqL * invN);
        const float rmsBlkR = std::sqrt (sumSqR * invN);
        const float alpha   = (numSamples == meterBlockSize)
                                ? meterRmsAlpha
                                : std::exp (-((float) numSamples / (float) sampleRateForMeter) / 0.3f);
        vuRmsLinL = alpha * vuRmsLinL + (1.0f - alpha) * rmsBlkL;
        vuRmsLinR = alpha * vuRmsLinR + (1.0f - alpha) * rmsBlkR;
        paramsRef->meterPostMasterRmsL.store (vuRmsLinL, std::memory_order_relaxed);
        paramsRef->meterPostMasterRmsR.store (vuRmsLinR, std::memory_order_relaxed);
    }

    // Loudness - measured on the post-limiter, post-output signal so the
    // user sees what's actually committed when they Export.
    loudnessMeter.process (L, R, numSamples);
    if (paramsRef != nullptr)
    {
        paramsRef->meterMomentaryLufs.store  (loudnessMeter.getMomentaryLufs(),  std::memory_order_relaxed);
        paramsRef->meterShortTermLufs.store  (loudnessMeter.getShortTermLufs(),  std::memory_order_relaxed);
        paramsRef->meterIntegratedLufs.store (loudnessMeter.getIntegratedLufs(), std::memory_order_relaxed);
        paramsRef->meterTruePeakDb.store     (loudnessMeter.getTruePeakDb(),    std::memory_order_relaxed);
    }
}
} // namespace duskstudio
