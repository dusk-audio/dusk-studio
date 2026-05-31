#include "MasterBus.h"
#include <cmath>
#include <cstring>

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
    tape.prepareToPlay (sampleRate, juce::jmax (1, blockSize));
    tapeStereoBuffer.setSize (2, juce::jmax (1, blockSize), false, false, true);
    tapeMidi.clear();
    tapeBypassAtom = tape.getAPVTS().getRawParameterValue ("bypass");

    // Drive TapeMachine's internal oversampling from the global Audio Settings
    // factor (Session::oversamplingFactor). The donor's "oversampling" param
    // is a 3-choice (1x / 2x / 4x); map Dusk Studio's 1/2/4 factor to indices 0/1/2.
    // Use AudioParameterChoice::operator= so JUCE's APVTS listeners fire and
    // any open editor's combo-box attachment updates — writing the raw atom
    // directly would skip the notification and leave the UI showing stale
    // state (combo would still read 4x even after we set 1x in MasterBus).
    if (auto* osParam = dynamic_cast<juce::AudioParameterChoice*> (
            tape.getAPVTS().getParameter ("oversampling")))
    {
        const int idx = (currentOxFactor == 4) ? 2
                     : (currentOxFactor == 2) ? 1
                                              : 0;
        *osParam = idx;
    }

    // Master oversampler around (TubeEQ + busComp). Both stages have donor
    // saturation that aliases at native rate; the wrap moves them to
    // oversampled rate. UC's internal toggle is OFF here because Dusk Studio
    // does the up/downsample around the chain. TapeMachine has its own
    // internal oversampling (driven via APVTS above) so it processes at
    // native rate AFTER this wrap.
    oversamplerStages = (currentOxFactor == 4) ? 2 : (currentOxFactor == 2) ? 1 : 0;
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

    const double prepSr = sampleRate * (double) currentOxFactor;
    const int    prepBs = bsClamped * currentOxFactor;

    tubeEQ.prepare (prepSr, prepBs, 2);
    tubeEQ.reset();

    busComp.setPlayConfigDetails (2, 2, prepSr, prepBs);
    busComp.prepareToPlay (prepSr, prepBs);
    busComp.setInternalOversamplingEnabled (false);  // External wrap handles oversampling.
    compStereoBuffer.setSize (2, prepBs, false, false, true);
    compMidi.clear();
    bindCompParams();
#endif

    preparedBlockSize = blockSize;
}

#if DUSKSTUDIO_HAS_DUSK_DSP
void MasterBus::bindCompParams()
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

    // Lock the comp into Bus mode (CompressorMode::Bus = 3); set wet-only mix.
    storeAtom (compModeAtom, 3.0f);
    storeAtom (compMixAtom,         100.0f);
    storeAtom (compBusMixAtom,      100.0f);
    storeAtom (compAutoMakeupAtom,    0.0f);  // Off
    // The donor's "Analog Noise" feature injects ~-80 dB white noise on every
    // analog mode (Bus included) and defaults ON. The master chain runs the
    // comp every block whenever it's engaged, so that hiss would sit on the
    // output continuously (~-67 dB peak, two channels) and print into bounces.
    // Force it off — Dusk Studio's master bus is meant to be clean.
    storeAtom (apvts.getRawParameterValue ("noise_enable"), 0.0f);
}

void MasterBus::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;

    // Value-init padding to zero so the memcmp cache against lastTubeEqParams
    // is reliable - see ChannelStrip equivalent for full reasoning.
    TubeEQProcessor::Parameters p {};
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
    if (std::memcmp (&p, &lastTubeEqParams, sizeof (p)) != 0)
    {
        tubeEQ.setParameters (p);
        lastTubeEqParams = p;
    }
}

void MasterBus::updateCompParameters() noexcept
{
    if (paramsRef == nullptr) return;

    storeAtom (compBypassAtom,
               paramsRef->compEnabled.load (std::memory_order_relaxed) ? 0.0f : 1.0f);
    storeAtom (compBusThreshAtom,  paramsRef->compThreshDb.load   (std::memory_order_relaxed));
    // bus_ratio / bus_attack are SSL-style stepped Choice params, NOT
    // continuous. Storing the raw knob value treated it as an out-of-range
    // index (ratio 4.0 -> 2:1, attack 10 ms -> 30 ms). Map to the nearest
    // discrete index.  bus_ratio: 0=2:1 1=4:1 2=10:1.  bus_attack: 0=0.1
    // 1=0.3 2=1 3=3 4=10 5=30 ms.
    const float ratio  = paramsRef->compRatio.load (std::memory_order_relaxed);
    storeAtom (compBusRatioAtom, ratio < 3.0f ? 0.0f : (ratio < 7.0f ? 1.0f : 2.0f));
    const float atkMs  = paramsRef->compAttackMs.load (std::memory_order_relaxed);
    storeAtom (compBusAttackAtom,
               atkMs < 0.2f ? 0.0f
             : (atkMs < 0.6f ? 1.0f
             : (atkMs < 2.0f ? 2.0f
             : (atkMs < 6.0f ? 3.0f
             : (atkMs < 20.0f ? 4.0f : 5.0f)))));
    // The donor's bus_release is a Choice param indexed 0..4 over
    // {0.1s, 0.3s, 0.6s, 1.2s, Auto}. Map Dusk Studio's continuous release knob
    // to the nearest discrete index, or send 4 ("Auto") when the Auto
    // toggle is engaged so the comp uses its program-dependent envelope.
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

void MasterBus::processInPlace (float* L, float* R, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    // Contract: numSamples must not exceed what was passed to prepare().
    // If a host violates this, our chunk loop below correctly handles the
    // comp portion; the assert just makes the violation visible in debug.
    jassert (numSamples <= preparedBlockSize);

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
                           : juce::Decibels::decibelsToGain (faderDb);
        faderGain.setTargetValue (gain);
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    updateEqParameters();
    updateCompParameters();

    if (oversamplerStages > 0 && oversampler != nullptr)
    {
        // Oversampled path. Wrap (TubeEQ + busComp) inside the up/down so
        // their saturation is band-limited before downsampling. EQ and UC
        // were prepped at oversampled rate / block size in prepare().
        const float* readPtrs[2]  = { L, R };
        float*       writePtrs[2] = { L, R };
        juce::dsp::AudioBlock<const float> nativeIn  (readPtrs,  2, (size_t) numSamples);
        juce::dsp::AudioBlock<float>       nativeOut (writePtrs, 2, (size_t) numSamples);

        auto upBlock = oversampler->processSamplesUp (nativeIn);
        const int upN = (int) upBlock.getNumSamples();
        float* upPtrs[2] = { upBlock.getChannelPointer (0),
                              upBlock.getChannelPointer (1) };
        juce::AudioBuffer<float> upBuf (upPtrs, 2, upN);
        tubeEQ.process (upBuf);

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
        // Native-rate path. EQ + comp run at sample rate; chunk both passes
        // to honor preparedBlockSize / compStereoBuffer sizes.
        const int eqBuf = juce::jmax (1, preparedBlockSize);
        for (int offset = 0; offset < numSamples; offset += eqBuf)
        {
            const int n = juce::jmin (eqBuf, numSamples - offset);
            float* channels[2] = { L + offset, R + offset };
            juce::AudioBuffer<float> buf (channels, 2, n);
            tubeEQ.process (buf);
        }
        const int compBuf = compStereoBuffer.getNumSamples();
        for (int offset = 0; offset < numSamples; offset += compBuf)
        {
            const int n = juce::jmin (compBuf, numSamples - offset);
            float* lrView[2] = { L + offset, R + offset };
            juce::AudioBuffer<float> cb (lrView, 2, n);
            compMidi.clear();
            busComp.processBlock (cb, compMidi);
        }
    }

    if (paramsRef != nullptr)
        paramsRef->meterGrDb.store (busComp.getGainReduction(),
                                     std::memory_order_relaxed);
#endif

#if DUSKSTUDIO_HAS_DUSK_DSP
    // TapeMachine handles its own internal oversampling (driven from the
    // global factor via its `oversampling` APVTS param, written in
    // prepare()). The TAPE button toggles the donor's `bypass` atom -
    // when bypassed, the donor's processBlock returns the dry signal.
    // Always call processBlock so the donor's bypass crossfade machinery
    // produces clean transitions; chunk by preparedBlockSize because the
    // donor's internal scratch is sized to that.
    storeAtom (tapeBypassAtom, tapeOn ? 0.0f : 1.0f);
    {
        const int bufSize = tapeStereoBuffer.getNumSamples();
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            float* lrView[2] = { L + offset, R + offset };
            juce::AudioBuffer<float> tapeBuf (lrView, 2, n);
            tapeMidi.clear();
            tape.processBlock (tapeBuf, tapeMidi);
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
            ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostMasterLDb.store (toDb (postPeakL), std::memory_order_relaxed);
        paramsRef->meterPostMasterRDb.store (toDb (postPeakR), std::memory_order_relaxed);

        const float invN    = 1.0f / (float) juce::jmax (1, numSamples);
        const float rmsBlkL = std::sqrt (sumSqL * invN);
        const float rmsBlkR = std::sqrt (sumSqR * invN);
        const float dt      = (float) numSamples / (float) sampleRateForMeter;
        const float alpha   = std::exp (-dt / 0.3f);
        vuRmsLinL = alpha * vuRmsLinL + (1.0f - alpha) * rmsBlkL;
        vuRmsLinR = alpha * vuRmsLinR + (1.0f - alpha) * rmsBlkR;
        paramsRef->meterPostMasterRmsL.store (vuRmsLinL, std::memory_order_relaxed);
        paramsRef->meterPostMasterRmsR.store (vuRmsLinR, std::memory_order_relaxed);
    }
}
} // namespace duskstudio
