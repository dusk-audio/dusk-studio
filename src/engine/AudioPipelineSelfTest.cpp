#include "AudioPipelineSelfTest.h"
#if defined(__linux__)
 #include "alsa/AlsaAudioIODevice.h"
#endif
#include <cmath>

namespace duskstudio
{
namespace
{
// -6 dBFS sine reference: amplitude = 10^(-6/20) ≈ 0.5012
constexpr float kInputAmpMinusSixDb = 0.5012f;
constexpr float kToneHz             = 1000.0f;

// Tolerance bands for "matches expected" - looser for peak (sine peak isn't
// always sampled cleanly at 1 kHz / 48 kHz), tighter for RMS.
constexpr float kPeakTol = 0.025f;   // ~ ±0.2 dB
constexpr float kRmsTol  = 0.010f;

float ampToDb (float a, float ref = 1.0f)
{
    return 20.0f * std::log10 (juce::jmax (1.0e-9f, a) / juce::jmax (1.0e-9f, ref));
}

juce::String fmtPassFail (bool pass) { return pass ? juce::String ("[PASS]") : juce::String ("[FAIL]"); }
} // namespace

AudioPipelineSelfTest::AudioPipelineSelfTest (AudioEngine& e,
                                              juce::AudioDeviceManager& dm,
                                              Session& s) noexcept
    : engine (e), deviceManager (dm), session (s)
{}

AudioPipelineSelfTest::SavedState AudioPipelineSelfTest::saveState() const
{
    SavedState s;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& ts = session.track ((int) t).strip;
        const auto& tt = session.track ((int) t);
        s.faderDb[(size_t) t]      = ts.faderDb.load (std::memory_order_relaxed);
        s.pan[(size_t) t]          = ts.pan.load (std::memory_order_relaxed);
        s.mute[(size_t) t]         = ts.mute.load (std::memory_order_relaxed);
        s.solo[(size_t) t]         = ts.solo.load (std::memory_order_relaxed);
        s.recordArmed[(size_t) t]  = tt.recordArmed.load (std::memory_order_relaxed);
        s.inputMonitor[(size_t) t] = tt.inputMonitor.load (std::memory_order_relaxed);
        s.compEnabled[(size_t) t]  = ts.compEnabled.load (std::memory_order_relaxed);
        s.phaseInvert[(size_t) t]  = ts.phaseInvert.load (std::memory_order_relaxed);
        s.hpfEnabled[(size_t) t]   = ts.hpfEnabled.load (std::memory_order_relaxed);
        s.lfGainDb[(size_t) t]     = ts.lfGainDb.load (std::memory_order_relaxed);
        s.lmGainDb[(size_t) t]     = ts.lmGainDb.load (std::memory_order_relaxed);
        s.hmGainDb[(size_t) t]     = ts.hmGainDb.load (std::memory_order_relaxed);
        s.hfGainDb[(size_t) t]     = ts.hfGainDb.load (std::memory_order_relaxed);
    }
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (int b = 0; b < ChannelStripParams::kNumBuses; ++b)
            s.trackBusAssign[(size_t) t][(size_t) b] =
                session.track (t).strip.busAssign[(size_t) b].load (std::memory_order_relaxed);
    for (int b = 0; b < Session::kNumBuses; ++b)
        s.busSolo[(size_t) b] = session.bus (b).strip.solo.load (std::memory_order_relaxed);
    s.masterFaderDb     = session.master().faderDb.load (std::memory_order_relaxed);
    s.masterTapeEnabled = session.master().tapeEnabled.load (std::memory_order_relaxed);
    s.masterTapeHQ      = session.master().tapeHQ.load (std::memory_order_relaxed);
    s.masterCompEnabled = session.master().compEnabled.load (std::memory_order_relaxed);
    return s;
}

void AudioPipelineSelfTest::restoreState (const SavedState& s)
{
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& ts = session.track (t).strip;
        auto& tt = session.track (t);
        ts.faderDb.store      (s.faderDb[(size_t) t],      std::memory_order_relaxed);
        ts.pan.store          (s.pan[(size_t) t],          std::memory_order_relaxed);
        ts.mute.store         (s.mute[(size_t) t],         std::memory_order_relaxed);
        ts.solo.store         (s.solo[(size_t) t],         std::memory_order_relaxed);
        tt.recordArmed.store  (s.recordArmed[(size_t) t],  std::memory_order_relaxed);
        tt.inputMonitor.store (s.inputMonitor[(size_t) t], std::memory_order_relaxed);
        ts.compEnabled.store  (s.compEnabled[(size_t) t],  std::memory_order_relaxed);
        ts.phaseInvert.store  (s.phaseInvert[(size_t) t],  std::memory_order_relaxed);
        ts.hpfEnabled.store   (s.hpfEnabled[(size_t) t],   std::memory_order_relaxed);
        ts.lfGainDb.store     (s.lfGainDb[(size_t) t],     std::memory_order_relaxed);
        ts.lmGainDb.store     (s.lmGainDb[(size_t) t],     std::memory_order_relaxed);
        ts.hmGainDb.store     (s.hmGainDb[(size_t) t],     std::memory_order_relaxed);
        ts.hfGainDb.store     (s.hfGainDb[(size_t) t],     std::memory_order_relaxed);
    }
    for (int t = 0; t < Session::kNumTracks; ++t)
        for (int b = 0; b < ChannelStripParams::kNumBuses; ++b)
            session.track (t).strip.busAssign[(size_t) b].store (
                s.trackBusAssign[(size_t) t][(size_t) b], std::memory_order_relaxed);
    for (int b = 0; b < Session::kNumBuses; ++b)
        session.bus (b).strip.solo.store (s.busSolo[(size_t) b], std::memory_order_relaxed);
    session.master().faderDb.store     (s.masterFaderDb,     std::memory_order_relaxed);
    session.master().tapeEnabled.store (s.masterTapeEnabled, std::memory_order_relaxed);
    session.master().tapeHQ.store      (s.masterTapeHQ,      std::memory_order_relaxed);
    session.master().compEnabled.store (s.masterCompEnabled, std::memory_order_relaxed);
    // Bulk-write path bypassed the counter-aware setters; resync the RT
    // counters so anyTrackSoloed/Armed reads are correct in the test pass.
    session.recomputeRtCounters();
}

int AudioPipelineSelfTest::prepareCleanState()
{
    // Track 0: live IN, unity, pan center, all DSP neutral / bypassed.
    auto& t0  = session.track (0);
    auto& t0s = t0.strip;
    t0s.faderDb.store      (0.0f, std::memory_order_relaxed);
    t0s.pan.store          (0.0f, std::memory_order_relaxed);
    t0s.mute.store         (false, std::memory_order_relaxed);
    t0s.solo.store         (false, std::memory_order_relaxed);
    t0.recordArmed.store   (false, std::memory_order_relaxed);
    t0.inputMonitor.store  (true,  std::memory_order_relaxed);
    t0s.compEnabled.store  (false, std::memory_order_relaxed);  // bypassed
    t0s.phaseInvert.store  (false, std::memory_order_relaxed);
    t0s.hpfEnabled.store   (false, std::memory_order_relaxed);
    t0s.lfGainDb.store     (0.0f,  std::memory_order_relaxed);
    t0s.lmGainDb.store     (0.0f,  std::memory_order_relaxed);
    t0s.hmGainDb.store     (0.0f,  std::memory_order_relaxed);
    t0s.hfGainDb.store     (0.0f,  std::memory_order_relaxed);
    // No bus assignment: track 0 routes DIRECT to master. Every track-0 test
    // (pass-through unity, mute, bus-solo) assumes this, so enforce it here
    // regardless of the live session's routing (which save/restoreState
    // round-trips for all tracks).
    for (int b = 0; b < ChannelStripParams::kNumBuses; ++b)
        t0s.busAssign[(size_t) b].store (false, std::memory_order_relaxed);

    // Clear bus solos too: a live session (or a test that set one) leaving a
    // bus soloed would mute track 0's direct-to-master path via the SIP gate
    // and spuriously fail every audible-track-0 test.
    for (int b = 0; b < Session::kNumBuses; ++b)
        session.bus (b).strip.solo.store (false, std::memory_order_relaxed);

    // Other tracks: hard mute so they can't contribute to the master mix.
    for (int t = 1; t < Session::kNumTracks; ++t)
    {
        auto& ts = session.track (t).strip;
        ts.mute.store        (true,  std::memory_order_relaxed);
        ts.solo.store        (false, std::memory_order_relaxed);
        session.track (t).recordArmed.store  (false, std::memory_order_relaxed);
        session.track (t).inputMonitor.store (false, std::memory_order_relaxed);
    }

    // Master: unity fader, tape + comp off (so we test pure master fader;
    // resetting comp stops testMasterCompNoNoiseFloor's compEnabled=true from
    // leaking into every later test and skewing their peak measurements).
    session.master().faderDb.store     (0.0f,  std::memory_order_relaxed);
    session.master().tapeEnabled.store (false, std::memory_order_relaxed);
    session.master().tapeHQ.store      (false, std::memory_order_relaxed);
    session.master().compEnabled.store (false, std::memory_order_relaxed);

    // Bulk-write path bypassed the counter-aware setters; resync.
    session.recomputeRtCounters();

    return 0;
}

AudioPipelineSelfTest::Measurements
AudioPipelineSelfTest::runSynthetic (double sampleRate, int blockSize,
                                      int numInChannels, int numOutChannels,
                                      float inputAmp, float toneHz,
                                      int numWarmupBlocks, int numMeasureBlocks)
{
    Measurements m;

    engine.prepareForSelfTest (sampleRate, blockSize);

    // Build input buffers - sine on input 0 (where track 0 reads from);
    // silence on the rest. Phase advances across blocks so successive blocks
    // are continuous, not repeating the same window.
    std::vector<std::vector<float>> inputs ((size_t) numInChannels,
                                              std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<const float*> inputPtrs ((size_t) numInChannels, nullptr);
    for (int c = 0; c < numInChannels; ++c)
        inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs ((size_t) numOutChannels,
                                               std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<float*> outputPtrs ((size_t) numOutChannels, nullptr);
    for (int c = 0; c < numOutChannels; ++c)
        outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    juce::AudioIODeviceCallbackContext ctx {};

    const double phaseInc = 2.0 * juce::MathConstants<double>::pi * (double) toneHz / sampleRate;
    double phase = 0.0;
    double rmsAcc = 0.0, rmsAccR = 0.0;
    long long countedSamples = 0;

    const int totalBlocks = numWarmupBlocks + numMeasureBlocks;
    for (int b = 0; b < totalBlocks; ++b)
    {
        // Refill input channel 0 with continuing sine wave.
        for (int s = 0; s < blockSize; ++s)
        {
            inputs[0][(size_t) s] = inputAmp * (float) std::sin (phase);
            phase += phaseInc;
            if (phase >= 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;
        }
        // Other inputs stay at zero (already).

        // Reset outputs each block.
        for (auto& o : outputs)
            std::fill (o.begin(), o.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (
            inputPtrs.data(), numInChannels,
            outputPtrs.data(), numOutChannels,
            blockSize, ctx);

        if (b >= numWarmupBlocks)
        {
            for (int s = 0; s < blockSize; ++s)
            {
                const float l = outputs[0][(size_t) s];
                const float r = (numOutChannels > 1) ? outputs[1][(size_t) s] : 0.0f;
                if (std::abs (l) > m.peakL) m.peakL = std::abs (l);
                if (std::abs (r) > m.peakR) m.peakR = std::abs (r);
                rmsAcc  += (double) l * (double) l;
                rmsAccR += (double) r * (double) r;
                ++countedSamples;
            }
        }
    }

    if (countedSamples > 0)
    {
        m.rmsL = (float) std::sqrt (rmsAcc  / (double) countedSamples);
        m.rmsR = (float) std::sqrt (rmsAccR / (double) countedSamples);
    }
    return m;
}

juce::String AudioPipelineSelfTest::testPassThroughUnity()
{
    prepareCleanState();
    constexpr int bs = 512;
    constexpr double sr = 48000.0;

    auto m = runSynthetic (sr, bs, 16, 2, kInputAmpMinusSixDb, kToneHz, 8, 8);

    // Track 0 unity, pan center: pan applies cos(pi/4) = 0.7071 to both L and R.
    // Master at unity. Expected output peak = inputAmp * 0.7071 = 0.3543.
    // Expected RMS = peak / sqrt(2) = 0.2506.
    const float expPeak = kInputAmpMinusSixDb * 0.7071f;
    const float expRms  = expPeak / 1.4142f;
    const bool peakOK  = std::abs (m.peakL - expPeak) < kPeakTol
                      && std::abs (m.peakR - expPeak) < kPeakTol;
    const bool rmsOK   = std::abs (m.rmsL  - expRms)  < kRmsTol
                      && std::abs (m.rmsR  - expRms)  < kRmsTol;
    const bool stereoOK = std::abs (m.peakL - m.peakR) < 0.01f;

    return juce::String::formatted (
        "%s Pass-Through Unity (track 0 IN, fader 0 dB, pan center, master 0 dB)\n"
        "      Input -6 dBFS sine @1 kHz, %.0f Hz / %d samples, 16in/2out\n"
        "      Expected peak=%.4f (%+.2f dBFS), RMS=%.4f (%+.2f dBFS)\n"
        "      Measured L peak=%.4f (%+.2f dBFS), RMS=%.4f (%+.2f dBFS) %s\n"
        "      Measured R peak=%.4f (%+.2f dBFS), RMS=%.4f (%+.2f dBFS) %s\n"
        "      L vs R peak delta: %.4f %s",
        fmtPassFail (peakOK && rmsOK && stereoOK).toRawUTF8(),
        sr, bs,
        expPeak, ampToDb (expPeak), expRms, ampToDb (expRms),
        m.peakL, ampToDb (m.peakL), m.rmsL, ampToDb (m.rmsL),
        peakOK && rmsOK ? "" : "<-- mismatch",
        m.peakR, ampToDb (m.peakR), m.rmsR, ampToDb (m.rmsR),
        peakOK && rmsOK ? "" : "<-- mismatch",
        std::abs (m.peakL - m.peakR),
        stereoOK ? "" : "<-- L/R imbalance!");
}

juce::String AudioPipelineSelfTest::testMuteSilences()
{
    prepareCleanState();
    session.track (0).strip.mute.store (true, std::memory_order_relaxed);

    auto m = runSynthetic (48000.0, 512, 16, 2, kInputAmpMinusSixDb, kToneHz, 8, 8);

    const bool silentL = m.peakL < 1.0e-4f;
    const bool silentR = m.peakR < 1.0e-4f;

    return juce::String::formatted (
        "%s Mute Silences (track 0 IN + mute on)\n"
        "      Expected peak=0.0 on both channels\n"
        "      Measured L peak=%.6f, R peak=%.6f %s",
        fmtPassFail (silentL && silentR).toRawUTF8(),
        m.peakL, m.peakR,
        silentL && silentR ? "" : "<-- audio leaking past mute!");
}

juce::String AudioPipelineSelfTest::testMasterFaderMinusSix()
{
    prepareCleanState();
    session.master().faderDb.store (-6.0f, std::memory_order_relaxed);

    auto m = runSynthetic (48000.0, 512, 16, 2, kInputAmpMinusSixDb, kToneHz, 12, 8);

    // Pan center -3 dB, master -6 dB; total -9 dB from input.
    // expPeak = 0.5012 * 0.7071 * 0.5012 = 0.1776
    const float expPeak = kInputAmpMinusSixDb * 0.7071f * 0.5012f;
    const bool peakOK = std::abs (m.peakL - expPeak) < kPeakTol;

    return juce::String::formatted (
        "%s Master Fader -6 dB (track 0 unity + pan center, master -6 dB)\n"
        "      Expected peak=%.4f (%+.2f dBFS)\n"
        "      Measured L peak=%.4f (%+.2f dBFS) %s",
        fmtPassFail (peakOK).toRawUTF8(),
        expPeak, ampToDb (expPeak),
        m.peakL, ampToDb (m.peakL),
        peakOK ? "" : "<-- master fader not applying expected gain!");
}

juce::String AudioPipelineSelfTest::testMasterCompNoNoiseFloor()
{
    prepareCleanState();
    // Engage the master bus compressor over SILENT input. The donor comp's
    // "Analog Noise" feature (~-80 dB white noise, default ON for analog
    // modes) is now forced off in MasterBus::bindCompParams. Because the
    // master chain runs the comp every block when engaged, leaving the noise
    // on printed a continuous ~-67 dB peak floor into the output (and every
    // bounce). With it disabled, an engaged comp over silence stays silent.
    session.master().compEnabled.store (true, std::memory_order_relaxed);

    auto m = runSynthetic (48000.0, 512, 16, 2, 0.0f, kToneHz, 12, 8);

    const bool silentL = m.peakL < 1.0e-4f;   // -80 dBFS
    const bool silentR = m.peakR < 1.0e-4f;

    return juce::String::formatted (
        "%s Master Comp No Noise Floor (comp ON, silent input)\n"
        "      Expected peak=0.0 on both channels (donor analog-noise disabled)\n"
        "      Measured L peak=%.6f (%+.2f dBFS), R peak=%.6f %s",
        fmtPassFail (silentL && silentR).toRawUTF8(),
        m.peakL, ampToDb (m.peakL), m.peakR,
        silentL && silentR ? "" : "<-- comp injecting noise into silence!");
}

juce::String AudioPipelineSelfTest::testBusSoloMutesDirect()
{
    prepareCleanState();   // clears track 0's busAssign -> guaranteed direct-to-master
    // Solo bus 0 with NO track routed to it. Track 0 takes the direct-to-master
    // path, so SIP-style bus solo must mute it -> master goes silent (the soloed
    // bus is empty). Before the fix the unassigned track bypassed the bus-solo
    // gate and leaked.
    session.setBusSoloed (0, true);

    auto m = runSynthetic (48000.0, 512, 16, 2, kInputAmpMinusSixDb, kToneHz, 8, 8);

    const bool silentL = m.peakL < 1.0e-4f;
    const bool silentR = m.peakR < 1.0e-4f;

    // Restore — prepareCleanState resets track solos but not bus solos, so
    // clear it here to avoid bleeding into the next test.
    session.setBusSoloed (0, false);

    return juce::String::formatted (
        "%s Bus Solo Mutes Direct (bus 0 soloed + empty, track 0 unassigned)\n"
        "      Expected silence: a direct-to-master track must not bypass bus solo\n"
        "      Measured L peak=%.6f, R peak=%.6f %s",
        fmtPassFail (silentL && silentR).toRawUTF8(),
        m.peakL, m.peakR,
        silentL && silentR ? "" : "<-- unassigned track leaking past bus solo!");
}

juce::String AudioPipelineSelfTest::testChannelRoutingTwoOut()
{
    prepareCleanState();
    auto m = runSynthetic (48000.0, 512, 16, 2, kInputAmpMinusSixDb, kToneHz, 8, 4);

    // With 2 output channels, both should have signal (pan center).
    const bool bothPresent = m.peakL > 0.05f && m.peakR > 0.05f;
    return juce::String::formatted (
        "%s Channel Routing 2-out (16in/2out, pan center)\n"
        "      L=%.4f R=%.4f %s",
        fmtPassFail (bothPresent).toRawUTF8(),
        m.peakL, m.peakR,
        bothPresent ? "" : "<-- one or both output channels missing signal");
}

juce::String AudioPipelineSelfTest::testChannelRoutingFourOut()
{
    prepareCleanState();

    // Same synthetic test but with 4 output channels. Engine should write
    // signal to channels 0+1 (L+R) and zero/leave channels 2+3.
    constexpr int bs = 512;
    engine.prepareForSelfTest (48000.0, bs);

    std::vector<std::vector<float>> inputs (16, std::vector<float> ((size_t) bs, 0.0f));
    std::vector<const float*> inputPtrs (16, nullptr);
    for (int c = 0; c < 16; ++c) inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs (4, std::vector<float> ((size_t) bs, 0.0f));
    std::vector<float*> outputPtrs (4, nullptr);
    for (int c = 0; c < 4; ++c) outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    juce::AudioIODeviceCallbackContext ctx {};
    const double phaseInc = 2.0 * juce::MathConstants<double>::pi * (double) kToneHz / 48000.0;
    double phase = 0.0;

    std::array<float, 4> peaks { 0.0f, 0.0f, 0.0f, 0.0f };
    constexpr int totalBlocks = 16;
    constexpr int warmup = 8;
    for (int b = 0; b < totalBlocks; ++b)
    {
        for (int s = 0; s < bs; ++s)
        {
            inputs[0][(size_t) s] = kInputAmpMinusSixDb * (float) std::sin (phase);
            phase += phaseInc;
            if (phase >= 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;
        }
        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), 16,
                                                  outputPtrs.data(), 4, bs, ctx);

        if (b >= warmup)
        {
            for (int c = 0; c < 4; ++c)
                for (int s = 0; s < bs; ++s)
                {
                    const float a = std::abs (outputs[(size_t) c][(size_t) s]);
                    if (a > peaks[(size_t) c]) peaks[(size_t) c] = a;
                }
        }
    }

    const bool ch01HaveSignal = peaks[0] > 0.05f && peaks[1] > 0.05f;
    const bool ch23AreSilent  = peaks[2] < 1.0e-4f && peaks[3] < 1.0e-4f;

    return juce::String::formatted (
        "%s Channel Routing 4-out (16in/4out, only ch 0+1 should have audio)\n"
        "      ch0=%.4f ch1=%.4f ch2=%.6f ch3=%.6f %s",
        fmtPassFail (ch01HaveSignal && ch23AreSilent).toRawUTF8(),
        peaks[0], peaks[1], peaks[2], peaks[3],
        (! ch01HaveSignal) ? "<-- L/R missing"
            : (! ch23AreSilent) ? "<-- ch2/3 should be silent (engine bleeding into extra channels?)"
            : "");
}

juce::String AudioPipelineSelfTest::testMasterTapeAddsGain()
{
    // Audit: characterize the master tape donor's transfer function across
    // its input-gain (drive) parameter, with auto-compensation both ON and
    // OFF. Pre-tape peak after pan-center is 0.3544 (-9.01 dBFS) given a
    // -6 dBFS sine on the input. Pass = (1) autoComp ON delivers a
    // monotonic decreasing curve with respect to drive (sane behavior:
    // higher drive -> more tape compression -> lower output) and (2) the
    // default config (autoComp ON, drive 0 dB) sits within +/- 3 dB of
    // unity. -3..+3 dB matches real analog tape behavior at 0 VU.
    //
    // Known TapeMachine donor issue: at high drive (+6..+12 dB) autoComp
    // undercompensates by 2-4 dB. Formula constants in PluginProcessor.cpp
    // (compressionCompensation) need re-tuning. Tracked as donor-side
    // punch-list, not a Dusk Studio regression.
    prepareCleanState();
    session.master().tapeEnabled.store (true,  std::memory_order_relaxed);
    session.master().tapeHQ.store      (false, std::memory_order_relaxed);

   #if DUSKSTUDIO_HAS_DUSK_DSP
    auto& tapeProc = engine.getMasterBus().getTapeProcessor();
    auto& apvts    = tapeProc.getAPVTS();
    auto* pIn      = apvts.getParameter ("inputGain");
    auto* pOut     = apvts.getParameter ("outputGain");
    auto* pAuto    = apvts.getParameter ("autoComp");
   #else
    return juce::String ("[SKIP] Master Tape gain audit - DUSKSTUDIO_HAS_DUSK_DSP not defined");
   #endif

    auto setNorm = [] (juce::AudioProcessorParameter* p, float n)
    {
        if (p != nullptr) p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, n));
    };
    auto normFromGainDb = [] (float db) { return (db + 12.0f) / 24.0f; };

    constexpr float postStripPeak = 0.5012f * 0.7071f;  // 0.3544 = -9.01 dBFS

    juce::String table;
    table << juce::String::formatted (
        "      Pre-tape peak (track 0 -> pan center): %.4f (%+.2f dBFS)\n",
        postStripPeak, ampToDb (postStripPeak));

    const float drives[]       = { -12.0f, -6.0f, 0.0f, +6.0f, +12.0f };
    const bool  autoCompModes[] = { true, false };

    float deltaAtDefault = 0.0f;
    bool  autoCompOnIsMonotonic = true;
    float prevDeltaAutoOn = std::numeric_limits<float>::infinity();
    for (bool ac : autoCompModes)
    {
        setNorm (pAuto, ac ? 1.0f : 0.0f);   // Choice "Off"/"On" -> 0 / 1
        setNorm (pOut,  normFromGainDb (0.0f));

        table << juce::String::formatted ("      autoComp=%s\n", ac ? "ON " : "OFF");
        for (float drDb : drives)
        {
            setNorm (pIn, normFromGainDb (drDb));
            auto m = runSynthetic (48000.0, 512, 16, 2,
                                    kInputAmpMinusSixDb, kToneHz, 24, 8);
            const float deltaDb = ampToDb (m.peakL / postStripPeak);
            table << juce::String::formatted (
                "        drive=%+6.1f dB -> post-tape L=%.4f (%+.2f dBFS), "
                "delta=%+.2f dB\n",
                drDb, m.peakL, ampToDb (m.peakL), deltaDb);
            if (ac && std::abs (drDb) < 0.001f)
                deltaAtDefault = deltaDb;
            if (ac)
            {
                if (deltaDb > prevDeltaAutoOn + 0.1f)
                    autoCompOnIsMonotonic = false;
                prevDeltaAutoOn = deltaDb;
            }
        }
    }

    setNorm (pIn,   normFromGainDb (0.0f));
    setNorm (pOut,  normFromGainDb (0.0f));
    setNorm (pAuto, 1.0f);

    const bool defaultInBand = std::abs (deltaAtDefault) < 3.0f;
    const bool pass          = defaultInBand && autoCompOnIsMonotonic;

    return juce::String::formatted (
        "%s Master Tape gain audit (sweep)\n%s"
        "      Default (autoComp ON, drive 0 dB): %+.2f dB vs unity "
        "(pass if |delta| < 3.0 dB and curve monotonic)\n"
        "      autoComp ON curve %s",
        fmtPassFail (pass).toRawUTF8(),
        table.toRawUTF8(),
        deltaAtDefault,
        autoCompOnIsMonotonic ? "monotonic decreasing - OK" : "not monotonic - INVESTIGATE");
}

namespace
{
// Goertzel sliding magnitude estimator. Returns the linear amplitude of a
// single discrete frequency component in the input buffer. O(N) per
// frequency; we only probe a handful so this is cheap compared to a full
// FFT and avoids the juce::dsp::FFT plumbing.
float goertzelMagnitude (const float* x, int n, double freqHz, double sr)
{
    if (n <= 0 || sr <= 0.0) return 0.0f;
    const double k     = (double) n * freqHz / sr;
    const double omega = 2.0 * juce::MathConstants<double>::pi * k / (double) n;
    const double coeff = 2.0 * std::cos (omega);
    double q0 = 0.0, q1 = 0.0, q2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        q0 = coeff * q1 - q2 + (double) x[i];
        q2 = q1;
        q1 = q0;
    }
    const double re  = q1 - q2 * std::cos (omega);
    const double im  = q2 * std::sin (omega);
    const double mag = std::sqrt (re * re + im * im);
    return (float) (mag * 2.0 / (double) n);   // scale to amplitude
}

// Drive the engine with a tone for `warmup + measure` blocks, capture the
// last `captureSamples` L-channel samples for spectral analysis. Mirrors
// runSynthetic but exposes the raw buffer for FFT/Goertzel work.
void captureToneOutput (AudioEngine& engine, double sr, int blockSize,
                          int numInChannels, int numOutChannels,
                          float inputAmp, float toneHz,
                          int warmupBlocks, int captureSamples,
                          std::vector<float>& outBuffer)
{
    engine.prepareForSelfTest (sr, blockSize);

    std::vector<std::vector<float>> inputs ((size_t) numInChannels,
                                              std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<const float*> inputPtrs ((size_t) numInChannels, nullptr);
    for (int c = 0; c < numInChannels; ++c)
        inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs ((size_t) numOutChannels,
                                               std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<float*> outputPtrs ((size_t) numOutChannels, nullptr);
    for (int c = 0; c < numOutChannels; ++c)
        outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    juce::AudioIODeviceCallbackContext ctx {};
    const double phaseInc = 2.0 * juce::MathConstants<double>::pi * (double) toneHz / sr;
    double phase = 0.0;

    outBuffer.clear();
    outBuffer.reserve ((size_t) captureSamples);

    const int measureBlocks = (captureSamples + blockSize - 1) / blockSize;
    const int totalBlocks   = warmupBlocks + measureBlocks;
    for (int b = 0; b < totalBlocks; ++b)
    {
        for (int s = 0; s < blockSize; ++s)
        {
            inputs[0][(size_t) s] = inputAmp * (float) std::sin (phase);
            phase += phaseInc;
            if (phase >= 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;
        }
        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);
        engine.audioDeviceIOCallbackWithContext (
            inputPtrs.data(), numInChannels,
            outputPtrs.data(), numOutChannels,
            blockSize, ctx);
        if (b >= warmupBlocks)
            for (int s = 0; s < blockSize
                              && (int) outBuffer.size() < captureSamples; ++s)
                outBuffer.push_back (outputs[0][(size_t) s]);
    }
}
} // namespace

juce::String AudioPipelineSelfTest::testCompEachMode()
{
    // -18 dBFS @ 1 kHz through each comp mode on track 0. Goertzel measures
    // fundamental + first 4 harmonics; the 17 kHz bin acts as a "no-signal-
    // expected-here" alias floor (with 4x oversampling + half-band downsampler,
    // anything above ~6 kHz harmonic content should be deeply attenuated).
    //
    // Pass criteria per mode:
    //   * Fundamental > -25 dBFS (comp + saturation should not crush below this)
    //   * Alias floor at 17 kHz < -60 dBFS (donor saturation aliasing budget)
    //
    // Settings per mode are moderate (similar to real-world tracking use).
    constexpr double sr = 48000.0;
    constexpr int    bs = 512;
    constexpr int    captureSamples = 8192;
    constexpr int    warmupBlocks   = 32;     // ~340 ms; smoothers + envelopes settle
    constexpr float  inputAmp       = 0.1259f;   // -18 dBFS

    struct ModeSpec { int idx; const char* label; };
    const ModeSpec modes[] = {
        { 0, "Opto" }, { 1, "FET" }, { 2, "VCA" }
    };

    juce::String table;
    bool allPass = true;

    for (const auto& m : modes)
    {
        prepareCleanState();
        auto& t0s = session.track (0).strip;
        t0s.compEnabled.store (true, std::memory_order_relaxed);
        t0s.compMode   .store (m.idx, std::memory_order_relaxed);

        // Moderate, mode-appropriate defaults so all 3 actually engage on a
        // -18 dBFS sine + audibly compress without slamming the donor's
        // hard ±2.0 clip.
        t0s.compOptoPeakRed.store (50.0f, std::memory_order_relaxed);
        t0s.compOptoGain   .store (60.0f, std::memory_order_relaxed);
        t0s.compFetInput   .store (12.0f, std::memory_order_relaxed);
        t0s.compFetOutput  .store ( 0.0f, std::memory_order_relaxed);
        t0s.compFetAttack  .store ( 1.0f, std::memory_order_relaxed);
        t0s.compFetRelease .store (200.0f, std::memory_order_relaxed);
        t0s.compFetRatio   .store (1, std::memory_order_relaxed);   // 8:1
        t0s.compVcaThreshDb.store (-24.0f, std::memory_order_relaxed);
        t0s.compVcaRatio   .store (4.0f, std::memory_order_relaxed);
        t0s.compVcaAttack  .store (5.0f, std::memory_order_relaxed);
        t0s.compVcaRelease .store (100.0f, std::memory_order_relaxed);
        t0s.compVcaOutput  .store (0.0f, std::memory_order_relaxed);

        std::vector<float> buf;
        captureToneOutput (engine, sr, bs, 16, 2, inputAmp, kToneHz,
                            warmupBlocks, captureSamples, buf);

        const int n = (int) buf.size();
        const float fund   = goertzelMagnitude (buf.data(), n, 1000.0,  sr);
        const float h2     = goertzelMagnitude (buf.data(), n, 2000.0,  sr);
        const float h3     = goertzelMagnitude (buf.data(), n, 3000.0,  sr);
        const float h4     = goertzelMagnitude (buf.data(), n, 4000.0,  sr);
        const float h5     = goertzelMagnitude (buf.data(), n, 5000.0,  sr);
        const float alias  = goertzelMagnitude (buf.data(), n, 17000.0, sr);
        const float harmonicRms = std::sqrt (h2*h2 + h3*h3 + h4*h4 + h5*h5);
        const float thd     = fund > 0.0f ? harmonicRms / fund : 0.0f;

        const bool fundOk  = ampToDb (fund)  > -25.0f;
        const bool aliasOk = ampToDb (alias) < -60.0f;
        const bool pass    = fundOk && aliasOk;
        if (! pass) allPass = false;

        table << juce::String::formatted (
            "      %s  fund=%+6.2f dBFS  H2=%+6.2f  H3=%+6.2f  H4=%+6.2f  "
            "H5=%+6.2f  THD=%5.2f%%  alias17k=%+7.2f dBFS  %s\n",
            m.label,
            ampToDb (fund),
            ampToDb (h2),
            ampToDb (h3),
            ampToDb (h4),
            ampToDb (h5),
            thd * 100.0f,
            ampToDb (alias),
            pass ? "OK" : "FAIL");
    }

    return juce::String::formatted (
        "%s Channel comp (Opto / FET / VCA) on -18 dBFS @ 1 kHz\n%s",
        fmtPassFail (allPass).toRawUTF8(),
        table.toRawUTF8());
}

juce::String AudioPipelineSelfTest::testCompHeavyGR()
{
    // Heavy gain-reduction stress: push each mode to its aggressive corner
    // and measure how much harmonic + alias energy comes out. This run is
    // CHARACTERIZATION ONLY (no pass/fail) - distortion under heavy GR is
    // partially inherent (fast-attack envelope chatter, asymmetric sat) and
    // partially side-effect (donor hard clip when makeup pushes hot). The
    // table tells us which.
    //
    // Drive level raised to -6 dBFS so heavy comp settings actually engage.
    constexpr double sr = 48000.0;
    constexpr int    bs = 512;
    constexpr int    captureSamples = 8192;
    constexpr int    warmupBlocks   = 48;
    constexpr float  inputAmp       = 0.5012f;   // -6 dBFS, hot

    struct Spec
    {
        int idx; const char* label;
        // mode-specific extreme settings
        float opto_peakRed; float opto_gain;
        float fet_input;    float fet_output;    float fet_attack;
        int   fet_ratio;    float fet_release;
        float vca_thresh;   float vca_ratio;     float vca_attack;
        float vca_release;  float vca_output;
    };
    const Spec specs[] = {
        // Opto: max peak-red + high gain = LA-2A "slammed" sound
        { 0, "Opto-extreme",  100.0f, 80.0f,
                                 0.0f, 0.0f, 1.0f, 0, 200.0f,
                                 0.0f, 4.0f, 5.0f, 100.0f, 0.0f },
        // FET: +40 dB input drive + ALL ratio = 1176 "British Mode" smash
        { 1, "FET-allbutton",  50.0f, 50.0f,
                                 40.0f, -10.0f, 0.02f, 4, 50.0f,
                                 0.0f, 4.0f, 5.0f, 100.0f, 0.0f },
        // VCA: -36 dB thresh + 20:1 ratio + +18 dB makeup = brick limiter
        { 2, "VCA-brick",      50.0f, 50.0f,
                                 0.0f, 0.0f, 1.0f, 0, 200.0f,
                                 -36.0f, 20.0f, 1.0f, 50.0f, 12.0f }
    };

    juce::String table;
    for (const auto& s : specs)
    {
        prepareCleanState();
        auto& t0s = session.track (0).strip;
        t0s.compEnabled.store (true, std::memory_order_relaxed);
        t0s.compMode   .store (s.idx, std::memory_order_relaxed);
        t0s.compOptoPeakRed.store (s.opto_peakRed, std::memory_order_relaxed);
        t0s.compOptoGain   .store (s.opto_gain,    std::memory_order_relaxed);
        t0s.compFetInput   .store (s.fet_input,    std::memory_order_relaxed);
        t0s.compFetOutput  .store (s.fet_output,   std::memory_order_relaxed);
        t0s.compFetAttack  .store (s.fet_attack,   std::memory_order_relaxed);
        t0s.compFetRelease .store (s.fet_release,  std::memory_order_relaxed);
        t0s.compFetRatio   .store (s.fet_ratio,    std::memory_order_relaxed);
        t0s.compVcaThreshDb.store (s.vca_thresh,   std::memory_order_relaxed);
        t0s.compVcaRatio   .store (s.vca_ratio,    std::memory_order_relaxed);
        t0s.compVcaAttack  .store (s.vca_attack,   std::memory_order_relaxed);
        t0s.compVcaRelease .store (s.vca_release,  std::memory_order_relaxed);
        t0s.compVcaOutput  .store (s.vca_output,   std::memory_order_relaxed);

        std::vector<float> buf;
        captureToneOutput (engine, sr, bs, 16, 2, inputAmp, kToneHz,
                            warmupBlocks, captureSamples, buf);

        const int n = (int) buf.size();
        const float fund  = goertzelMagnitude (buf.data(), n, 1000.0,  sr);
        const float h2    = goertzelMagnitude (buf.data(), n, 2000.0,  sr);
        const float h3    = goertzelMagnitude (buf.data(), n, 3000.0,  sr);
        const float h4    = goertzelMagnitude (buf.data(), n, 4000.0,  sr);
        const float h5    = goertzelMagnitude (buf.data(), n, 5000.0,  sr);
        const float alias = goertzelMagnitude (buf.data(), n, 17000.0, sr);
        const float harmonicRms = std::sqrt (h2*h2 + h3*h3 + h4*h4 + h5*h5);
        const float thd = fund > 0.0f ? harmonicRms / fund : 0.0f;
        float peak = 0.0f;
        for (float v : buf) if (std::abs (v) > peak) peak = std::abs (v);

        table << juce::String::formatted (
            "      %s  peak=%+6.2f  fund=%+6.2f  H2=%+6.2f  H3=%+6.2f  "
            "H4=%+6.2f  H5=%+6.2f  THD=%6.2f%%  alias17k=%+7.2f\n",
            s.label,
            ampToDb (peak),
            ampToDb (fund),
            ampToDb (h2),
            ampToDb (h3),
            ampToDb (h4),
            ampToDb (h5),
            thd * 100.0f,
            ampToDb (alias));
    }

    return juce::String::formatted (
        "[INFO] Channel comp heavy-GR characterization (-6 dBFS in, extreme settings)\n%s"
        "      Heavy-GR distortion is partly intrinsic (envelope chatter,\n"
        "      asymmetric saturation) and partly side-effect (donor clip if\n"
        "      makeup pushes hot). 17 kHz floor < -50 dBFS = OS doing its job.",
        table.toRawUTF8());
}

juce::String AudioPipelineSelfTest::testCompPerTrack()
{
    // Wiring uniformity: run the same Opto-comp config on each of the 16
    // tracks one at a time. Output peaks must agree within tolerance,
    // otherwise something is wired differently per-track (regression).
    constexpr double sr = 48000.0;
    constexpr int    bs = 512;
    constexpr int    captureSamples = 4096;
    constexpr int    warmupBlocks   = 24;
    constexpr float  inputAmp       = 0.1259f;

    float peaks[Session::kNumTracks] {};
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        // Unmute target track, mute the rest. Inputs land on track t when
        // its inputSource follows the track index (-2 = follow).
        for (int i = 0; i < Session::kNumTracks; ++i)
        {
            auto& ts = session.track (i).strip;
            ts.faderDb     .store (0.0f, std::memory_order_relaxed);
            ts.pan         .store (0.0f, std::memory_order_relaxed);
            ts.mute        .store (i != t, std::memory_order_relaxed);
            ts.solo        .store (false, std::memory_order_relaxed);
            ts.compEnabled .store (i == t, std::memory_order_relaxed);
            ts.compMode    .store (0, std::memory_order_relaxed);   // Opto
            ts.compOptoPeakRed.store (50.0f, std::memory_order_relaxed);
            ts.compOptoGain   .store (60.0f, std::memory_order_relaxed);
            // Force every tested track direct-to-master; a live bus assignment
            // would route it through bus EQ/comp/fader and skew the per-track
            // peak comparison. (saveState/restoreState round-trip the routing.)
            for (int b = 0; b < ChannelStripParams::kNumBuses; ++b)
                ts.busAssign[(size_t) b].store (false, std::memory_order_relaxed);
            session.track (i).inputMonitor.store (i == t, std::memory_order_relaxed);
            session.track (i).recordArmed .store (false, std::memory_order_relaxed);
            session.track (i).inputSource .store (t, std::memory_order_relaxed);
        }
        session.recomputeRtCounters();

        // Drive input channel t (track t reads from input t when source is
        // explicit). captureToneOutput puts the sine on input 0, so we
        // temporarily route track t to follow input 0 via inputSource=0.
        session.track (t).inputSource.store (0, std::memory_order_relaxed);

        std::vector<float> buf;
        captureToneOutput (engine, sr, bs, 16, 2, inputAmp, kToneHz,
                            warmupBlocks, captureSamples, buf);

        float peak = 0.0f;
        for (float v : buf) if (std::abs (v) > peak) peak = std::abs (v);
        peaks[t] = peak;
    }

    float minP = peaks[0], maxP = peaks[0];
    for (int t = 1; t < Session::kNumTracks; ++t)
    {
        minP = juce::jmin (minP, peaks[t]);
        maxP = juce::jmax (maxP, peaks[t]);
    }
    const float spreadDb = (minP > 0.0f)
                              ? std::abs (ampToDb (maxP) - ampToDb (minP))
                              : 999.0f;
    const bool pass = spreadDb < 0.5f && minP > 0.01f;

    return juce::String::formatted (
        "%s Comp wiring uniformity across 16 tracks (Opto)\n"
        "      peak min=%+6.2f dBFS  max=%+6.2f dBFS  spread=%.3f dB "
        "(pass if spread < 0.5 dB AND min > -40 dBFS)",
        fmtPassFail (pass).toRawUTF8(),
        ampToDb (minP),
        ampToDb (maxP),
        spreadDb);
}

juce::String AudioPipelineSelfTest::probeUMC1820AlsaFormat()
{
    // Explicitly open the UMC1820 front: device on the ALSA backend so the
    // device-open stderr line shows what format/access mode plug negotiated
    // for THAT specific PCM. Verifies the format-pin patch (S24_3LE first
    // for non-hw: devices) is taking effect on the actual hardware the user
    // selects in the dialog.
    juce::String out;
    out << "--- UMC1820 ALSA Direct Open Probe ---\n";

    const auto origType  = deviceManager.getCurrentAudioDeviceType();
    const auto origSetup = deviceManager.getAudioDeviceSetup();

    deviceManager.setCurrentAudioDeviceType ("ALSA", true);

    // JUCE-ALSA exposes UMC1820 outputs under several surround-mode names -
    // probe the most common one used in the dialog.
    const juce::StringArray candidates {
        "UMC1820, USB Audio; Front output / input",
        "UMC1820, USB Audio; 4.0 Surround output to Front and Rear speakers"
    };

    for (const auto& name : candidates)
    {
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName        = name;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;

        const auto err = deviceManager.setAudioDeviceSetup (setup, /*treatAsChosen*/ true);
        if (err.isNotEmpty())
        {
            out << "  " << name << ": ERROR " << err << "\n";
            continue;
        }

        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            out << juce::String::formatted (
                "  %s\n      OPENED rate=%.0f buf=%d in=%d out=%d "
                "(see [Dusk Studio/JUCE-ALSA] line in stderr for format/access)\n",
                name.toRawUTF8(),
                dev->getCurrentSampleRate(),
                dev->getCurrentBufferSizeSamples(),
                dev->getActiveInputChannels().countNumberOfSetBits(),
                dev->getActiveOutputChannels().countNumberOfSetBits());
        }
        else
        {
            out << "  " << name << ": OPEN-FAILED (getCurrentAudioDevice() == nullptr)\n";
        }
    }

    deviceManager.setCurrentAudioDeviceType (origType, true);
    deviceManager.setAudioDeviceSetup (origSetup, true);
    return out;
}

juce::String AudioPipelineSelfTest::testBackendsOpenCleanly()
{
    juce::String out;
    out << "--- Backend Open Tests ---\n";

    // Capture the user's current setup so we can restore.
    const auto origType  = deviceManager.getCurrentAudioDeviceType();
    const auto origSetup = deviceManager.getAudioDeviceSetup();

    auto& types = deviceManager.getAvailableDeviceTypes();
    for (auto* type : types)
    {
        if (type == nullptr) continue;

        type->scanForDevices();
        const auto outDevs = type->getDeviceNames (false);
        const auto inDevs  = type->getDeviceNames (true);

        out << "  Backend: " << type->getTypeName()
            << " | " << outDevs.size() << " out devs, " << inDevs.size() << " in devs\n";
        for (const auto& d : outDevs) out << "      out: " << d << "\n";
        for (const auto& d : inDevs)  out << "      in:  " << d << "\n";

        // Try opening the type's default device.
        deviceManager.setCurrentAudioDeviceType (type->getTypeName(), /*treatAsChosenDevice*/ true);
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            out << juce::String::formatted (
                "      OPENED  rate=%.0f buf=%d in=%d out=%d\n",
                dev->getCurrentSampleRate(),
                dev->getCurrentBufferSizeSamples(),
                dev->getActiveInputChannels().countNumberOfSetBits(),
                dev->getActiveOutputChannels().countNumberOfSetBits());
        }
        else
        {
            out << "      OPEN-FAILED (deviceManager.getCurrentAudioDevice() returned nullptr)\n";
        }
    }

    // Restore.
    deviceManager.setCurrentAudioDeviceType (origType, /*treatAsChosenDevice*/ true);
    deviceManager.setAudioDeviceSetup (origSetup, /*treatAsChosenDevice*/ true);
    out << "  (restored original setup: " << origType << ")\n";
    return out;
}

juce::String AudioPipelineSelfTest::runAll()
{
    juce::StringArray report;
    report.add ("=== Dusk Studio Audio Pipeline Self-Test ===");
    report.add ("Time: " + juce::Time::getCurrentTime().toString (true, true));
    {
        const auto& setup = deviceManager.getAudioDeviceSetup();
        report.add (juce::String::formatted (
            "Active backend: %s, %s out, %s in, %.0f Hz, %d-sample buffer",
            deviceManager.getCurrentAudioDeviceType().toRawUTF8(),
            setup.outputDeviceName.toRawUTF8(),
            setup.inputDeviceName.toRawUTF8(),
            setup.sampleRate,
            setup.bufferSize));
    }
    report.add ("");

    // Save state, detach engine.
    const auto savedSession = saveState();
    deviceManager.removeAudioCallback (&engine);

    report.add ("--- Synthetic Engine Pipeline Tests (no hardware) ---");
    report.add (testPassThroughUnity());
    report.add (testMuteSilences());
    report.add (testMasterFaderMinusSix());
    report.add (testMasterCompNoNoiseFloor());
    report.add (testBusSoloMutesDirect());
    report.add (testChannelRoutingTwoOut());
    report.add (testChannelRoutingFourOut());
    report.add (testMasterTapeAddsGain());
    report.add (testCompEachMode());
    report.add (testCompHeavyGR());
    report.add (testCompPerTrack());
    report.add ("");

   #if defined(__linux__)
    // Pure-logic self-test for the Dusk Studio-owned ALSA backend. Covers the
    // converter math, channel-mask routing, hw:CARD,DEV parsing, and the
    // periods-knob clamping. Real-device opens of the backend are
    // exercised by the backend cycle below alongside the JUCE-stock
    // ALSA / JACK paths.
    report.add (AlsaAudioIODevice::runSelfTest());
    report.add ("");
   #endif

    // Restore session state, then re-attach the engine BEFORE the backend
    // cycle. Without this, every setCurrentAudioDeviceType / setAudioDeviceSetup
    // below opens the hardware PCM with no audio callback registered, so the
    // device thread streams whatever's in the kernel/driver buffer
    // (uninitialised memory, stale samples, or PipeWire-graph residue).
    // Audibly that's the "test button distortion" - speakers connected during
    // the test cycle hear noise on each device transition. With the engine
    // attached, the audio thread writes a silent master mix on every
    // callback (no inputs to process), so transitions are clean.
    restoreState (savedSession);
    deviceManager.addAudioCallback (&engine);

    report.add (testBackendsOpenCleanly());
    report.add ("");
    report.add (probeUMC1820AlsaFormat());

    report.add ("");
    report.add ("=== End of Self-Test ===");
    return report.joinIntoString ("\n");
}

juce::String AudioPipelineSelfTest::runPerfBenchmark (const juce::String& label,
                                                      double sampleRate, int blockSize,
                                                      int numActiveTracks,
                                                      bool eqEnabled, bool compEnabled,
                                                      bool tapeOn, int oversamplingFactor,
                                                      int numWarmupBlocks, int numMeasureBlocks)
{
    // Configure session for the requested load. prepareCleanState mutes
    // tracks 1..15 - un-mute the requested count and force a non-trivial
    // signal flow (un-bypassed comp / EQ where requested).
    prepareCleanState();

    // Global ox factor is consumed at engine.prepareForSelfTest time below,
    // so set it here before the prepare call.
    const int oxClamped = (oversamplingFactor == 2 || oversamplingFactor == 4) ? oversamplingFactor : 1;
    session.oversamplingFactor.store (oxClamped, std::memory_order_relaxed);

    for (int t = 0; t < numActiveTracks; ++t)
    {
        auto& s = session.track (t).strip;
        s.mute.store        (false, std::memory_order_relaxed);
        s.faderDb.store     (-6.0f, std::memory_order_relaxed);
        s.pan.store         (0.0f,  std::memory_order_relaxed);
        s.compEnabled.store (compEnabled, std::memory_order_relaxed);
        s.hpfEnabled.store  (eqEnabled,   std::memory_order_relaxed);
        // Slightly off-flat EQ so the BritishEQProcessor coefficients are
        // not all unity (which the impl might short-circuit).
        s.lfGainDb.store    (eqEnabled ? 1.5f  : 0.0f, std::memory_order_relaxed);
        s.lmGainDb.store    (eqEnabled ? -1.0f : 0.0f, std::memory_order_relaxed);
        s.hmGainDb.store    (eqEnabled ? 1.0f  : 0.0f, std::memory_order_relaxed);
        s.hfGainDb.store    (eqEnabled ? 2.0f  : 0.0f, std::memory_order_relaxed);
        session.track (t).inputMonitor.store (true, std::memory_order_relaxed);
    }
    session.master().tapeEnabled.store (tapeOn, std::memory_order_relaxed);
    // tapeHQ is legacy - global oversampling factor is now the source of truth.
    session.master().tapeHQ.store      (false, std::memory_order_relaxed);

    engine.prepareForSelfTest (sampleRate, blockSize);

    // Per-input synthetic sine. Different freq per channel so each EQ band
    // sees real energy and the comp's detector isn't running on identical
    // signal across channels.
    constexpr int kInChannels  = 16;
    constexpr int kOutChannels = 2;
    std::vector<std::vector<float>> inputs ((size_t) kInChannels,
                                              std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<const float*> inputPtrs ((size_t) kInChannels, nullptr);
    for (int c = 0; c < kInChannels; ++c)
        inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs ((size_t) kOutChannels,
                                               std::vector<float> ((size_t) blockSize, 0.0f));
    std::vector<float*> outputPtrs ((size_t) kOutChannels, nullptr);
    for (int c = 0; c < kOutChannels; ++c)
        outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    juce::AudioIODeviceCallbackContext ctx {};

    std::vector<double> phase ((size_t) kInChannels, 0.0);
    std::vector<double> phaseInc ((size_t) kInChannels, 0.0);
    for (int c = 0; c < kInChannels; ++c)
        phaseInc[(size_t) c] =
            2.0 * juce::MathConstants<double>::pi * (200.0 + 50.0 * c) / sampleRate;
    constexpr float inputAmp = 0.1f;

    const int totalBlocks = numWarmupBlocks + numMeasureBlocks;
    std::vector<double> times;
    times.reserve ((size_t) numMeasureBlocks);

    for (int b = 0; b < totalBlocks; ++b)
    {
        for (int c = 0; c < kInChannels; ++c)
        {
            for (int s = 0; s < blockSize; ++s)
            {
                inputs[(size_t) c][(size_t) s] = inputAmp * (float) std::sin (phase[(size_t) c]);
                phase[(size_t) c] += phaseInc[(size_t) c];
                if (phase[(size_t) c] >= 2.0 * juce::MathConstants<double>::pi)
                    phase[(size_t) c] -= 2.0 * juce::MathConstants<double>::pi;
            }
        }

        const auto t0 = juce::Time::getHighResolutionTicks();
        engine.audioDeviceIOCallbackWithContext (
            inputPtrs.data(), kInChannels,
            outputPtrs.data(), kOutChannels,
            blockSize, ctx);
        const auto t1 = juce::Time::getHighResolutionTicks();

        if (b >= numWarmupBlocks)
        {
            const double seconds = juce::Time::highResolutionTicksToSeconds (t1 - t0);
            times.push_back (seconds * 1.0e6);  // micros
        }
    }

    if (times.empty())
        return label + ": (no measurement blocks)";

    std::sort (times.begin(), times.end());
    const double median = times[times.size() / 2];
    const double p95    = times[(size_t) ((double) times.size() * 0.95)];
    const double p99    = times[(size_t) ((double) times.size() * 0.99)];
    const double maxV   = times.back();
    const double minV   = times.front();
    const double budgetUs = 1.0e6 * (double) blockSize / sampleRate;

    int overruns = 0;
    for (auto v : times) if (v > budgetUs) ++overruns;
    const double headroomPctMedian = 100.0 * (1.0 - median / budgetUs);

    return juce::String::formatted (
        "%s %s  median=%.1f us  p95=%.1f us  p99=%.1f us  max=%.1f us  min=%.1f us  "
        "(budget=%.1f us, headroom@median=%.1f%%, overruns=%d/%d)",
        overruns > 0 ? "[OVER]" : "[OK]",
        label.toRawUTF8(),
        median, p95, p99, maxV, minV,
        budgetUs, headroomPctMedian,
        overruns, (int) times.size());
}

juce::String AudioPipelineSelfTest::runPerfSuite()
{
    juce::StringArray report;
    report.add ("=== Dusk Studio Engine Perf Benchmark ===");
    report.add (juce::String ("Time: ") + juce::Time::getCurrentTime().toString (true, true));
    report.add ("Measures pure-engine callback wall time across configs.");
    report.add ("All callbacks driven directly via audioDeviceIOCallbackWithContext;");
    report.add ("no audio device, no PipeWire, no ALSA - engine DSP only.");
    report.add ("");

    const auto saved = saveState();
    deviceManager.removeAudioCallback (&engine);

    constexpr int warm = 32;
    constexpr int meas = 512;

    struct Config
    {
        const char* label;
        double sr;
        int    bs;
    };
    const Config configs[] = {
        { "48k/128 ", 48000.0, 128 },
        { "48k/256 ", 48000.0, 256 },
        { "48k/512 ", 48000.0, 512 },
        { "96k/256 ", 96000.0, 256 },
        { "96k/512 ", 96000.0, 512 },
        { "96k/1024", 96000.0, 1024 },
    };

    for (const auto& cfg : configs)
    {
        report.add (juce::String ("--- ") + cfg.label + " ---");
        report.add (runPerfBenchmark ("idle (1 track, EQ off, comp off)            ",
                                       cfg.sr, cfg.bs, 1, false, false, false, /*ox*/ 1,
                                       warm, meas));
        // EQ-only and comp-only on 16ch isolates where the per-channel cost lives.
        report.add (runPerfBenchmark ("16ch EQ only  (comp off)                    ",
                                       cfg.sr, cfg.bs, 16, true,  false, false, /*ox*/ 1,
                                       warm, meas));
        report.add (runPerfBenchmark ("16ch comp only (EQ off)                     ",
                                       cfg.sr, cfg.bs, 16, false, true,  false, /*ox*/ 1,
                                       warm, meas));
        // ASCII-only labels - these go straight into a TextEditor display
        // and would mojibake without UTF-8 wrapping. "x" reads naturally.
        report.add (runPerfBenchmark ("16ch + EQ + comp (full mixer) @1x           ",
                                       cfg.sr, cfg.bs, 16, true,  true,  false, /*ox*/ 1,
                                       warm, meas));
        report.add (runPerfBenchmark ("16ch + EQ + comp + tape on    @1x           ",
                                       cfg.sr, cfg.bs, 16, true,  true,  true,  /*ox*/ 1,
                                       warm, meas));
        report.add (runPerfBenchmark ("16ch + EQ + comp + tape on    @2x           ",
                                       cfg.sr, cfg.bs, 16, true,  true,  true,  /*ox*/ 2,
                                       warm, meas));
        report.add (runPerfBenchmark ("16ch + EQ + comp + tape on    @4x           ",
                                       cfg.sr, cfg.bs, 16, true,  true,  true,  /*ox*/ 4,
                                       warm, meas));
        report.add ("");
    }

    restoreState (saved);
    deviceManager.addAudioCallback (&engine);

    report.add ("=== End of Engine Perf Benchmark ===");
    return report.joinIntoString ("\n");
}
} // namespace duskstudio
