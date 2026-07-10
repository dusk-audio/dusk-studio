#include "MasterBus.h"
#include "../foundation/Decibels.h"
#include "../foundation/ScopedNoDenormals.h"
#include <algorithm>
#include <cmath>

namespace duskstudio
{
MasterBus::MasterBus() = default;

void MasterBus::bind (const MasterBusParams& params) noexcept
{
    paramsRef = &params;
}

void MasterBus::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    sampleRateForMeter = sampleRate > 0.0 ? sampleRate : 44100.0;
    vuRmsLinL = vuRmsLinR = 0.0f;
    {
        const int bs = std::max (1, blockSize);
        meterBlockSize = bs;
        meterRmsAlpha  = std::exp (-((float) bs / (float) sampleRateForMeter) / 0.3f);
    }
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);

    // Clamp to the supported set. Anything else falls back to native rate.
    currentOxFactor = (oversamplingFactor == 2 || oversamplingFactor == 4)
                       ? oversamplingFactor : 1;

#if DUSKSTUDIO_HAS_DUSK_DSP
    // TapeMachine is a full juce::AudioProcessor instance. Configure its bus
    // layout, prepare it for the working SR/block size, and pre-size the
    // scratch buffer used to feed processBlock each callback. The bypass
    // APVTS atom is cached here so the audio thread can flip it lock-free.
    tape.setPlayConfigDetails (2, 2, sampleRate, juce::jmax (1, blockSize));

    // Drive TapeMachine's internal oversampling from the global Audio Settings
    // factor (Session::oversamplingFactor). The donor's "oversampling" param
    // is a 3-choice (1x / 2x / 4x); map Dusk Studio's 1/2/4 factor to indices 0/1/2.
    // Set this BEFORE prepareToPlay: the donor reads the param there to size its
    // oversampler and report its latency, so setting it first means
    // getLatencySamples() below is correct without waiting for a processBlock to
    // re-sync. operator= so JUCE's listeners fire and any open editor's combo
    // updates (raw-atom write would skip the notification and leave it stale).
    if (auto* osParam = dynamic_cast<juce::AudioParameterChoice*> (
            tape.getAPVTS().getParameter ("oversampling")))
    {
        const int idx = (currentOxFactor == 4) ? 2
                     : (currentOxFactor == 2) ? 1
                                              : 0;
        *osParam = idx;
    }

    tape.prepareToPlay (sampleRate, juce::jmax (1, blockSize));
    tapeStereoBuffer.setSize (2, juce::jmax (1, blockSize), false, false, true);
    tapeDryBuffer.setSize (2, juce::jmax (1, blockSize), false, false, true);
    tapeMidi.clear();
    tapeBypassAtom = tape.getAPVTS().getRawParameterValue ("bypass");

    // 20 ms toggle crossfade. Primed to the live tape state on the first block
    // (prepare can't see paramsRef reliably) so loading a session with tape ON
    // doesn't fade in from dry.
    tapeMix.reset (sampleRate, 0.020);
    tapeMixPrimed = false;

    // Resolve the tape's engaged latency (0 at 1×) now that prepareToPlay has
    // set it from the factor above, and size the dry delay to match so the
    // crossfade is phase-coherent and bit-perfect.
    tapeLatencySamples = juce::jmax (0, tape.getLatencySamples());
    {
        const int maxDelay = juce::jmax (1, tapeLatencySamples);
        const juce::dsp::ProcessSpec drySpec {
            sampleRate, (std::uint32_t) juce::jmax (1, blockSize), 1 };
        tapeDryDelayL.prepare (drySpec);
        tapeDryDelayR.prepare (drySpec);
        tapeDryDelayL.setMaximumDelayInSamples (maxDelay);
        tapeDryDelayR.setMaximumDelayInSamples (maxDelay);
        tapeDryDelayL.setDelay ((float) tapeLatencySamples);
        tapeDryDelayR.setDelay ((float) tapeLatencySamples);
        tapeDryDelayL.reset();
        tapeDryDelayR.reset();
    }

    // Master oversampler around (TubeEQ + busComp). Both stages have donor
    // saturation that aliases at native rate; the wrap moves them to
    // oversampled rate. The comp core's internal oversampling path is never
    // engaged because Dusk Studio does the up/downsample around the chain.
    // TapeMachine has its own internal oversampling (driven via APVTS above) so
    // it processes at native rate AFTER this wrap.
    const int bsClamped = std::max (1, blockSize);
    oversampler.setFactor (currentOxFactor);
    oversampler.prepare (bsClamped);
    osLatencySamples = (currentOxFactor > 1)
        ? std::min (kMaxOsLatency, (int) std::lround (oversampler.latency()))
        : 0;

    osSkipDelayL.setMaximumDelayInSamples (kMaxOsLatency);
    osSkipDelayR.setMaximumDelayInSamples (kMaxOsLatency);
    osSkipDelayL.setDelay (osLatencySamples);
    osSkipDelayR.setDelay (osLatencySamples);
    osSkipDelayL.reset();
    osSkipDelayR.reset();

    const double prepSr = sampleRate * (double) currentOxFactor;
    const int    prepBs = bsClamped * currentOxFactor;

    tubeEQ.prepare (prepSr, prepBs, 2);
    tubeEQ.reset();

    busComp.setMode (3);            // Bus mode
    busComp.setMix (100.0f);
    busComp.setBusMix (100.0f);
    busComp.setAutoMakeup (false);
    // The core does not port the donor's analog-noise stage, so no explicit
    // force-off is needed — the master chain stays clean under signal.
    busComp.prepare (prepSr, prepBs);
    busComp.reset();
    compMaxBlock = prepBs;
#endif
}

#if DUSKSTUDIO_HAS_DUSK_DSP
void MasterBus::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;

    duskaudio::MultiQTube::Parameters p {};
    p.lfBoostGain      = paramsRef->eqLfBoost.load (std::memory_order_relaxed);
    p.lfBoostFreq      = paramsRef->eqLfFreq.load (std::memory_order_relaxed);
    p.lfAttenGain      = paramsRef->eqLfAtten.load (std::memory_order_relaxed);
    p.hfBoostGain      = paramsRef->eqHfBoost.load (std::memory_order_relaxed);
    p.hfBoostFreq      = paramsRef->eqHfBoostFreq.load (std::memory_order_relaxed);
    p.hfBoostBandwidth = paramsRef->eqHfBoostBandwidth.load (std::memory_order_relaxed);
    p.hfAttenGain      = paramsRef->eqHfAtten.load (std::memory_order_relaxed);
    p.hfAttenFreq      = paramsRef->eqHfAttenFreq.load (std::memory_order_relaxed);
    p.midEnabled       = false;  // Mid Dip/Peak section disabled at master - Pultec users
                                  // typically reach for tube drive instead at the master bus.
    p.midLowFreq = 500.0f;  p.midLowPeak = 0.0f;
    p.midDipFreq = 700.0f;  p.midDip = 0.0f;
    p.midHighFreq = 3000.0f; p.midHighPeak = 0.0f;
    p.inputGain  = 0.0f;
    p.outputGain = paramsRef->eqOutputGainDb.load (std::memory_order_relaxed);
    // Fixed tube drive calibrated to ~-70 dB H2 at 0 dBFS sine input, matching
    // a UAD EQP-1A at +0 dB. With the donor TubeEQTubeStage polynomial
    // (H2 ≈ b·drive·DRIVE_SCALE·A/2, b=0.015, DRIVE_SCALE=2), drive=0.02 yields
    // 0.015·0.04·0.5 = 3e-4 → 20·log10(3e-4) ≈ -70.5 dB H2 at 0 dBFS. H3 sits
    // ~50 dB below (polynomial physics, H3 ∝ drive²·A²). The legacy user-
    // facing Drive knob is gone — the master tube stage is now a fixed
    // "warm but not saturated" character, like a real EQP-1A at unity.
    p.tubeDrive  = 0.02f;
    p.bypass     = ! paramsRef->eqEnabled.load (std::memory_order_relaxed);
    // The core's setParameters self-gates on operator!= (marks filters dirty
    // only on an actual change), preserving the "updateFilters only on change"
    // semantics that keep the end-of-block HF inductor-Q remodulation persistent
    // — no external memcmp cache needed.
    tubeEQ.setParameters (p);
}

void MasterBus::updateCompParameters() noexcept
{
    if (paramsRef == nullptr) return;

    busComp.setBypass (! paramsRef->compEnabled.load (std::memory_order_relaxed));
    busComp.setBusThreshold (paramsRef->compThreshDb.load (std::memory_order_relaxed));
    // bus_ratio / bus_attack are SSL-style stepped Choice params, NOT
    // continuous. Map the raw knob value to the nearest discrete index.
    //   bus_ratio: 0=2:1 1=4:1 2=10:1.  bus_attack: 0=0.1 1=0.3 2=1 3=3 4=10
    //   5=30 ms.
    const float ratio  = paramsRef->compRatio.load (std::memory_order_relaxed);
    busComp.setBusRatio (ratio < 3.0f ? 0 : (ratio < 7.0f ? 1 : 2));
    const float atkMs  = paramsRef->compAttackMs.load (std::memory_order_relaxed);
    busComp.setBusAttack (atkMs < 0.2f ? 0
                        : (atkMs < 0.65f ? 1
                        : (atkMs < 2.0f ? 2
                        : (atkMs < 6.5f ? 3
                        : (atkMs < 20.0f ? 4 : 5)))));
    // bus_release is a Choice indexed 0..4 over {0.1s, 0.3s, 0.6s, 1.2s, Auto}.
    // Map the continuous release knob to the nearest discrete index, or send 4
    // ("Auto") when the Auto toggle is engaged so the comp uses its
    // program-dependent envelope.
    const bool autoRel = paramsRef->compReleaseAuto.load (std::memory_order_relaxed);
    const float relMs  = paramsRef->compReleaseMs.load   (std::memory_order_relaxed);
    busComp.setBusRelease (autoRel ? 4
                         : (relMs < 200.0f ? 0
                         : (relMs < 450.0f ? 1
                         : (relMs < 900.0f ? 2 : 3))));
    busComp.setBusMakeup (paramsRef->compMakeupDb.load (std::memory_order_relaxed));
}
#endif

void MasterBus::processInPlace (float* L, float* R, int numSamples) noexcept
{
    dusk::audio::ScopedNoDenormals noDenormals;

    const bool tapeOn = paramsRef != nullptr
                       && paramsRef->tapeEnabled.load (std::memory_order_relaxed);

    if (paramsRef != nullptr)
    {
        // Reads liveFaderDb (post-automation), not faderDb. Off mode
        // leaves them equal; Read/Touch routes the lane value into
        // liveFaderDb each block from AudioEngine's automation block.
        const float faderDb = paramsRef->liveFaderDb.load (std::memory_order_relaxed);
        const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                           ? 0.0f
                           : dusk::audio::decibelsToGain (faderDb);
        faderGain.setTargetValue (gain);
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    updateEqParameters();
    updateCompParameters();

    // The master tube EQ and bus comp are the only saturating stages inside
    // this wrap (TapeMachine handles its own OS below). When BOTH are bypassed
    // the up/downsample round-trip is pure waste — both donors would just pass
    // the signal through dry — so we skip the oversampler entirely.
    const bool eqOn   = paramsRef != nullptr
                     && paramsRef->eqEnabled.load (std::memory_order_relaxed);
    const bool compOn = paramsRef != nullptr
                     && paramsRef->compEnabled.load (std::memory_order_relaxed);

    if (currentOxFactor > 1 && (eqOn || compOn))
    {
        // Oversampled path. Wrap (TubeEQ + busComp) inside the up/down so their
        // saturation is band-limited before downsampling. Both were prepped at
        // oversampled rate / block size in prepare().
        auto up = oversampler.processSamplesUp (L, R, numSamples);
        if (eqOn)
        {
            float* eqPtrs[2] = { up.L, up.R };
            tubeEQ.process (eqPtrs, 2, up.numSamples);
        }
        if (compOn)
        {
            for (int offset = 0; offset < up.numSamples; offset += compMaxBlock)
            {
                const int n = std::min (compMaxBlock, up.numSamples - offset);
                const float* compIn[2]  = { up.L + offset, up.R + offset };
                float*       compOut[2] = { up.L + offset, up.R + offset };
                busComp.processBlock (compIn, compOut, 2, n);
            }
        }
        oversampler.processSamplesDown (L, R, numSamples);
    }
    else if (eqOn || compOn)
    {
        // Native-rate path (factor == 1). The tube EQ has no block-size limit;
        // chunk only the comp to honor compMaxBlock.
        if (eqOn)
        {
            float* eqPtrs[2] = { L, R };
            tubeEQ.process (eqPtrs, 2, numSamples);
        }
        if (compOn)
        {
            for (int offset = 0; offset < numSamples; offset += compMaxBlock)
            {
                const int n = std::min (compMaxBlock, numSamples - offset);
                const float* compIn[2]  = { L + offset, R + offset };
                float*       compOut[2] = { L + offset, R + offset };
                busComp.processBlock (compIn, compOut, 2, n);
            }
        }
    }
    else if (currentOxFactor > 1 && osLatencySamples > 0)
    {
        // EQ + comp both bypassed → oversampler skipped. Delay by its latency
        // so the master's latency stays invariant to the EQ/comp toggle.
        for (int i = 0; i < numSamples; ++i)
        {
            osSkipDelayL.pushSample (L[i]);  L[i] = osSkipDelayL.popSample();
            osSkipDelayR.pushSample (R[i]);  R[i] = osSkipDelayR.popSample();
        }
    }
    // else (factor == 1, both off): signal passes through. TapeMachine still
    // runs below.

    if (paramsRef != nullptr)
        paramsRef->meterGrDb.store (compOn ? busComp.getGainReduction() : 0.0f,
                                     std::memory_order_relaxed);
#endif

#if DUSKSTUDIO_HAS_DUSK_DSP
    // TapeMachine handles its own internal oversampling (driven from the
    // global factor via its `oversampling` APVTS param, written in prepare()).
    // The donor hard-bypasses (early-returns, no ramp), so we own the on/off
    // crossfade here: blend the dry (pre-tape) signal against the wet output
    // over 20 ms. Tape is run only while audible — fully on, or still fading —
    // so a disengaged tape costs ~nothing. Chunked to the tape scratch size
    // because the donor's internal scratch is sized to the prepared block.
    const float tapeTarget = tapeOn ? 1.0f : 0.0f;
    if (! tapeMixPrimed)
    {
        tapeMix.setCurrentAndTargetValue (tapeTarget);
        tapeMixPrimed = true;
    }
    tapeMix.setTargetValue (tapeTarget);

    const bool blending = tapeMix.isSmoothing();
    const bool runTape  = tapeOn || blending;            // wet needed this block
    const bool alignDry = tapeLatencySamples > 0;        // tape adds latency (2×/4×)

    if (! runTape && ! alignDry)
    {
        // 1× (no latency) and fully faded out → dry passes through untouched
        // (free). Keep the donor's bypass atom set so any open editor agrees.
        storeAtom (tapeBypassAtom, 1.0f);
    }
    else
    {
        storeAtom (tapeBypassAtom, runTape ? 0.0f : 1.0f);

        const int bufSize = tapeStereoBuffer.getNumSamples();
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            float* Lc = L + offset;
            float* Rc = R + offset;

            // Capture the dry (pre-tape) signal. With tape latency present we
            // push it through a matching delay — fed EVERY block so the ring
            // stays warm → seamless next toggle. At 0 latency a plain copy
            // suffices and is only needed while blending.
            if (alignDry)
            {
                for (int i = 0; i < n; ++i)
                {
                    tapeDryDelayL.pushSample (0, Lc[i]);
                    tapeDryDelayR.pushSample (0, Rc[i]);
                    tapeDryBuffer.setSample (0, i, tapeDryDelayL.popSample (0));
                    tapeDryBuffer.setSample (1, i, tapeDryDelayR.popSample (0));
                }
            }
            else if (blending)
            {
                tapeDryBuffer.copyFrom (0, 0, Lc, n);
                tapeDryBuffer.copyFrom (1, 0, Rc, n);
            }

            if (runTape)
            {
                float* lrView[2] = { Lc, Rc };
                juce::AudioBuffer<float> tapeBuf (lrView, 2, n);
                tapeMidi.clear();
                tape.processBlock (tapeBuf, tapeMidi);
            }

            const float* dryL = tapeDryBuffer.getReadPointer (0);
            const float* dryR = tapeDryBuffer.getReadPointer (1);
            if (! runTape)
            {
                // Fully off but latency-compensated → emit the delayed dry so
                // master latency stays constant (no timing jump on re-engage).
                juce::FloatVectorOperations::copy (Lc, dryL, n);
                juce::FloatVectorOperations::copy (Rc, dryR, n);
            }
            else if (blending)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float g = tapeMix.getNextValue();   // 0 = dry, 1 = wet
                    Lc[i] = dryL[i] * (1.0f - g) + Lc[i] * g;
                    Rc[i] = dryR[i] * (1.0f - g) + Rc[i] * g;
                }
            }
            // else fully on → Lc/Rc already hold the wet output.
        }
    }
#endif

    // Master mute + mono-sum live alongside the fader gain: same loop,
    // no extra pass over the buffer. Both are read once at block top
    // (relaxed: UI clicks are message-thread, one-block stale read is
    // benign). Mute zeros L+R + bypasses the meter writes since a
    // muted bus's RMS smoothing should also fall to silence cleanly.
    const bool muteOn = paramsRef != nullptr
                       && paramsRef->mute.load (std::memory_order_relaxed);
    const bool monoOn = paramsRef != nullptr
                       && paramsRef->monoSum.load (std::memory_order_relaxed);

    float postPeakL = 0.0f, postPeakR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = faderGain.getNextValue();
        float lv = L[i] * g;
        float rv = R[i] * g;
        if (monoOn)
        {
            const float m = 0.5f * (lv + rv);
            lv = m;
            rv = m;
        }
        if (muteOn)
        {
            lv = 0.0f;
            rv = 0.0f;
        }
        L[i] = lv;
        R[i] = rv;
        const float aL = std::fabs (lv);
        const float aR = std::fabs (rv);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
        sumSqL += lv * lv;
        sumSqR += rv * rv;
    }

    if (paramsRef != nullptr)
    {
        const auto toDb = [] (float a) { return a > 1.0e-5f
            ? dusk::audio::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostMasterLDb.store (toDb (postPeakL), std::memory_order_relaxed);
        paramsRef->meterPostMasterRDb.store (toDb (postPeakR), std::memory_order_relaxed);

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
}
} // namespace duskstudio
