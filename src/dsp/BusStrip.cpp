#include "BusStrip.h"
#include <cmath>
#include <cstring>

namespace duskstudio
{
void BusStrip::bind (const BusParams& params) noexcept
{
    paramsRef = &params;
}

void BusStrip::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    sampleRateForMeter = sampleRate > 0.0 ? sampleRate : 44100.0;
    vuRmsLinL = vuRmsLinR = 0.0f;
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);
    panGainL .reset (sampleRate, 0.020);
    panGainR .reset (sampleRate, 0.020);
    panGainL .setCurrentAndTargetValue (1.0f);
    panGainR .setCurrentAndTargetValue (1.0f);

    // Bus oversampling: wrap (EQ + UC) externally. UC's internal toggle is
    // disabled because Dusk Studio does the up/downsample around the chain — having
    // UC oversample on top would compound. EQ has no internal toggle, so the
    // external wrap is the only way to suppress its console-saturation
    // aliasing. Two stages = 4×, one stage = 2×, none = 1× (no oversampling).
    const int factor = (oversamplingFactor == 2 || oversamplingFactor == 4)
                            ? oversamplingFactor : 1;
    oversamplerStages = (factor == 4) ? 2 : (factor == 2) ? 1 : 0;
    const int bsClamped = juce::jmax (1, blockSize);
    if (oversamplerStages > 0)
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            2, (size_t) oversamplerStages,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            /*isMaximumQuality*/ true);
        oversampler->initProcessing ((size_t) bsClamped);
        oversampler->reset();
    }
    else
    {
        oversampler.reset();
    }

    const double prepSr = sampleRate * (double) factor;
    const int    prepBs = bsClamped * factor;

#if DUSKSTUDIO_HAS_DUSK_DSP
    eq.prepare (prepSr, prepBs, 2);
    eq.reset();

    busComp.setPlayConfigDetails (2, 2, prepSr, prepBs);
    busComp.prepareToPlay (prepSr, prepBs);
    // External wrap handles oversampling; UC's internal toggle is OFF.
    busComp.setInternalOversamplingEnabled (false);
    compStereoBuffer.setSize (2, prepBs, false, false, true);
    compMidi.clear();
    bindCompParams();
#endif
}

#if DUSKSTUDIO_HAS_DUSK_DSP
void BusStrip::bindCompParams() noexcept
{
    auto& apvts = busComp.getParameters();
    compModeAtom       = apvts.getRawParameterValue ("mode");
    compBypassAtom     = apvts.getRawParameterValue ("bypass");
    compMixAtom        = apvts.getRawParameterValue ("mix");
    compAutoMakeupAtom = apvts.getRawParameterValue ("auto_makeup");
    compBusThreshAtom  = apvts.getRawParameterValue ("bus_threshold");
    compBusRatioAtom   = apvts.getRawParameterValue ("bus_ratio");
    compBusAttackAtom  = apvts.getRawParameterValue ("bus_attack");
    compBusReleaseAtom = apvts.getRawParameterValue ("bus_release");
    compBusMakeupAtom  = apvts.getRawParameterValue ("bus_makeup");
    compBusMixAtom     = apvts.getRawParameterValue ("bus_mix");

    storeAtom (compModeAtom, 3.0f);            // Bus mode
    storeAtom (compMixAtom,         100.0f);
    storeAtom (compBusMixAtom,      100.0f);
    storeAtom (compAutoMakeupAtom,    0.0f);
    // No injected analog hiss under signal — see MasterBus::bindCompParams.
    storeAtom (apvts.getRawParameterValue ("noise_enable"), 0.0f);
}

void BusStrip::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;
    // Value-init padding to zero so the memcmp cache against lastEqParams
    // is reliable - see ChannelStrip equivalent for full reasoning.
    BritishEQProcessor::Parameters p {};
    p.hpfEnabled = false; p.hpfFreq = 80.0f;
    p.lpfEnabled = false; p.lpfFreq = 20000.0f;

    // Bus EQ frequencies + gain range mirror Harrison Mixbus's mix-bus Tone EQ
    // exactly: LO 300 Hz Q1.0 shelf / MID 800 Hz Q0.7 bell / HI 2 kHz Q1.0
    // shelf, +/-9 dB — wide, musical tone-shaping for subgroups. (NOT the
    // narrower Mixbus master-bus EQ, which is 90/300/4000 Hz +/-6 dB.)
    p.lfGain  = paramsRef->eqLfGainDb.load  (std::memory_order_relaxed);
    p.lfFreq  = 300.0f;
    p.lfBell  = false;  // shelf

    p.lmGain  = paramsRef->eqMidGainDb.load (std::memory_order_relaxed);
    p.lmFreq  = 800.0f;
    p.lmQ     = 0.7f;

    p.hmGain  = 0.0f;  // HM unused; Bus EQ exposes only LF / MID / HF.
    p.hmFreq  = 4000.0f;
    p.hmQ     = 0.7f;

    p.hfGain  = paramsRef->eqHfGainDb.load  (std::memory_order_relaxed);
    p.hfFreq  = 2000.0f;
    p.hfBell  = false;

    p.isBlackMode = false;
    p.saturation  = 0.0f;
    p.inputGain   = 0.0f;
    p.outputGain  = 0.0f;
    if (std::memcmp (&p, &lastEqParams, sizeof (p)) != 0)
    {
        eq.setParameters (p);
        lastEqParams = p;
    }
}

void BusStrip::updateCompParameters() noexcept
{
    if (paramsRef == nullptr) return;
    storeAtom (compBypassAtom,
               paramsRef->compEnabled.load (std::memory_order_relaxed) ? 0.0f : 1.0f);
    storeAtom (compBusThreshAtom,  paramsRef->compThreshDb.load   (std::memory_order_relaxed));
    // The donor's bus_ratio / bus_attack / bus_release are SSL-style stepped
    // Choice params, NOT continuous. Storing the raw knob value treated it as
    // an out-of-range index (ratio 4.0 -> 2:1, attack 10 ms -> 30 ms). Map to
    // the nearest discrete index — same as a real SSL bus comp's stepped knobs.
    //   bus_ratio:  0=2:1, 1=4:1, 2=10:1
    //   bus_attack: 0=0.1  1=0.3  2=1  3=3  4=10  5=30  (ms)
    const float ratio  = paramsRef->compRatio.load (std::memory_order_relaxed);
    storeAtom (compBusRatioAtom, ratio < 3.0f ? 0.0f : (ratio < 7.0f ? 1.0f : 2.0f));
    const float atkMs  = paramsRef->compAttackMs.load (std::memory_order_relaxed);
    storeAtom (compBusAttackAtom,
               atkMs < 0.2f ? 0.0f
             : (atkMs < 0.6f ? 1.0f
             : (atkMs < 2.0f ? 2.0f
             : (atkMs < 6.0f ? 3.0f
             : (atkMs < 20.0f ? 4.0f : 5.0f)))));
    // bus_release is a Choice {0.1s, 0.3s, 0.6s, 1.2s, Auto}; see
    // MasterBus::updateCompParameters for the mapping rationale.
    const bool autoRel = paramsRef->compReleaseAuto.load (std::memory_order_relaxed);
    const float relMs  = paramsRef->compReleaseMs.load   (std::memory_order_relaxed);
    const float relIdx = autoRel ? 4.0f
                       : (relMs < 200.0f ? 0.0f
                       : (relMs < 450.0f ? 1.0f
                       : (relMs < 900.0f ? 2.0f : 3.0f)));
    storeAtom (compBusReleaseAtom, relIdx);
    storeAtom (compBusMakeupAtom,  paramsRef->compMakeupDb.load   (std::memory_order_relaxed));
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
                       : juce::Decibels::decibelsToGain (faderDb);
    faderGain.setTargetValue (gain);

    // Equal-power L/R balance - pan -1..1 → angle 0..pi/2.
    const float p     = juce::jlimit (-1.0f, 1.0f, paramsRef->livePan.load (std::memory_order_relaxed));
    const float angle = (p + 1.0f) * (juce::MathConstants<float>::halfPi * 0.5f);
    panGainL.setTargetValue (std::cos (angle) * juce::MathConstants<float>::sqrt2);
    panGainR.setTargetValue (std::sin (angle) * juce::MathConstants<float>::sqrt2);
}

void BusStrip::processInPlace (float* L, float* R, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

   #if DUSKSTUDIO_HAS_DUSK_DSP
    // Contract: numSamples must fit the buffer prepare() sized for the comp.
    // The chunk loop below is the production safety net.
    jassert (numSamples <= compStereoBuffer.getNumSamples());
   #endif

    updateGainTargets();

#if DUSKSTUDIO_HAS_DUSK_DSP
    updateEqParameters();
    updateCompParameters();

    if (oversamplerStages > 0 && oversampler != nullptr)
    {
        // Oversampled chain: upsample (L, R), run EQ + UC at oversampled
        // rate, downsample back into (L, R). EQ and UC were prepared at
        // sampleRate × factor in prepare() so their coefficients are
        // correct for the upsampled rate.
        const float* readPtrs[2]  = { L, R };
        float*       writePtrs[2] = { L, R };
        juce::dsp::AudioBlock<const float> nativeIn  (readPtrs,  2, (size_t) numSamples);
        juce::dsp::AudioBlock<float>       nativeOut (writePtrs, 2, (size_t) numSamples);

        auto upBlock = oversampler->processSamplesUp (nativeIn);
        const int upN = (int) upBlock.getNumSamples();
        float* upPtrs[2] = { upBlock.getChannelPointer (0),
                              upBlock.getChannelPointer (1) };
        juce::AudioBuffer<float> upBuf (upPtrs, 2, upN);
        eq.process (upBuf);

        const int compBufSize = compStereoBuffer.getNumSamples();
        for (int offset = 0; offset < upN; offset += compBufSize)
        {
            const int n = juce::jmin (compBufSize, upN - offset);
            float* compView[2] = { upPtrs[0] + offset, upPtrs[1] + offset };
            juce::AudioBuffer<float> compBuf (compView, 2, n);
            compMidi.clear();
            busComp.processBlock (compBuf, compMidi);
        }

        oversampler->processSamplesDown (nativeOut);
    }
    else
    {
        // Native-rate path (factor == 1).
        float* channels[2] = { L, R };
        juce::AudioBuffer<float> buf (channels, 2, numSamples);
        eq.process (buf);

        const int bufSize = compStereoBuffer.getNumSamples();
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            float* lrView[2] = { L + offset, R + offset };
            juce::AudioBuffer<float> compBuf (lrView, 2, n);
            compMidi.clear();
            busComp.processBlock (compBuf, compMidi);
        }
    }

    if (paramsRef != nullptr)
        paramsRef->meterGrDb.store (busComp.getGainReduction(),
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
            ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostBusLDb.store (toDb (postPeakL), std::memory_order_relaxed);
        paramsRef->meterPostBusRDb.store (toDb (postPeakR), std::memory_order_relaxed);

        const float invN     = 1.0f / (float) juce::jmax (1, numSamples);
        const float rmsBlkL  = std::sqrt (sumSqL * invN);
        const float rmsBlkR  = std::sqrt (sumSqR * invN);
        const float dt       = (float) numSamples / (float) sampleRateForMeter;
        const float alpha    = std::exp (-dt / 0.3f);   // 300 ms VU integration
        vuRmsLinL = alpha * vuRmsLinL + (1.0f - alpha) * rmsBlkL;
        vuRmsLinR = alpha * vuRmsLinR + (1.0f - alpha) * rmsBlkR;
        paramsRef->meterPostBusRmsL.store (vuRmsLinL, std::memory_order_relaxed);
        paramsRef->meterPostBusRmsR.store (vuRmsLinR, std::memory_order_relaxed);
    }
}
} // namespace duskstudio
