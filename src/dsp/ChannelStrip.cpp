#include "ChannelStrip.h"
#include <cmath>
#include <cstring>

namespace duskstudio
{
void ChannelStrip::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    constexpr double rampSeconds = 0.020;
    faderGain.reset (sampleRate, rampSeconds);
    panGainL.reset  (sampleRate, rampSeconds);
    panGainR.reset  (sampleRate, rampSeconds);
    faderGain.setCurrentAndTargetValue (0.0f);
    panGainL.setCurrentAndTargetValue (0.7071f);
    panGainR.setCurrentAndTargetValue (0.7071f);
    for (auto& s : busGain)
    {
        s.reset (sampleRate, rampSeconds);
        s.setCurrentAndTargetValue (0.0f);
    }
    for (auto& s : auxSendGain)
    {
        s.reset (sampleRate, rampSeconds);
        s.setCurrentAndTargetValue (0.0f);
    }
    for (auto& b : auxSendPre) b = false;

    tempMono.assign ((size_t) juce::jmax (1, blockSize), 0.0f);
    tempStereoBuffer.setSize (2, juce::jmax (1, blockSize), false, false, true);

    // Hardware insert + the plugin <-> hardware crossfade gate. Same
    // 20 ms ramp as the rest of the strip so the transition feels in
    // tempo with fader/pan smoothing already in place. The gain is
    // initialised to MATCH the active mode so a session that restores
    // insertMode=Empty doesn't audibly fade-down on the first block
    // (the steady-state target for Empty is 0; without this seed the
    // current value would be 1 and the 20 ms ramp would tick down).
    hardwareSlot.prepare (sampleRate, blockSize);
    activeInsertGain.reset (sampleRate, rampSeconds);
    activeInsertMode = insertMode.load (std::memory_order_relaxed);
    activeInsertGain.setCurrentAndTargetValue (
        activeInsertMode == kInsertEmpty ? 0.0f : 1.0f);
    insertScratchL.assign ((size_t) juce::jmax (1, blockSize), 0.0f);
    insertScratchR.assign ((size_t) juce::jmax (1, blockSize), 0.0f);

    // Pre-size the MIDI scratch buffers so the audio thread's addEvent /
    // processBlock calls never grow them. 4 KB covers ~400-800 typical
    // channel-voice messages per block; far above any sane density even
    // for instrument plugins generating dense controller streams.
    pluginMidiScratch.ensureSize (4096);
    compMidiScratch  .ensureSize (4096);

    // Plugin slot - prepared at the same SR/BS so the audio thread never
    // sees an unprepared instance. If the slot has no plugin loaded, this
    // is essentially a no-op; if a plugin is loaded across a device-rate
    // change, the slot re-preps it for the new config.
    pluginSlot.prepareToPlay (sampleRate, juce::jmax (1, blockSize));

    // Oversampling: build a Dusk Studio-side wrapper around (EQ + Comp) when the
    // user picks 2× / 4× in Audio Settings. The donor's BritishEQ console
    // saturation and ChannelComp/UC saturation alias hard at native rate;
    // wrapping the chain with juce::dsp::Oversampling moves the entire
    // per-channel DSP to oversampled rate so the saturation is band-limited
    // before downsampling. EQ + Comp are then prepared at the OVERSAMPLED
    // sample rate / block size so their coefficients and scratch are sized
    // correctly. Two oversampler instances (1ch + 2ch) so mono / stereo
    // tracks each get the right channel count without wasted DSP.
    const int factor = (oversamplingFactor == 2 || oversamplingFactor == 4)
                            ? oversamplingFactor : 1;
    oversamplerStages = (factor == 4) ? 2 : (factor == 2) ? 1 : 0;
    const int bsClamped = juce::jmax (1, blockSize);
    if (oversamplerStages > 0)
    {
        const auto stages = (size_t) oversamplerStages;
        oversamplerMono = std::make_unique<juce::dsp::Oversampling<float>> (
            1, stages,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            /*isMaximumQuality*/ true);
        oversamplerStereo = std::make_unique<juce::dsp::Oversampling<float>> (
            2, stages,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            /*isMaximumQuality*/ true);
        oversamplerMono  ->initProcessing ((size_t) bsClamped);
        oversamplerStereo->initProcessing ((size_t) bsClamped);
        oversamplerMono  ->reset();
        oversamplerStereo->reset();
    }
    else
    {
        oversamplerMono.reset();
        oversamplerStereo.reset();
    }

    const double prepSr = sampleRate * (double) factor;
    const int    prepBs = bsClamped * factor;

#if DUSKSTUDIO_HAS_DUSK_DSP
    // EQ + Comp prep at the OVERSAMPLED rate / block size so their internal
    // filter coefficients + scratch sizes track the upsampled buffer the
    // chain processes when factor > 1. At factor == 1 this collapses to
    // (sampleRate, blockSize), preserving the legacy path.
    eq.prepare (prepSr, prepBs, 2);
    eq.reset();
    // H8 idempotence: re-clear the memcmp cache + the false→true edge
    // detector so a re-prepare (sample-rate change, oversampling toggle,
    // mid-session blockSize bump) forces a fresh setParameters call
    // against the now-reset filter state. Without this, lastEqParams
    // could hold the SAME params as the live paramsRef → memcmp returns
    // 0 → setParameters skipped → filters keep their zero coefficients
    // post-reset → silent EQ.
    lastEqParams  = {};
    prevEqEnabled = true;

    // Force the full multi-comp processing path (sidechain HP, true-peak,
    // transient shaper, stereo linking, auto-makeup, bypass-fade, lookahead)
    // and disable the donor's internal oversampling since Dusk Studio already
    // wraps the strip in its own oversampler.
    compressor.setMinimalProcessing (false);
    compressor.setInternalOversamplingEnabled (false);
    compressor.setPlayConfigDetails (2, 2, prepSr, juce::jmax (1, prepBs));
    compressor.prepareToPlay (prepSr, juce::jmax (1, prepBs));
    // H8 belt-and-braces: explicit reset() after prepareToPlay so any
    // detector / envelope state cached across the prior session
    // (different sample rate, different block size) is wiped to
    // initial conditions. Some donor builds reset internal state
    // inside prepareToPlay, some do not — calling reset() makes the
    // behaviour deterministic across donor revisions and rules out
    // NaN-prone detectors firing on the first post-prepare block.
    compressor.reset();
    bindCompParams();

    // SmoothedValue ramp at the OVERSAMPLED sample rate (the rate the
    // donor sees inside processBlock). 20 ms is the same constant the
    // fader / pan / bus gains use.
    auto seedComp = [prepSr] (juce::SmoothedValue<float>& sv, float v)
    {
        sv.reset (prepSr, 0.020);
        sv.setCurrentAndTargetValue (v);
    };
    if (paramsRef != nullptr)
    {
        seedComp (smoothedOptoPeakRed, paramsRef->compOptoPeakRed.load (std::memory_order_relaxed));
        seedComp (smoothedOptoGain,    paramsRef->compOptoGain   .load (std::memory_order_relaxed));
        seedComp (smoothedFetInput,    paramsRef->compFetInput   .load (std::memory_order_relaxed));
        seedComp (smoothedFetOutput,   paramsRef->compFetOutput  .load (std::memory_order_relaxed));
        seedComp (smoothedFetAttack,   paramsRef->compFetAttack  .load (std::memory_order_relaxed));
        seedComp (smoothedFetRelease,  paramsRef->compFetRelease .load (std::memory_order_relaxed));
        seedComp (smoothedFetThreshold,paramsRef->compFetThresholdDb.load (std::memory_order_relaxed));
        seedComp (smoothedVcaThresh,   paramsRef->compVcaThreshDb.load (std::memory_order_relaxed));
        seedComp (smoothedVcaRatio,    paramsRef->compVcaRatio   .load (std::memory_order_relaxed));
        seedComp (smoothedVcaAttack,   paramsRef->compVcaAttack  .load (std::memory_order_relaxed));
        seedComp (smoothedVcaRelease,  paramsRef->compVcaRelease .load (std::memory_order_relaxed));
        seedComp (smoothedVcaOutput,   paramsRef->compVcaOutput  .load (std::memory_order_relaxed));
    }
#endif
}

#if DUSKSTUDIO_HAS_DUSK_DSP
void ChannelStrip::bindCompParams()
{
    auto& apvts = compressor.getParameters();
    // getRawParameterValue() returns a pointer to the parameter's denormalised
    // value atomic - the same atomic that UniversalCompressor's processBlock()
    // reads. Writing here is lock-free and notification-free, suitable for the
    // audio thread. Stores hold SI-unit / index / 0-or-1 values.
    compModeAtom        = apvts.getRawParameterValue ("mode");
    compBypassAtom      = apvts.getRawParameterValue ("bypass");
    compMixAtom         = apvts.getRawParameterValue ("mix");
    compAutoMakeupAtom  = apvts.getRawParameterValue ("auto_makeup");
    compScHpAtom        = apvts.getRawParameterValue ("sidechain_hp");
    compOptoPeakRedAtom = apvts.getRawParameterValue ("opto_peak_reduction");
    compOptoGainAtom    = apvts.getRawParameterValue ("opto_gain");
    compOptoLimitAtom   = apvts.getRawParameterValue ("opto_limit");
    compFetInputAtom     = apvts.getRawParameterValue ("fet_input");
    compFetOutputAtom    = apvts.getRawParameterValue ("fet_output");
    compFetAttackAtom    = apvts.getRawParameterValue ("fet_attack");
    compFetReleaseAtom   = apvts.getRawParameterValue ("fet_release");
    compFetRatioAtom     = apvts.getRawParameterValue ("fet_ratio");
    compFetThresholdAtom = apvts.getRawParameterValue ("fet_threshold");
    compVcaThreshAtom   = apvts.getRawParameterValue ("vca_threshold");
    compVcaRatioAtom    = apvts.getRawParameterValue ("vca_ratio");
    compVcaAttackAtom   = apvts.getRawParameterValue ("vca_attack");
    compVcaReleaseAtom  = apvts.getRawParameterValue ("vca_release");
    compVcaOutputAtom   = apvts.getRawParameterValue ("vca_output");
    compVcaOverEasyAtom = apvts.getRawParameterValue ("vca_overeasy");
    compVcaDetectorModeAtom = apvts.getRawParameterValue ("vca_detector_mode");

    // Mix=100% wet, auto-makeup off (we control makeup via per-mode output param).
    storeAtom (compMixAtom,        100.0f);
    storeAtom (compAutoMakeupAtom,   0.0f);  // Choice index 0 = "Off"
    // Donor's "Analog Noise" injects ~-80 dB white noise on every analog mode
    // (FET / Opto / VCA) and defaults ON — force it off so an engaged channel
    // comp doesn't raise the noise floor. See MasterBus::bindCompParams.
    storeAtom (apvts.getRawParameterValue ("noise_enable"), 0.0f);
}
#endif

void ChannelStrip::updateGainTargets() noexcept
{
    if (paramsRef == nullptr) return;

    // Read liveFaderDb / liveMute, not faderDb / mute: AudioEngine routes
    // the effective values (manual setpoint OR Read-mode automation)
    // through these atoms each block. The non-live `faderDb` / `mute`
    // stay the persisted user setpoint.
    const float faderDb = paramsRef->liveFaderDb.load (std::memory_order_relaxed);
    const bool  muted   = paramsRef->liveMute.load    (std::memory_order_relaxed);

    const float gain = (muted || faderDb <= ChannelStripParams::kFaderInfThreshDb)
                       ? 0.0f
                       : juce::Decibels::decibelsToGain (faderDb);
    faderGain.setTargetValue (gain);

    // Read livePan (engine routes manual pan or lane value through it),
    // matching the liveFaderDb pattern.
    const float p     = juce::jlimit (-1.0f, 1.0f, paramsRef->livePan.load (std::memory_order_relaxed));
    const float angle = (p + 1.0f) * (juce::MathConstants<float>::halfPi * 0.5f);
    panGainL.setTargetValue (std::cos (angle));
    panGainR.setTargetValue (std::sin (angle));

    for (int i = 0; i < kNumBuses; ++i)
        busGain[(size_t) i].setTargetValue (
            paramsRef->busAssign[(size_t) i].load (std::memory_order_relaxed) ? 1.0f : 0.0f);

    // Aux sends - per-knob linear gain. Reads liveAuxSendDb (engine routes
    // manual or lane through it) - same pattern as liveFaderDb / livePan.
    // -100 dB sentinel (knob fully CCW) hard-mutes; the inner loop
    // short-circuits zero-gain sends.
    for (int i = 0; i < kNumAuxSends; ++i)
    {
        const float db = paramsRef->liveAuxSendDb[(size_t) i].load (std::memory_order_relaxed);
        const float g  = (db <= ChannelStripParams::kAuxSendOffDb)
                            ? 0.0f
                            : juce::Decibels::decibelsToGain (db);
        auxSendGain[(size_t) i].setTargetValue (g);
        auxSendPre[(size_t) i] =
            paramsRef->auxSendPreFader[(size_t) i].load (std::memory_order_relaxed);
    }
}

void ChannelStrip::updateEqParameters() noexcept
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    if (paramsRef == nullptr) return;
    // Value-init so padding bytes are zero - paired with lastEqParams's {}
    // initializer, this lets memcmp tell us reliably whether the params
    // actually changed since last block. Skipping setParameters() when they
    // haven't avoids a full BritishEQProcessor coefficient recompute (8-14
    // biquads) on every silent block on every channel.
    BritishEQProcessor::Parameters p {};
    p.hpfEnabled = paramsRef->hpfEnabled.load (std::memory_order_relaxed);
    p.hpfFreq    = paramsRef->hpfFreq.load    (std::memory_order_relaxed);
    p.lpfEnabled = paramsRef->lpfEnabled.load (std::memory_order_relaxed);
    p.lpfFreq    = paramsRef->lpfFreq.load    (std::memory_order_relaxed);
    p.lfGain     = paramsRef->lfGainDb.load (std::memory_order_relaxed);
    p.lfFreq     = paramsRef->lfFreq.load   (std::memory_order_relaxed);
    p.lfBell     = false;
    p.lmGain     = paramsRef->lmGainDb.load (std::memory_order_relaxed);
    p.lmFreq     = paramsRef->lmFreq.load   (std::memory_order_relaxed);
    p.lmQ        = paramsRef->lmQ.load      (std::memory_order_relaxed);
    p.hmGain     = paramsRef->hmGainDb.load (std::memory_order_relaxed);
    p.hmFreq     = paramsRef->hmFreq.load   (std::memory_order_relaxed);
    p.hmQ        = paramsRef->hmQ.load      (std::memory_order_relaxed);
    p.hfGain     = paramsRef->hfGainDb.load (std::memory_order_relaxed);
    p.hfFreq     = paramsRef->hfFreq.load   (std::memory_order_relaxed);
    p.hfBell     = false;
    p.isBlackMode = paramsRef->eqBlackMode.load (std::memory_order_relaxed);
    p.saturation  = 0.0f;
    p.inputGain   = 0.0f;
    p.outputGain  = 0.0f;
    if (std::memcmp (&p, &lastEqParams, sizeof (p)) != 0)
    {
        eq.setParameters (p);
        lastEqParams = p;
    }
#endif
}

void ChannelStrip::updateCompParameters() noexcept
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    if (paramsRef == nullptr) return;

    // Discrete params (no smoothing): bypass + mode + LIMIT toggle + FET
    // ratio index. Stored directly on the donor atoms.
    storeAtom (compBypassAtom,
               paramsRef->compEnabled.load (std::memory_order_relaxed) ? 0.0f : 1.0f);

    const int modeIdx = juce::jlimit (0, 2, paramsRef->compMode.load (std::memory_order_relaxed));
    storeAtom (compModeAtom, (float) modeIdx);

    // Per-mode sidechain HPF preset. VCA gets 60 Hz to match the SSL G /
    // dbx convention (keeps low-end transients from pumping the mids);
    // Opto and FET stay at 0 Hz so their original feedback-detector
    // character is preserved.
    const float scHpHz = (modeIdx == 2) ? 60.0f : 0.0f;
    storeAtom (compScHpAtom, scHpHz);

    storeAtom (compOptoLimitAtom,
               paramsRef->compOptoLimit.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    storeAtom (compFetRatioAtom,
               (float) paramsRef->compFetRatio.load (std::memory_order_relaxed));
    storeAtom (compVcaOverEasyAtom,
               paramsRef->compVcaOverEasy.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
    // 0 = Adaptive (donor default), 1 = Classic (dbx 160 fixed 10 ms).
    storeAtom (compVcaDetectorModeAtom,
               paramsRef->compVcaDetectorClassic.load (std::memory_order_relaxed) ? 1.0f : 0.0f);

    // Continuous params: set smoother targets. The per-chunk
    // publishSmoothedCompParams() advances the smoothers and writes the
    // current value into the donor atom right before each comp.processBlock
    // call, so a knob drag fans out as a 20 ms ramp instead of a step.
    smoothedOptoPeakRed.setTargetValue (paramsRef->compOptoPeakRed.load (std::memory_order_relaxed));
    smoothedOptoGain   .setTargetValue (paramsRef->compOptoGain   .load (std::memory_order_relaxed));
    smoothedFetInput   .setTargetValue (paramsRef->compFetInput   .load (std::memory_order_relaxed));
    smoothedFetOutput  .setTargetValue (paramsRef->compFetOutput  .load (std::memory_order_relaxed));
    smoothedFetAttack  .setTargetValue (paramsRef->compFetAttack  .load (std::memory_order_relaxed));
    smoothedFetRelease .setTargetValue (paramsRef->compFetRelease .load (std::memory_order_relaxed));
    smoothedFetThreshold.setTargetValue (paramsRef->compFetThresholdDb.load (std::memory_order_relaxed));
    smoothedVcaThresh  .setTargetValue (paramsRef->compVcaThreshDb.load (std::memory_order_relaxed));
    smoothedVcaRatio   .setTargetValue (paramsRef->compVcaRatio   .load (std::memory_order_relaxed));
    smoothedVcaAttack  .setTargetValue (paramsRef->compVcaAttack  .load (std::memory_order_relaxed));
    smoothedVcaRelease .setTargetValue (paramsRef->compVcaRelease .load (std::memory_order_relaxed));
    smoothedVcaOutput  .setTargetValue (paramsRef->compVcaOutput  .load (std::memory_order_relaxed));
#endif
}

void ChannelStrip::publishSmoothedCompParams (int numSamples) noexcept
{
#if DUSKSTUDIO_HAS_DUSK_DSP
    if (numSamples <= 0) return;
    // Advance each smoother by N samples then write the end-of-chunk
    // value to the donor atom. Called once per inner chunk so chunks
    // shorter than the smoother's 20 ms ramp see a STEPPED-but-fine-
    // grained param trajectory (one step per 64 samples) rather than
    // one step per audio block. Donor reads each atom only at the
    // start of its processBlock call - true per-sample interpolation
    // would require calling processBlock per-sample.
    auto step = [numSamples] (juce::SmoothedValue<float>& sv,
                                 std::atomic<float>* atom)
    {
        sv.skip (numSamples);
        if (atom != nullptr) atom->store (sv.getCurrentValue(),
                                            std::memory_order_relaxed);
    };
    step (smoothedOptoPeakRed, compOptoPeakRedAtom);
    step (smoothedOptoGain,    compOptoGainAtom);
    step (smoothedFetInput,    compFetInputAtom);
    step (smoothedFetOutput,   compFetOutputAtom);
    step (smoothedFetAttack,   compFetAttackAtom);
    step (smoothedFetRelease,  compFetReleaseAtom);
    step (smoothedFetThreshold,compFetThresholdAtom);
    step (smoothedVcaThresh,   compVcaThreshAtom);
    step (smoothedVcaRatio,    compVcaRatioAtom);
    step (smoothedVcaAttack,   compVcaAttackAtom);
    step (smoothedVcaRelease,  compVcaReleaseAtom);
    step (smoothedVcaOutput,   compVcaOutputAtom);
#else
    (void) numSamples;
#endif
}

void ChannelStrip::bindHardwareInsert (const HardwareInsertParams& params) noexcept
{
    hardwareSlot.bind (params);
}

void ChannelStrip::processAndAccumulate (const float* inL,
                                         const float* inR,
                                         juce::MidiBuffer& trackMidi,
                                         bool  isMidi,
                                         float* masterL, float* masterR,
                                         const std::array<float*, kNumBuses>& busL,
                                         const std::array<float*, kNumBuses>& busR,
                                         const std::array<float*, kNumAuxSends>& auxLaneL,
                                         const std::array<float*, kNumAuxSends>& auxLaneR,
                                         int numSamples,
                                         bool passByGate,
                                         const float* const* deviceInputs,
                                         int   numDeviceInputs,
                                         float* const*       deviceOutputs,
                                         int   numDeviceOutputs) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    // Insert-mode crossfade gate. Run once at the top of every block.
    //   - If the user (or session-load) flipped insertMode, ramp the
    //     gate's gain DOWN. Once it reaches zero, swap activeInsertMode
    //     and reset the freshly-activated slot's tail state so it starts
    //     clean. Then ramp UP to 1 on subsequent blocks.
    //   - Steady state: the gate's target follows the active mode -
    //     1.0 when an insert is active (plugin or hardware), 0.0 when
    //     empty (no DSP runs at all).
    const int req = insertMode.load (std::memory_order_acquire);
    if (req != activeInsertMode)
    {
        if (activeInsertGain.getCurrentValue() > 1.0e-4f)
        {
            activeInsertGain.setTargetValue (0.0f);
        }
        else
        {
            activeInsertMode = req;
            if (activeInsertMode == kInsertHardware)
                hardwareSlot.resetTailsAndDelayLine();
            activeInsertGain.setTargetValue (activeInsertMode == kInsertEmpty
                                              ? 0.0f : 1.0f);
        }
    }
    else
    {
        activeInsertGain.setTargetValue (activeInsertMode == kInsertEmpty
                                          ? 0.0f : 1.0f);
    }

    lastProcessedPtr = nullptr;
    lastProcessedR   = nullptr;
    lastProcessedSamples = 0;

    if (numSamples == 0) return;

    // MIDI tracks always run a stereo audio path: the instrument plugin fills
    // L+R from MIDI events and the rest of the strip processes that as a
    // stereo signal. Mono / Stereo audio tracks behave as before.
    const bool stereo = (inR != nullptr) || isMidi;

    // No params, or an audio track with no input → tick the smoothers down
    // (so M/S transitions sound smooth) and bail. MIDI tracks are exempt
    // from the inL == nullptr bail because their audio source comes from
    // the plugin, not an input buffer.
    if (paramsRef == nullptr || (inL == nullptr && ! isMidi))
    {
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain)     s.setTargetValue (0.0f);
        for (auto& s : auxSendGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain)     s.getNextValue();
            for (auto& s : auxSendGain) s.getNextValue();
        }
        return;
    }

    if ((int) tempMono.size() < numSamples)
        return;  // can't allocate on the audio thread; bail safely (silence)
    if (stereo && tempStereoBuffer.getNumSamples() < numSamples)
        return;

    // Skip the heavy DSP when the strip isn't passing to master and the
    // recorder doesn't need a processed buffer either. With 16 channels each
    // hosting a UniversalCompressor (a full juce::AudioProcessor), running
    // the chain on every silent track was an xrun-class CPU spike.
    if (! passByGate && ! needsProcessedMono)
    {
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain)     s.setTargetValue (0.0f);
        for (auto& s : auxSendGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain)     s.getNextValue();
            for (auto& s : auxSendGain) s.getNextValue();
        }
        return;
    }

    // Per-track silent-skip — when the audio source is dead silent AND
    // there's no insert plugin that could produce tail / generated
    // audio, the whole HPF / 4-band EQ / comp / sends chain is a no-op
    // worth dodging. 24 strips × full DSP per block is an xrun-class
    // cost on ALSA's tight scheduling window; PipeWire's slack hides
    // it. Mirrors the aux-lane silent skip in
    // AudioEngine.cpp:audioDeviceIOCallbackWithContext (the canonical
    // freeze-smoothers / reset-meter pattern). Constraints:
    //   - MIDI tracks excluded (instrument plugin generates audio
    //     from MIDI even with no audio input).
    //   - Tracks with an insert plugin excluded (plugin tail / latency
    //     compensation would suffer from skipped blocks).
    //   - `needsProcessedMono` (recorder-print path) already handled
    //     above; tracks armed-with-print stay on the full pass.
    if (! isMidi && activeInsertMode == kInsertEmpty)
    {
        const auto peakAbs = [] (const float* buf, int n) -> float
        {
            if (buf == nullptr || n <= 0) return 0.0f;
            const auto rng = juce::FloatVectorOperations::findMinAndMax (buf, n);
            return juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
        };
        const float peakL = peakAbs (inL, numSamples);
        const float peakR = (stereo && inR != nullptr) ? peakAbs (inR, numSamples) : 0.0f;
        if (peakL <= 1e-6f && peakR <= 1e-6f)
        {
           #if DUSKSTUDIO_HAS_DUSK_DSP
            // No comp ran this block, so the GR meter would otherwise pin
            // at the last reduction value. Force it to 0 so a silent track
            // reads no gain reduction — same intent as the bypass path that
            // zeroes the GR atom after the comp pass below.
            currentGrDb.store (0.0f, std::memory_order_relaxed);
           #endif
            // Silent input -> silent output: the output meter reads -inf too.
            currentOutLDb.store (-100.0f, std::memory_order_relaxed);
            currentOutRDb.store (-100.0f, std::memory_order_relaxed);
            // Smoothers stay FROZEN on skip (no setTargetValue,
            // no getNextValue) — when audio returns, they pick up
            // at the configured value with no click. The track's
            // input meter is written by AudioEngine before this
            // call and already reflects -inf for silent input, so
            // no meter reset is required here.
            return;
        }
    }

    updateGainTargets();
    updateEqParameters();
    updateCompParameters();

    // Hoist eqEnabled once per block (constant for the whole pass) so the
    // four call-sites below all see the same value. On a false->true
    // transition reset the EQ so stale filter history from before the
    // bypass doesn't ring out as a transient burst when re-enabled.
    const bool eqEnabled = paramsRef->eqEnabled.load (std::memory_order_relaxed);
    if (eqEnabled && ! prevEqEnabled)
        eq.reset();
    prevEqEnabled = eqEnabled;

    // Source-pointer set used by the accumulation loop below. For mono,
    // both point at tempMono (so srcL[i] and srcR[i] are the same sample —
    // the existing equal-power pan distributes the mono signal to L/R).
    // For stereo, they point at the two channels of tempStereoBuffer; the
    // pan gains then act as a per-channel balance.
    const float* srcL = nullptr;
    const float* srcR = nullptr;

    if (! stereo)
    {
        std::memcpy (tempMono.data(), inL, sizeof (float) * (size_t) numSamples);

        // Phase invert (Ø) - flip polarity before EQ/comp/fader so the rest of
        // the chain sees the corrected-polarity signal. SIMD'd negate via JUCE
        // saves a per-sample scalar multiply when phase invert is engaged.
        if (paramsRef->phaseInvert.load (std::memory_order_relaxed))
            juce::FloatVectorOperations::negate (tempMono.data(), tempMono.data(), numSamples);

        // Per-channel insert (post-phase-invert, pre-EQ). insertMode picks
        // plugin or hardware; activeInsertGain ramps the post-insert signal
        // against the pre-insert copy so mode flips are click-free and an
        // empty slot just bypasses both.
        //
        // The early-return at the top of this function bails on numSamples
        // larger than tempMono / insertScratchL (sized identically at
        // prepare()), so the assertion below is the invariant guard for
        // debug builds; release relies on the bail.
        jassert (numSamples <= (int) insertScratchL.size());

        std::memcpy (insertScratchL.data(), tempMono.data(),
                      sizeof (float) * (size_t) numSamples);
        if (activeInsertMode == kInsertPlugin)
        {
            pluginMidiScratch.clear();
            pluginSlot.processMonoBlock (tempMono.data(), numSamples, pluginMidiScratch);
        }
        else if (activeInsertMode == kInsertHardware)
        {
            // Mono path: duplicate to L+R, run the stereo hardware insert,
            // collapse back to mono by averaging. Hardware return is
            // inherently stereo (it routes to a physical input pair) so
            // the average is the most faithful mono interpretation.
            std::memcpy (insertScratchR.data(), tempMono.data(),
                          sizeof (float) * (size_t) numSamples);
            hardwareSlot.processStereoBlock (tempMono.data(), insertScratchR.data(),
                                              numSamples,
                                              deviceInputs, numDeviceInputs,
                                              deviceOutputs, numDeviceOutputs);
            for (int i = 0; i < numSamples; ++i)
                tempMono[(size_t) i] = 0.5f
                    * (tempMono[(size_t) i] + insertScratchR[(size_t) i]);
        }
        // Crossfade pre-insert vs post-insert by the gate.
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = activeInsertGain.getNextValue();
            tempMono[(size_t) i] = (1.0f - g) * insertScratchL[(size_t) i]
                                   +        g  * tempMono[(size_t) i];
        }

#if DUSKSTUDIO_HAS_DUSK_DSP
        if (oversamplerStages > 0 && oversamplerMono != nullptr)
        {
            // Oversampled chain: upsample tempMono, run EQ + Comp on the
            // upsampled view, then downsample back into tempMono. Donor
            // saturation aliasing is suppressed by the half-band filters
            // inside juce::dsp::Oversampling.
            const float* readPtrs[1]  = { tempMono.data() };
            float*       writePtrs[1] = { tempMono.data() };
            juce::dsp::AudioBlock<const float> nativeIn  (readPtrs,  1, (size_t) numSamples);
            juce::dsp::AudioBlock<float>       nativeOut (writePtrs, 1, (size_t) numSamples);

            auto upBlock = oversamplerMono->processSamplesUp (nativeIn);
            const int upN = (int) upBlock.getNumSamples();
            float* upPtrs[1] = { upBlock.getChannelPointer (0) };
            juce::AudioBuffer<float> upBuf (upPtrs, 1, upN);
            if (eqEnabled)
                eq.process (upBuf);

            // 64-sample sub-chunks so the SmoothedValue ramp updates the
            // donor's atoms many times per audio block instead of once -
            // the donor reads each atom only at the start of its
            // processBlock call, so a single big chunk = a single
            // stepped value per block. Actual count per block depends
            // on host buffer size + OS factor (range 4-64 sub-chunks
            // for typical 256-512 sample buffers at 1-4× OS). 64
            // samples = 1.3 ms at 48 kHz native / 0.33 ms at 4× OS,
            // well below audible zipper threshold.
            constexpr int compBufSize = 64;
            for (int offset = 0; offset < upN; offset += compBufSize)
            {
                const int n = juce::jmin (compBufSize, upN - offset);
                float* compView[1] = { upPtrs[0] + offset };
                juce::AudioBuffer<float> compBuf (compView, 1, n);
                // Smooth-publish continuous comp params to the donor atoms
                // for this chunk (advances smoothers by n samples, writes
                // current value). 20 ms ramp turns knob jumps into clean
                // ramps instead of zipper-noise steps.
                publishSmoothedCompParams (n);
                compMidiScratch.clear();
                // Always call processBlock — even when bypassed — so the
                // donor's lookahead ring buffers, bypass-fade crossfader,
                // and detector state stay warm. compBypassAtom flips the
                // donor's bypass param at the top of updateCompParameters,
                // and its internal bypass branch returns dry signal.
                compressor.processBlock (compBuf, compMidiScratch);
            }

            oversamplerMono->processSamplesDown (nativeOut);
        }
        else
        {
            // Native-rate path (factor == 1).
            float* monoChannel[1] = { tempMono.data() };
            juce::AudioBuffer<float> monoBuf (monoChannel, 1, numSamples);
            if (eqEnabled)
                eq.process (monoBuf);

            constexpr int bufSize = 64;   // same sub-chunk rationale as above
            for (int offset = 0; offset < numSamples; offset += bufSize)
            {
                const int n = juce::jmin (bufSize, numSamples - offset);
                float* monoView[1] = { tempMono.data() + offset };
                juce::AudioBuffer<float> compBuf (monoView, 1, n);
                publishSmoothedCompParams (n);
                compMidiScratch.clear();
                compressor.processBlock (compBuf, compMidiScratch);
            }
        }
        // When bypassed, force the GR atom to 0 so the meter doesn't hold
        // the last-computed reduction (the donor's getGainReduction()
        // returns the cached value from its detector even while bypass=1).
        const bool compOn = paramsRef->compEnabled.load (std::memory_order_relaxed);
        currentGrDb.store (compOn ? compressor.getGainReduction() : 0.0f,
                            std::memory_order_relaxed);
#endif

        srcL = tempMono.data();
        srcR = tempMono.data();
        lastProcessedPtr = tempMono.data();
        lastProcessedSamples = numSamples;
    }
    else
    {
        // Stereo / MIDI path. Stereo tracks copy inL/inR into the pre-
        // allocated scratch and run the (insert) plugin in place. MIDI
        // tracks zero the scratch and let the (instrument) plugin fill it
        // from the filtered per-track MIDI events. Both then proceed
        // through the same EQ + Comp + accumulate pipeline.
        auto* L = tempStereoBuffer.getWritePointer (0);
        auto* R = tempStereoBuffer.getWritePointer (1);

        if (isMidi)
        {
            juce::FloatVectorOperations::clear (L, numSamples);
            juce::FloatVectorOperations::clear (R, numSamples);
            // Instrument plugins read MIDI and write audio. Phase invert is
            // a no-op against generated content, so we don't apply it here.
            // The instrument plugin IS the audio source - the crossfade
            // gate doesn't apply (no dry to blend with, no "empty" valid
            // for a MIDI track that has an instrument loaded). We still
            // tick the smoother to keep its state in sync in case the
            // track later flips to audio mode.
            pluginSlot.processStereoBlock (L, R, numSamples, trackMidi);
            for (int i = 0; i < numSamples; ++i)
                activeInsertGain.getNextValue();
        }
        else
        {
            std::memcpy (L, inL, sizeof (float) * (size_t) numSamples);
            std::memcpy (R, inR, sizeof (float) * (size_t) numSamples);

            if (paramsRef->phaseInvert.load (std::memory_order_relaxed))
            {
                juce::FloatVectorOperations::negate (L, L, numSamples);
                juce::FloatVectorOperations::negate (R, R, numSamples);
            }

            // Invariant guard: tempStereoBuffer + insertScratchL/R sized
            // identically at prepare(); the early-return at the top of this
            // function bails on oversized blocks before this point.
            jassert (numSamples <= (int) insertScratchL.size());

            // Stash pre-insert for the crossfade gate.
            std::memcpy (insertScratchL.data(), L, sizeof (float) * (size_t) numSamples);
            std::memcpy (insertScratchR.data(), R, sizeof (float) * (size_t) numSamples);

            if (activeInsertMode == kInsertPlugin)
            {
                pluginMidiScratch.clear();
                pluginSlot.processStereoBlock (L, R, numSamples, pluginMidiScratch);
            }
            else if (activeInsertMode == kInsertHardware)
            {
                hardwareSlot.processStereoBlock (L, R, numSamples,
                                                  deviceInputs, numDeviceInputs,
                                                  deviceOutputs, numDeviceOutputs);
            }
            // Crossfade pre vs post by the gate (empty mode collapses to
            // pre, plugin/hardware ramp from pre to their processed output).
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = activeInsertGain.getNextValue();
                L[i] = (1.0f - g) * insertScratchL[(size_t) i] + g * L[i];
                R[i] = (1.0f - g) * insertScratchR[(size_t) i] + g * R[i];
            }
        }

#if DUSKSTUDIO_HAS_DUSK_DSP
        if (oversamplerStages > 0 && oversamplerStereo != nullptr)
        {
            const float* readPtrs[2]  = { L, R };
            float*       writePtrs[2] = { L, R };
            juce::dsp::AudioBlock<const float> nativeIn  (readPtrs,  2, (size_t) numSamples);
            juce::dsp::AudioBlock<float>       nativeOut (writePtrs, 2, (size_t) numSamples);

            auto upBlock = oversamplerStereo->processSamplesUp (nativeIn);
            const int upN = (int) upBlock.getNumSamples();
            float* upPtrs[2] = { upBlock.getChannelPointer (0),
                                  upBlock.getChannelPointer (1) };
            juce::AudioBuffer<float> upBuf (upPtrs, 2, upN);
            if (eqEnabled)
                eq.process (upBuf);

            // 64-sample sub-chunks so the SmoothedValue ramp updates the
            // donor's atoms many times per audio block instead of once -
            // the donor reads each atom only at the start of its
            // processBlock call, so a single big chunk = a single
            // stepped value per block. Actual count per block depends
            // on host buffer size + OS factor (range 4-64 sub-chunks
            // for typical 256-512 sample buffers at 1-4× OS). 64
            // samples = 1.3 ms at 48 kHz native / 0.33 ms at 4× OS,
            // well below audible zipper threshold.
            constexpr int compBufSize = 64;
            for (int offset = 0; offset < upN; offset += compBufSize)
            {
                const int n = juce::jmin (compBufSize, upN - offset);
                float* compView[2] = { upPtrs[0] + offset, upPtrs[1] + offset };
                juce::AudioBuffer<float> compBuf (compView, 2, n);
                publishSmoothedCompParams (n);
                compMidiScratch.clear();
                compressor.processBlock (compBuf, compMidiScratch);
            }

            oversamplerStereo->processSamplesDown (nativeOut);
        }
        else
        {
            float* stereoChannels[2] = { L, R };
            juce::AudioBuffer<float> stereoBuf (stereoChannels, 2, numSamples);
            if (eqEnabled)
                eq.process (stereoBuf);

            constexpr int bufSize = 64;   // same sub-chunk rationale as above
            for (int offset = 0; offset < numSamples; offset += bufSize)
            {
                const int n = juce::jmin (bufSize, numSamples - offset);
                float* stView[2] = { L + offset, R + offset };
                juce::AudioBuffer<float> compBuf (stView, 2, n);
                publishSmoothedCompParams (n);
                compMidiScratch.clear();
                compressor.processBlock (compBuf, compMidiScratch);
            }
        }
        // See mono-path comment above — zero the GR atom when bypassed so
        // the meter doesn't pin at the last reduction value.
        const bool compOnStereo = paramsRef->compEnabled.load (std::memory_order_relaxed);
        currentGrDb.store (compOnStereo ? compressor.getGainReduction() : 0.0f,
                            std::memory_order_relaxed);
#endif

        srcL = L;
        srcR = R;
        // Recorder reads getLastProcessedMono() / getLastProcessedR() —
        // both pointers are valid for stereo tracks so the recorder can
        // capture both channels when printEffects is engaged.
        lastProcessedPtr = L;
        lastProcessedR   = R;
        lastProcessedSamples = numSamples;
    }

    if (! passByGate)
    {
        // Strip is muted/soloed-out/IN-off - keep the smoothers ticking but
        // don't accumulate to master/buses. The DSP STILL ran above, so a
        // recording-armed track with `printEffects` can still capture the
        // post-effects signal even when the engineer has IN off (direct
        // hardware monitoring scenario).
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain)     s.setTargetValue (0.0f);
        for (auto& s : auxSendGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain)     s.getNextValue();
            for (auto& s : auxSendGain) s.getNextValue();
        }
        return;
    }

    // Hoist the bus-active set out of the inner sample loop. The common
    // case (and the only case for a bare-bones session) is "no bus
    // assigned" - tracks pass straight to master. We branch once per
    // block on whether any bus is active OR smoothing; if not we run a
    // master-only fast path that skips the per-sample 4-bus scan and the
    // (1 - maxBusG) crossfade math entirely. Bus smoothers must still
    // tick to follow target so re-engaging a bus is click-free.
    bool anyBusActive = false;
    bool anyBusSmoothing = false;
    for (int a = 0; a < kNumBuses; ++a)
    {
        if (busGain[(size_t) a].getCurrentValue() > 0.0f) anyBusActive = true;
        if (busGain[(size_t) a].isSmoothing())            anyBusSmoothing = true;
    }
    bool anyAuxActive = false;
    bool anyAuxSmoothing = false;
    for (int a = 0; a < kNumAuxSends; ++a)
    {
        if (auxSendGain[(size_t) a].getCurrentValue() > 0.0f) anyAuxActive = true;
        if (auxSendGain[(size_t) a].isSmoothing())            anyAuxSmoothing = true;
    }

    // Post-fader / post-pan output peak, captured in whichever accumulate path
    // runs below, then published as the strip's output meter (the UI shows it
    // during playback). It's the level BEFORE the master/bus routing split, so
    // it reflects the track's contribution regardless of where it's routed.
    float outPeakL = 0.0f, outPeakR = 0.0f;
    const auto publishOutMeter = [this] (float pL, float pR)
    {
        currentOutLDb.store (pL > 1e-5f ? juce::Decibels::gainToDecibels (pL, -100.0f) : -100.0f,
                             std::memory_order_relaxed);
        currentOutRDb.store (pR > 1e-5f ? juce::Decibels::gainToDecibels (pR, -100.0f) : -100.0f,
                             std::memory_order_relaxed);
    };

    if (! anyBusActive && ! anyBusSmoothing && ! anyAuxActive && ! anyAuxSmoothing)
    {
        // Master-only fast path - no bus, no aux. The common steady state
        // for a track that's just summing direct to master. srcL == srcR
        // for mono (existing equal-power pan distributes mono → L/R); for
        // stereo, srcL / srcR are the two channels of tempStereoBuffer
        // and pan acts as a per-channel balance.
        for (int i = 0; i < numSamples; ++i)
        {
            const float fg = faderGain.getNextValue();
            const float gL = panGainL.getNextValue() * fg;
            const float gR = panGainR.getNextValue() * fg;
            const float oL = srcL[i] * gL;
            const float oR = srcR[i] * gR;
            masterL[i] += oL;
            masterR[i] += oR;
            outPeakL = juce::jmax (outPeakL, std::abs (oL));
            outPeakR = juce::jmax (outPeakR, std::abs (oR));
        }
        publishOutMeter (outPeakL, outPeakR);
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float fg = faderGain.getNextValue();
        // Capture pan-only gains so we can build pre-fader stereo for the
        // aux sends without ticking the smoother twice. wetL/wetR include
        // the fader; wetLPre/wetRPre are post-pan, pre-fader (used by aux
        // sends with auxSendPre[i]==true).
        const float pL = panGainL.getNextValue();
        const float pR = panGainR.getNextValue();
        const float gL = pL * fg;
        const float gR = pR * fg;
        const float sL = srcL[i];
        const float sR = srcR[i];
        const float wetLPre = sL * pL;
        const float wetRPre = sR * pR;
        const float wetL = sL * gL;
        const float wetR = sR * gR;
        outPeakL = juce::jmax (outPeakL, std::abs (wetL));
        outPeakR = juce::jmax (outPeakR, std::abs (wetR));

        // Bus routing is EXCLUSIVE with master routing: a track assigned to
        // any bus must not also hit the master direct, otherwise the signal
        // would arrive at master twice (once direct, once via the bus's own
        // sum-into-master in AudioEngine), producing a +3 dB doubling. We
        // gate the master accumulation by (1 - maxBusGain) so a fully-on
        // bus assignment fully removes the direct send, and a mid-ramp
        // toggle produces a smooth crossfade between routes.
        float perBusG[kNumBuses];
        float maxBusG = 0.0f;
        for (int a = 0; a < kNumBuses; ++a)
        {
            perBusG[a] = busGain[(size_t) a].getNextValue();
            if (perBusG[a] > maxBusG) maxBusG = perBusG[a];
        }
        const float toMaster = juce::jmax (0.0f, 1.0f - maxBusG);

        masterL[i] += wetL * toMaster;
        masterR[i] += wetR * toMaster;

        for (int a = 0; a < kNumBuses; ++a)
        {
            if (perBusG[a] > 0.0f)
            {
                busL[(size_t) a][i] += wetL * perBusG[a];
                busR[(size_t) a][i] += wetR * perBusG[a];
            }
        }

        // Aux sends. Each send's gain ticks every sample so a knob ramp is
        // smooth; pre/post-fader is captured per-block (auxSendPre[]). A
        // send is independent of bus-vs-master routing - turning up an aux
        // send doesn't reduce the master / bus signal, since auxes are FX
        // returns that mix back into master via their own AuxLaneStrip pass.
        for (int a = 0; a < kNumAuxSends; ++a)
        {
            const float sg = auxSendGain[(size_t) a].getNextValue();
            if (sg <= 0.0f) continue;
            if (auxSendPre[(size_t) a])
            {
                auxLaneL[(size_t) a][i] += wetLPre * sg;
                auxLaneR[(size_t) a][i] += wetRPre * sg;
            }
            else
            {
                auxLaneL[(size_t) a][i] += wetL * sg;
                auxLaneR[(size_t) a][i] += wetR * sg;
            }
        }
    }
    publishOutMeter (outPeakL, outPeakR);
}
} // namespace duskstudio
