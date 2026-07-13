#include "AudioEngine.h"
#include "BounceEngine.h"
#include "PdcMath.h"
#include "RtPriority.h"
#include "../dsp/OutputPairRouting.h"
#include "McuReceiver.h"
#include "McuController.h"
#include "FaderBindingMap.h"
#include "../session/RegionEditActions.h"
#include "DeviceFallbackMessage.h"
#if defined(DUSKSTUDIO_HAS_ALSA)
  #include "alsa/AlsaAudioIODeviceType.h"
#endif
#include <array>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace duskstudio
{
// Log-span of the HPF sweep band, precomputed once. The MIDI-CC HPF map runs
// on the audio thread per controller event; without this it would recompute
// std::log(max/min) on every CC message.
static const float kHpfLogRange = std::log (ChannelStripParams::kHpfMaxHz
                                             / ChannelStripParams::kHpfMinHz);

// Soft-takeover read-back: the parameter's CURRENT position as the same 0..1
// fraction the apply switch below maps FROM. Each case is the exact inverse
// of its apply-side mapping - change one, change the other. Returns < 0 for
// targets pickup doesn't cover (discrete toggles, transport, plugin params -
// plugin values have no RT-safe read-back). Audio thread; relaxed loads only.
static float currentFracForTarget (Session& session, const MidiBinding& b) noexcept
{
    const auto inv = [] (float v, float lo, float hi)
    {
        return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
    };
    switch (b.target)
    {
        case MidiBindingTarget::TrackFader:
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumTracks) return -1.0f;
            return faderBindingDbToFrac (
                session.track (b.targetIndex).strip.faderDb.load (std::memory_order_relaxed),
                b.trigger == MidiBindingTrigger::PitchBend);
        case MidiBindingTarget::TrackPan:
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumTracks) return -1.0f;
            return inv (session.track (b.targetIndex).strip.pan.load (std::memory_order_relaxed),
                        -1.0f, 1.0f);
        case MidiBindingTarget::TrackAuxSend:
        {
            const int trk = unpackTrackAuxTrack (b.targetIndex);
            const int aux = unpackTrackAuxLane  (b.targetIndex);
            if (trk < 0 || trk >= Session::kNumTracks
                || aux < 0 || aux >= ChannelStripParams::kNumAuxSends) return -1.0f;
            const float db = session.track (trk).strip.auxSendDb[(size_t) aux]
                                 .load (std::memory_order_relaxed);
            if (db <= ChannelStripParams::kAuxSendOffDb + 0.01f) return 0.0f;
            return inv (db, ChannelStripParams::kAuxSendMinDb,
                            ChannelStripParams::kAuxSendMaxDb);
        }
        case MidiBindingTarget::TrackHpfFreq:
        {
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumTracks) return -1.0f;
            const auto& strip = session.track (b.targetIndex).strip;
            if (! strip.hpfEnabled.load (std::memory_order_relaxed)) return 0.0f;
            const float f = strip.hpfFreq.load (std::memory_order_relaxed);
            if (f <= ChannelStripParams::kHpfMinHz) return 0.0f;
            return juce::jlimit (0.0f, 1.0f,
                std::log (f / ChannelStripParams::kHpfMinHz) / kHpfLogRange);
        }
        case MidiBindingTarget::TrackEqGain:
        {
            const int trk  = unpackTrackEqTrack (b.targetIndex);
            const int band = unpackTrackEqBand  (b.targetIndex);
            if (trk < 0 || trk >= Session::kNumTracks
                || band < 0 || band >= kPackedEqBands) return -1.0f;
            const auto& s = session.track (trk).strip;
            const float db = band == 0 ? s.lfGainDb.load (std::memory_order_relaxed)
                           : band == 1 ? s.lmGainDb.load (std::memory_order_relaxed)
                           : band == 2 ? s.hmGainDb.load (std::memory_order_relaxed)
                                       : s.hfGainDb.load (std::memory_order_relaxed);
            return inv (db, -15.0f, 15.0f);
        }
        case MidiBindingTarget::TrackEqFreq:
        {
            const int trk  = unpackTrackEqTrack (b.targetIndex);
            const int band = unpackTrackEqBand  (b.targetIndex);
            if (trk < 0 || trk >= Session::kNumTracks
                || band < 0 || band >= kPackedEqBands) return -1.0f;
            const auto& s = session.track (trk).strip;
            const auto logInv = [] (float f, float lo, float hi)
            {
                if (f <= lo) return 0.0f;
                return juce::jlimit (0.0f, 1.0f, std::log (f / lo) / std::log (hi / lo));
            };
            switch (band)
            {
                case 0: return logInv (s.lfFreq.load (std::memory_order_relaxed), ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax);
                case 1: return logInv (s.lmFreq.load (std::memory_order_relaxed), ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax);
                case 2: return logInv (s.hmFreq.load (std::memory_order_relaxed), ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax);
                default: return logInv (s.hfFreq.load (std::memory_order_relaxed), ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax);
            }
        }
        case MidiBindingTarget::TrackEqQ:
        {
            const int trk  = unpackTrackEqTrack (b.targetIndex);
            const int band = unpackTrackEqBand  (b.targetIndex);
            if (trk < 0 || trk >= Session::kNumTracks) return -1.0f;
            const auto& s = session.track (trk).strip;
            if (band == 1) return inv (s.lmQ.load (std::memory_order_relaxed),
                                       ChannelStripParams::kBandQMin, ChannelStripParams::kBandQMax);
            if (band == 2) return inv (s.hmQ.load (std::memory_order_relaxed),
                                       ChannelStripParams::kBandQMin, ChannelStripParams::kBandQMax);
            return -1.0f;
        }
        case MidiBindingTarget::TrackCompThresh:
        case MidiBindingTarget::TrackCompMakeup:
        {
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumTracks) return -1.0f;
            const auto& s = session.track (b.targetIndex).strip;
            const int mode = juce::jlimit (0, 2, s.compMode.load (std::memory_order_relaxed));
            if (b.target == MidiBindingTarget::TrackCompMakeup)
                switch (mode)
                {
                    case 0:  return inv (s.compOptoGain .load (std::memory_order_relaxed),   0.0f, 100.0f);
                    case 1:  return inv (s.compFetOutput.load (std::memory_order_relaxed), -20.0f,  20.0f);
                    default: return inv (s.compVcaOutput.load (std::memory_order_relaxed), -20.0f,  20.0f);
                }
            switch (mode)
            {
                case 0:  return inv (s.compOptoPeakRed.load (std::memory_order_relaxed),   0.0f, 100.0f);
                case 1:  return inv (s.compFetInput   .load (std::memory_order_relaxed), -20.0f,  40.0f);
                default: return inv (s.compVcaThreshDb.load (std::memory_order_relaxed), -38.0f,  12.0f);
            }
        }
        case MidiBindingTarget::BusFader:
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumBuses) return -1.0f;
            return faderBindingDbToFrac (
                session.bus (b.targetIndex).strip.faderDb.load (std::memory_order_relaxed),
                b.trigger == MidiBindingTrigger::PitchBend);
        case MidiBindingTarget::BusPan:
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumBuses) return -1.0f;
            return inv (session.bus (b.targetIndex).strip.pan.load (std::memory_order_relaxed),
                        -1.0f, 1.0f);
        case MidiBindingTarget::BusEqGain:
        {
            const int bus  = unpackBusEqBus  (b.targetIndex);
            const int band = unpackBusEqBand (b.targetIndex);
            if (bus < 0 || bus >= Session::kNumBuses) return -1.0f;
            const auto& s = session.bus (bus).strip;
            const float db = band == 0 ? s.eqLfGainDb .load (std::memory_order_relaxed)
                           : band == 1 ? s.eqMidGainDb.load (std::memory_order_relaxed)
                                       : s.eqHfGainDb .load (std::memory_order_relaxed);
            return inv (db, -9.0f, 9.0f);
        }
        case MidiBindingTarget::AuxLaneFader:
            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumAuxLanes) return -1.0f;
            return faderBindingDbToFrac (
                session.auxLane (b.targetIndex).params.returnLevelDb.load (std::memory_order_relaxed),
                b.trigger == MidiBindingTrigger::PitchBend);
        case MidiBindingTarget::MasterFader:
            return faderBindingDbToFrac (
                session.master().faderDb.load (std::memory_order_relaxed),
                b.trigger == MidiBindingTrigger::PitchBend);
        case MidiBindingTarget::MasterEqLfBoost:
            return inv (session.master().eqLfBoost.load (std::memory_order_relaxed), 0.0f, 10.0f);
        case MidiBindingTarget::MasterEqHfBoost:
            return inv (session.master().eqHfBoost.load (std::memory_order_relaxed), 0.0f, 10.0f);
        case MidiBindingTarget::MasterCompThresh:
            return inv (session.master().compThreshDb.load (std::memory_order_relaxed), -38.0f, 12.0f);
        case MidiBindingTarget::MasterCompMakeup:
            return inv (session.master().compMakeupDb.load (std::memory_order_relaxed), -20.0f, 20.0f);
        case MidiBindingTarget::MasterCompRatio:
            return inv (session.master().compRatio.load (std::memory_order_relaxed), 1.0f, 20.0f);
        default:
            return -1.0f;
    }
}

// Per-machine audio device setup, alongside app-config.properties /
// window-state.txt (one concern per file). Restored at construction,
// persisted on every device change broadcast.
static juce::File audioDeviceStateFile()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Dusk Studio");
    if (! dir.exists()) dir.createDirectory();
    return dir.getChildFile ("audio-device.xml");
}

#if DUSKSTUDIO_HAS_NATIVE_LV2
// Per-slot directory for LV2 FILE-BACKED state, under the session: track
// slots at state/lv2/trackNN, aux slots at state/lv2/auxA_slotS. Empty when
// the session has no directory yet - saves fall back to blob-only. Save As
// consolidation copies the whole state/ tree (SessionSerializer).
static std::filesystem::path lv2StateDirFor (Session& session, const juce::String& slotTag)
{
    const auto dir = session.getSessionDirectory();
    if (dir == juce::File()) return {};
    return std::filesystem::u8path (dir.getChildFile ("state").getChildFile ("lv2")
                                        .getChildFile (slotTag).getFullPathName().toStdString());
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 || DUSKSTUDIO_HAS_NATIVE_VST3
// Session-carried native plugin state blob (base64) -> bytes. Empty on any
// decode failure - callers treat "no state" and "bad state" the same.
static std::vector<uint8_t> decodeBase64Blob (const juce::String& s)
{
    std::vector<uint8_t> blob;
    if (s.isEmpty()) return blob;
    juce::MemoryBlock mb;
    if (mb.fromBase64Encoding (s) && mb.getSize() > 0)
        blob.assign (static_cast<const uint8_t*> (mb.getData()),
                     static_cast<const uint8_t*> (mb.getData()) + mb.getSize());
    return blob;
}
#endif

class AudioEngine::PerfReporter final : public juce::Timer
{
public:
    explicit PerfReporter (AudioEngine& e) : engine (e) { startTimer (2000); }
    ~PerfReporter() override { stopTimer(); }

private:
    void timerCallback() override { engine.printPerfTable(); }
    AudioEngine& engine;
};

void AudioEngine::printPerfTable()
{
    // The per-counter exchanges aren't a consistent snapshot - a block
    // can land between them, skewing the averages by ~1 block out of
    // the hundreds in a reporting window. Fine for a diagnostic.
    const auto blocks = perf.blocks.exchange (0, std::memory_order_relaxed);
    if (blocks <= 0) return;

    static const char* const names[PerfSections::kNumSections] = {
        "pre (midi/sync/bindings/automation/pdc/playback prep)",
        "strip DSP (24 tracks: playback read + chain + accumulate)",
        "meter + record tail",
        "bus loop",
        "aux loop",
        "master + metronome + output",
    };

    const double tps      = (double) juce::Time::getHighResolutionTicksPerSecond();
    const double sr       = currentSampleRate.load (std::memory_order_relaxed);
    const int    bs       = currentBlockSize.load (std::memory_order_relaxed);
    const double budgetUs = (sr > 0.0 && bs > 0) ? 1.0e6 * (double) bs / sr : 0.0;

    std::fprintf (stderr,
                  "[Dusk Studio/perf] %lld blocks @ %.0f Hz / %d samples (budget %.0f us)\n",
                  (long long) blocks, sr, bs, budgetUs);

    for (int s = 0; s < PerfSections::kNumSections; ++s)
    {
        const auto t = perf.ticks[(size_t) s].exchange (0, std::memory_order_relaxed);
        const double us = 1.0e6 * (double) t / tps / (double) blocks;
        std::fprintf (stderr, "  %-58s %8.1f us/block  %5.1f%%\n",
                      names[s], us, budgetUs > 0.0 ? 100.0 * us / budgetUs : 0.0);
    }

    const auto tot = perf.totalTicks.exchange (0, std::memory_order_relaxed);
    const double totUs = 1.0e6 * (double) tot / tps / (double) blocks;
    std::fprintf (stderr, "  %-58s %8.1f us/block  %5.1f%%\n",
                  "TOTAL callback", totUs,
                  budgetUs > 0.0 ? 100.0 * totUs / budgetUs : 0.0);
    std::fflush (stderr);
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 || DUSKSTUDIO_HAS_NATIVE_VST3
// Message-thread drain for the native slots' MIDI-binding rings (the audio
// thread's binding apply can't touch the instances' single-producer param
// rings directly). 30 Hz matches PluginSlot's own drain cadence; a tick over
// empty rings is one atomic load per slot.
class AudioEngine::NativeParamDrain final : public juce::Timer
{
public:
    explicit NativeParamDrain (AudioEngine& e) : engine (e) { startTimerHz (30); }
    ~NativeParamDrain() override { stopTimer(); }
    void timerCallback() override
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            auto& strip = engine.getChannelStrip (t);
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            strip.getNativeClapSlot().drainQueuedParamBindings();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            strip.getNativeLv2Slot().drainQueuedParamBindings();
            if (auto* inst = strip.getNativeLv2Slot().getInstance())
                inst->drainPatchFeedback();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            strip.getNativeVst3Slot().drainQueuedParamBindings();
#endif
        }
        for (int a = 0; a < Session::kNumAuxLanes; ++a)
        {
            auto& lane = engine.getAuxLaneStrip (a);
            for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
            {
#if DUSKSTUDIO_HAS_NATIVE_CLAP
                lane.getNativeClapSlot (s).drainQueuedParamBindings();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
                lane.getNativeLv2Slot (s).drainQueuedParamBindings();
                if (auto* inst = lane.getNativeLv2Slot (s).getInstance())
                    inst->drainPatchFeedback();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
                lane.getNativeVst3Slot (s).drainQueuedParamBindings();
#endif
            }
        }

#if DUSKSTUDIO_HAS_NATIVE_VST3
        // A plugin that signalled kLatencyChanged only reports the new value
        // across a setActive cycle: reactivate the flagged slots under one
        // engine fence, then let PDC re-align the mixer. Aux slots join the
        // sweep for their instances' sake even though aux latency doesn't
        // feed PDC.
        bool anyLatencyChanged = false;
        const double sr    = engine.getCurrentSampleRate();
        const int    block = engine.getCurrentBlockSize();
        if (sr > 0.0 && block > 0)
        {
            auto cycle = [&] (auto& slot)
            {
                if (auto* inst = slot.getInstance())
                    inst->refreshParamInfoIfChanged();
                if (! slot.consumeLatencyChanged() || ! slot.isLoaded()) return;
                if (! anyLatencyChanged) engine.suspendProcessing();
                anyLatencyChanged = true;
                std::string err;
                slot.reactivate (sr, block, err);
            };
            for (int t = 0; t < Session::kNumTracks; ++t)
                cycle (engine.getChannelStrip (t).getNativeVst3Slot());
            for (int a = 0; a < Session::kNumAuxLanes; ++a)
                for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
                    cycle (engine.getAuxLaneStrip (a).getNativeVst3Slot (s));
            if (anyLatencyChanged)
            {
                engine.resumeProcessing();
                engine.recomputePdc();
            }
        }
#endif
    }
private:
    AudioEngine& engine;
};
#endif

AudioEngine::AudioEngine (Session& sessionToBindTo, int initialWorkers)
    : session (sessionToBindTo), desiredWorkers (juce::jmax (0, initialWorkers))
{
    if (const char* p = std::getenv ("DUSKSTUDIO_PERF"); p != nullptr && p[0] == '1')
    {
        perf.enabled.store (true, std::memory_order_relaxed);
        perfReporter = std::make_unique<PerfReporter> (*this);
    }

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 || DUSKSTUDIO_HAS_NATIVE_VST3
    nativeParamDrain = std::make_unique<NativeParamDrain> (*this);
#endif

    // Held by unique_ptr so AudioEngine.h stays free of McuReceiver /
    // McuController definitions.
    mcuReceiver   = std::make_unique<McuReceiver>   (session);
    mcuController = std::make_unique<McuController> (session);
    mcuController->setSink ([this] (const juce::MidiBuffer& buf)
    {
        // Message thread (30 Hz MCU tick). The bank's juce send overload keeps
        // McuController juce-typed (events-tower coupling).
        const int outIdx = session.mcu.resolvedOutputIdx.load (std::memory_order_acquire);
        if (outIdx >= 0) midiOut.send (outIdx, buf);
    });
    mcuController->setTransportProvider ([this] { return &transport; });
    mcuController->setSampleRateProvider ([this] { return getCurrentSampleRate(); });
    playbackEngine.bindTransport (transport);
    // 500 units covers thousands of edits while bounding memory under
    // multi-hour sessions; without this the stack grows unbounded.
    // 50 minimum kept even when total exceeds the cap.
    undoManager.setMaxNumberOfStoredUnits (500, 50);

    // Drain silent-output diagnostics on the message thread (1 Hz) so the audio
    // callback never touches stdio. Cheap when nothing has stalled.
    diagTimer.startTimer (1000);

    // Hosted plugins get this via setPlayHead so tempo-synced features
    // (LFOs, arps, delays) read live session BPM + playhead.
    playHead = std::make_unique<DuskStudioPlayHead> (transport,
                                                  &session.tempoBpm,
                                                  &currentSampleRate);

    // -1 sentinel so the first block sees a "swap" if a device is
    // already selected (harmless flush - no notes held yet).
    lastMidiInputIndex.fill (-1);

    publishTempoMap();   // seed the snapshot (empty -> constant tempoBpm)

    // High priority so a loaded message thread can't starve clock /
    // note delivery; still below the audio thread.
    midiOut.startPump();

    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i].bind (session.track (i).strip);
        strips[(size_t) i].bindPluginManager (pluginManager);
        strips[(size_t) i].bindHardwareInsert (session.track (i).hardwareInsert);
    }
    for (int i = 0; i < Session::kNumBuses; ++i)
        busStrips[(size_t) i].bind (session.bus (i).strip);
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        auxLaneStrips[(size_t) i].bind (session.auxLane (i).params);
        auxLaneStrips[(size_t) i].bindPluginManager (pluginManager);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
            auxLaneStrips[(size_t) i].bindHardwareInsert (
                s, session.auxLane (i).hardwareInserts[(size_t) s]);
    }
    master.bind (session.master());
    masteringChain.bind (session.mastering());

    // Linux: pre-register Dusk Studio's ALSA + JACK BEFORE
    // initialiseWithDefaultDevices so JUCE's createDeviceTypesIfNeeded
    // branch is a no-op (it only auto-registers defaults when the type
    // list is empty, which would re-add the stock ALSA path). Explicit
    // scanForDevices so init's pickCurrentDeviceTypeWithDevices doesn't
    // trip hasScanned assertions. Dropdown ends up with "ALSA" (ours)
    // + "JACK", no double-listing.
    // macOS / Windows let JUCE auto-register natives - DUSKSTUDIO_HAS_ALSA
    // isn't defined there.
   #if defined(DUSKSTUDIO_HAS_ALSA)
    if (auto* jackType = juce::AudioIODeviceType::createAudioIODeviceType_JACK())
        deviceManager.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (jackType));
    deviceManager.addAudioDeviceType (std::make_unique<AlsaAudioIODeviceType>());

    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t != nullptr) t->scanForDevices();
   #endif

   #if JUCE_WINDOWS
    // Windows backend preference. initialiseWithDefaultDevices' default pick
    // (AudioDeviceManager::pickCurrentDeviceTypeWithDevices) lands on the
    // FIRST registered type that has devices, and createDeviceTypesIfNeeded
    // only auto-registers JUCE's defaults when the list is empty. So
    // pre-registering in preference order both chooses the default backend
    // and prevents double-listing - same mechanism as the Linux block above.
    //
    // Order: ASIO (lowest latency; only compiled in when the SDK was present
    // at build time) -> WASAPI exclusive (low-latency, no SDK needed: our
    // fallback for machines with no ASIO driver) -> WASAPI shared -> DirectSound.
    // ASIO with no installed driver enumerates zero devices, so the pick falls
    // through to WASAPI exclusive automatically.
   #if JUCE_ASIO
    if (auto* asio = juce::AudioIODeviceType::createAudioIODeviceType_ASIO())
        deviceManager.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (asio));
   #endif
    if (auto* wasapiExclusive = juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (
            juce::WASAPIDeviceMode::exclusive))
        deviceManager.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (wasapiExclusive));
    if (auto* wasapiShared = juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (
            juce::WASAPIDeviceMode::shared))
        deviceManager.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (wasapiShared));
    if (auto* directSound = juce::AudioIODeviceType::createAudioIODeviceType_DirectSound())
        deviceManager.addAudioDeviceType (std::unique_ptr<juce::AudioIODeviceType> (directSound));

    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t != nullptr) t->scanForDevices();
   #endif

    // Restore the persisted device setup so the backend pick above only
    // decides the FIRST launch. Without this, every start re-runs the
    // default pick - on Windows that re-selects the first ASIO entry,
    // re-instantiating app-shim "drivers" (JRiver et al) the user already
    // switched away from. Null XML (fresh machine, unreadable file) makes
    // initialise behave exactly like initialiseWithDefaultDevices.
    std::unique_ptr<juce::XmlElement> savedDeviceState;
    if (const auto stateFile = audioDeviceStateFile(); stateFile.existsAsFile())
        savedDeviceState = juce::parseXML (stateFile);

    if (const auto err = deviceManager.initialise (16, 2, savedDeviceState.get(),
                                                   /*selectDefaultDeviceOnFailure*/ true);
        err.isNotEmpty())
    {
        std::fprintf (stderr,
                      "[Dusk Studio/AudioEngine] device-manager init reported: %s\n",
                      err.toRawUTF8());
    }

   #if defined(DUSKSTUDIO_HAS_ALSA)
    // The saved setup can pin an ALSA hw: device that another app (PipeWire,
    // JACK, another DAW) holds exclusively. JUCE's selectDefaultDeviceOnFailure
    // only re-picks another device NAME on the same ALSA type - the same hw
    // conflict - so it never falls through to JACK. Recover explicitly: JACK
    // (routes through PipeWire and reaches the interface even while the raw hw
    // handle is held) -> the first ALSA device that opens -> give up and tell the
    // user. This runs BEFORE addAudioCallback / addChangeListener: the audio
    // thread isn't attached and the engine isn't a change listener yet, so the
    // setup-switch broadcasts reach no one (no re-entrancy, no fallback loop).
    {
        auto working = [this]
        {
            auto* d = deviceManager.getCurrentAudioDevice();
            // A started SR alone isn't enough: a per-device ALSA name can resolve
            // to 0 active outputs (see the silent-failure guard further down), so
            // require real output channels too.
            return d != nullptr && d->getCurrentSampleRate() > 0.0
                && d->getActiveOutputChannels().countNumberOfSetBits() > 0;
        };

        const juce::String savedName = savedDeviceState != nullptr
            ? savedDeviceState->getStringAttribute ("audioOutputDeviceName",
                  savedDeviceState->getStringAttribute ("audioInputDeviceName"))
            : juce::String();

        if (! working())
        {
            // Clear the pinned (busy) device name + use default channels, else
            // setAudioDeviceSetup can short-circuit to a non-started, sr-0 setup.
            auto openDefaultOnType = [this] (const char* typeName, const juce::String& devName)
            {
                deviceManager.setCurrentAudioDeviceType (typeName, /*treatAsChosen*/ true);
                auto s = deviceManager.getAudioDeviceSetup();
                s.inputDeviceName.clear();
                s.outputDeviceName         = devName;   // empty = the type's default device
                s.useDefaultInputChannels  = true;
                s.useDefaultOutputChannels = true;
                // Drop the failed device's rate/buffer so JUCE picks each fallback
                // device's own defaults instead of carrying over unsupported values.
                s.sampleRate = 0;
                s.bufferSize = 0;
                deviceManager.setAudioDeviceSetup (s, /*treatAsChosen*/ true);
            };

            // 1) JACK / PipeWire default.
            openDefaultOnType ("JACK", juce::String());

            // 2) Walk ALSA device names; the busy one fails, the next (Built-in
            //    / HDMI / other interface) opens.
            if (! working())
                for (auto* t : deviceManager.getAvailableDeviceTypes())
                {
                    if (t == nullptr || t->getTypeName() != "ALSA") continue;
                    t->scanForDevices();
                    for (const auto& name : t->getDeviceNames (/*wantInputNames*/ false))
                    {
                        openDefaultOnType ("ALSA", name);
                        if (working()) break;
                    }
                    break;
                }
        }

        // Resolve the outcome unconditionally: this also catches the case where
        // JUCE's own selectDefaultDeviceOnFailure SILENTLY moved us onto a
        // different working device than the one saved - the user should still be
        // told their interface wasn't available. startupDeviceMessage() returns
        // empty when the saved device opened, so the normal path stays silent.
        auto* dev = deviceManager.getCurrentAudioDevice();
        const bool opened = working();
        startupDeviceMessage_ = duskstudio::startupDeviceMessage (
            opened, savedName.toStdString(), opened ? dev->getName().toStdString() : std::string());
    }
   #endif

    // Build both MIDI banks before ANY callback is registered - the audio
    // callback iterates perInputMidi, so it must not be attachable while
    // rebuildMidiBanks sizes the vector. Empty deviceIdentifier = every
    // enabled input fans out to midiIn, routed there by source identifier.
    midiIn.setDeviceManager (deviceManager);
    rebuildMidiBanks();
    midiIn.attachCallback();

    deviceManager.addAudioCallback (this);

    // Subscribe to AudioDeviceManager change broadcasts so we can
    // detect hot-unplug (current device becomes null while we had
    // one). Fires on the message thread per JUCE's ChangeBroadcaster
    // contract.
    deviceManager.addChangeListener (this);
}

void AudioEngine::refreshMidiInputs()
{
    // Detach both callbacks - JUCE's remove* joins the audio + MIDI
    // dispatch sides before returning. Cheapest correct sync for a
    // rare hot-plug event; lock-free vector mutation would cost more
    // throughout the audio path than it's worth.
    deviceManager.removeAudioCallback (this);
    midiIn.detachCallback();

    // Disable currently-enabled inputs before rebuilding so the device
    // manager's own bookkeeping releases the OS handles. The new pass
    // re-enables whichever devices are still present.
    midiIn.disableAllDevices();

    rebuildMidiBanks();

    // The track-side index atoms may now point at moved or removed
    // devices. Re-resolve each track's saved identifier so a refresh
    // doesn't silently break existing routing. Tracks with no saved
    // identifier (very old sessions) keep their raw index, clamped.
    auto resolveByIdentifier = [] (const std::vector<duskstudio::midi::MidiDeviceInfo>& devices,
                                    const juce::String& wantedId)
    {
        for (int i = 0; i < (int) devices.size(); ++i)
            if (devices[(size_t) i].identifier == wantedId)
                return i;
        return -1;
    };
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        if (track.midiInputIdentifier.isNotEmpty())
        {
            track.midiInputIndex.store (
                resolveByIdentifier (midiIn.getDevices(), track.midiInputIdentifier),
                std::memory_order_relaxed);
        }
        else
        {
            const int cur = track.midiInputIndex.load (std::memory_order_relaxed);
            if (cur >= (int) midiIn.getDevices().size())
                track.midiInputIndex.store (-1, std::memory_order_relaxed);
        }
        if (track.midiOutputIdentifier.isNotEmpty())
        {
            track.midiOutputIndex.store (
                resolveByIdentifier (midiOut.getDevices(), track.midiOutputIdentifier),
                std::memory_order_relaxed);
        }
        else
        {
            const int cur = track.midiOutputIndex.load (std::memory_order_relaxed);
            if (cur >= (int) midiOut.getDevices().size())
                track.midiOutputIndex.store (-1, std::memory_order_relaxed);
        }
    }

    // Must run BEFORE re-attaching callbacks - otherwise the audio
    // thread reads the output bank while ensureOpen mutates it.
    openConfiguredMidiOutputs();

    midiIn.attachCallback();
    deviceManager.addAudioCallback (this);

    sendChangeMessage();
}

void AudioEngine::publishTempoMap()
{
    // Message thread. Heap-allocate an immutable copy, keep it alive in the
    // pool, then release-store its pointer so the audio thread's next
    // acquire-load sees a fully-built map. The old copy is NOT freed here - a
    // concurrent callback may still hold it (publishes can be as frequent as a
    // drag's mouse-moves); the pool is trimmed in audioDeviceStopped, when no
    // callback can be in flight. Each copy is tiny (a few tempo points).
    auto fresh = std::make_unique<TempoMap> (session.tempoMap);
    const TempoMap* raw = fresh.get();
    rtTempoMapPool.push_back (std::move (fresh));
    rtTempoMap.store (raw, std::memory_order_release);

    // Backstop bound so a marathon drag session can't grow the pool without
    // limit between audioDeviceStopped reclamations. Generous on purpose: a
    // queued-event burst (post-stall drain of a tempo-handle drag) can land
    // many publishes inside ONE audio block while the callback still holds
    // an old map across its MIDI-region scheduling scan - a tight bound here
    // would free that map under the reader. 256 publishes inside a single
    // block is not a reachable burst; each map is a few tempo points.
    constexpr size_t kMaxRtTempoMaps = 256;
    if (rtTempoMapPool.size() > kMaxRtTempoMaps)
        rtTempoMapPool.erase (rtTempoMapPool.begin(),
                               rtTempoMapPool.end() - (std::ptrdiff_t) kMaxRtTempoMaps);
}

void AudioEngine::setTempoPoints (std::vector<TempoPoint> pts)
{
    session.tempoMap.setPoints (std::move (pts));
    // Keep the constant tempoBpm mirroring the bar-1 tempo so the transport
    // readout, count-in, and the empty-map fallback all show the right number
    // while a map is active. release pairs with the audio thread's acquire.
    if (! session.tempoMap.empty())
        session.tempoBpm.store (session.tempoMap.points().front().bpm,
                                 std::memory_order_release);
    publishTempoMap();
}

void AudioEngine::recomputePdc() noexcept
{
    int latency[Session::kNumTracks];
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& strip = strips[(size_t) t];
        int lat = 0;
        // MIDI tracks report 0: the instrument's latency is already absorbed by
        // the MIDI scheduling pre-shift (audioDeviceIOCallbackWithContext), so
        // their output is timeline-aligned as if zero-latency.
        const bool midi = session.track (t).mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Midi;
        // A frozen track plays its baked WAV with the insert bypassed, so its
        // plugin / hardware latency must not count toward PDC - it's a zero-latency
        // disk source (the bake was captured pre-PDC; see ChannelStrip freeze tap).
        const bool frozen = session.track (t).frozen.load (std::memory_order_relaxed);
        if (! midi && ! frozen)
        {
            const int mode = strip.insertMode.load (std::memory_order_relaxed);
            if (mode == ChannelStrip::kInsertPlugin)
            {
                lat = strip.getPluginSlot().getLatencySamples();
                // A native insert replaces the JUCE slot (which then reports 0),
                // so its latency must feed PDC instead. Skip it while bypassed -
                // bypass passes audio through at zero delay, matching the JUCE
                // slot's getLatencySamples() returning 0 when bypassed.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
                if (strip.isNativeClapLoaded() && ! strip.getNativeClapSlot().isBypassed())
                    if (auto* inst = strip.getNativeClapSlot().getInstance())
                        lat = inst->getLatencySamples();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
                if (strip.isNativeLv2Loaded() && ! strip.getNativeLv2Slot().isBypassed())
                    if (auto* inst = strip.getNativeLv2Slot().getInstance())
                        lat = inst->getLatencySamples();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
                if (strip.isNativeVst3Loaded() && ! strip.getNativeVst3Slot().isBypassed())
                    if (auto* inst = strip.getNativeVst3Slot().getInstance())
                        lat = inst->getLatencySamples();
#endif
            }
            else if (mode == ChannelStrip::kInsertHardware)
                lat = strip.getHardwareInsertSlot().getLatencySamples();
        }
        latency[t] = juce::jlimit (0, ChannelStrip::kMaxPdcSamples, lat);
    }

    int comp[Session::kNumTracks];
    const int deepest = pdc::computeCompensations (latency, comp, Session::kNumTracks);
    for (int t = 0; t < Session::kNumTracks; ++t)
        strips[(size_t) t].setPdcCompensationSamples (comp[t]);
    aggregatePdcLatencySamples.store (deepest, std::memory_order_relaxed);

    // Aux-lane (send-effect) latency delays only the wet return. Master-stage
    // targets: dry mix waits for the deepest lane, each return for
    // (deepest - own). Buses host no plugins, so delaying the mix after the
    // bus pass covers them too. Slots on a lane run in series - their
    // latencies sum.
    int auxLat[Session::kNumAuxLanes];
    int deepestAux = 0;
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = auxLaneStrips[(size_t) a];
        int laneLat = 0;
        for (int p = 0; p < AuxLaneStrip::kMaxPlugins; ++p)
        {
            const int mode = lane.insertMode[(size_t) p].load (std::memory_order_relaxed);
            if (mode == AuxLaneStrip::kInsertPlugin)
            {
                int slotLat = lane.getPluginSlot (p).getLatencySamples();
#if DUSKSTUDIO_HAS_NATIVE_CLAP
                if (lane.isNativeClapLoaded (p) && ! lane.getNativeClapSlot (p).isBypassed())
                    if (auto* inst = lane.getNativeClapSlot (p).getInstance())
                        slotLat = inst->getLatencySamples();
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
                if (lane.isNativeLv2Loaded (p) && ! lane.getNativeLv2Slot (p).isBypassed())
                    if (auto* inst = lane.getNativeLv2Slot (p).getInstance())
                        slotLat = inst->getLatencySamples();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
                if (lane.isNativeVst3Loaded (p) && ! lane.getNativeVst3Slot (p).isBypassed())
                    if (auto* inst = lane.getNativeVst3Slot (p).getInstance())
                        slotLat = inst->getLatencySamples();
#endif
                laneLat += juce::jmax (0, slotLat);
            }
            else if (mode == AuxLaneStrip::kInsertHardware)
                laneLat += juce::jmax (0, lane.getHardwareInsertSlot (p).getLatencySamples());
        }
        auxLat[a]  = juce::jlimit (0, ChannelStrip::kMaxPdcSamples, laneLat);
        deepestAux = juce::jmax (deepestAux, auxLat[a]);
    }
    masterDryPdcTarget.store (deepestAux, std::memory_order_relaxed);
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
        auxReturnPdcTarget[(size_t) a].store (deepestAux - auxLat[a],
                                               std::memory_order_relaxed);
}

void AudioEngine::applyMasterPdcTargetsNow() noexcept
{
    recomputePdc();

    masterDryPdcApplied = masterDryPdcTarget.load (std::memory_order_relaxed);
    masterDryPdcL.reset();
    masterDryPdcR.reset();
    masterDryPdcL.setDelay ((float) masterDryPdcApplied);
    masterDryPdcR.setDelay ((float) masterDryPdcApplied);
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        const int t = auxReturnPdcTarget[(size_t) a].load (std::memory_order_relaxed);
        auxReturnPdcApplied[(size_t) a] = t;
        auxReturnPdcL[(size_t) a].reset();
        auxReturnPdcR[(size_t) a].reset();
        auxReturnPdcL[(size_t) a].setDelay ((float) t);
        auxReturnPdcR[(size_t) a].setDelay ((float) t);
    }
}

bool AudioEngine::freezePrepare (int trackIndex, juce::File& outFile, std::int64_t& lenSamples)
{
    lastFreezeError.clear();
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks)
        { lastFreezeError = "Invalid track"; return false; }

    auto& track = session.track (trackIndex);
    if (track.frozen.load (std::memory_order_relaxed))
        { lastFreezeError = "Track is already frozen"; return false; }

    // Render length: the track's content end + a tail so the instrument's
    // release / FX ringout decays before the WAV cuts. MIDI tracks freeze the
    // instrument + EQ/comp; audio tracks freeze the recorded audio through its
    // insert + EQ/comp (a big win at high oversampling).
    const bool isMidi = track.mode.load (std::memory_order_relaxed) == (int) Track::Mode::Midi;
    std::int64_t contentEnd = 0;
    if (isMidi)
        for (const auto& mr : track.midiRegions.current())
            contentEnd = juce::jmax (contentEnd, mr.timelineStart + mr.lengthInSamples);
    else
        for (const auto& r : track.regions)
            contentEnd = juce::jmax (contentEnd, r.timelineStart + r.lengthInSamples);
    if (contentEnd <= 0)
        { lastFreezeError = "Track has no content to freeze"; return false; }

    const double sr = getCurrentSampleRate();
    if (sr <= 0.0)
        { lastFreezeError = "Audio device not running"; return false; }
    constexpr double kFreezeTailSeconds = 5.0;
    lenSamples = contentEnd + (std::int64_t) (sr * kFreezeTailSeconds);

    auto audioDir = session.getAudioDirectory();
    if (audioDir.createDirectory().failed())
        { lastFreezeError = "Could not create audio directory"; return false; }
    // Freeze WAVs live in a dedicated subdir so they can never overwrite (and later be
    // deleted by unfreezeTrack) a user file that happens to share the freeze name.
    auto freezeDir = audioDir.getChildFile ("freeze");
    if (freezeDir.createDirectory().failed())
        { lastFreezeError = "Could not create freeze directory"; return false; }
    outFile = freezeDir.getChildFile (
        "freeze_track" + juce::String (trackIndex + 1).paddedLeft ('0', 2) + ".wav");
    return true;
}

void AudioEngine::commitFreeze (int trackIndex, const juce::File& outFile, std::int64_t lenSamples)
{
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks)
        return;
    auto& track = session.track (trackIndex);

    // Point frozenRegion at the baked WAV: one stereo region spanning the song
    // from timeline 0 at unity, no fades.
    auto& fr = track.frozenRegion;
    fr = AudioRegion {};
    fr.file            = outFile;
    fr.timelineStart   = 0;
    fr.lengthInSamples = lenSamples;
    fr.sourceOffset    = 0;
    fr.numChannels     = 2;
    fr.gainDb          = 0.0f;
    track.frozenAudioPath = outFile.getFullPathName();

    // A frozen track can't record (playback is the baked WAV), so it must not stay
    // record-armed - disarm via setTrackArmed so armedTrackCount stays consistent
    // (done before frozen is set; setTrackArmed only refuses to ARM a frozen track).
    session.setTrackArmed (trackIndex, false);

    // Capture the user's current bypass before forcing it on, so unfreeze can
    // restore it (freeze always bypasses to free CPU). Then bypass the
    // instrument (defensive - the frozen strip path already skips it) and
    // publish frozen with release so the audio thread sees the flag only after
    // frozenRegion + the path are fully written.
    auto& slot = strips[(size_t) trackIndex].getPluginSlot();
    track.frozenPluginBypass.store (slot.isBypassed(), std::memory_order_relaxed);
    slot.setBypassed (true);
    track.frozen.store (true, std::memory_order_release);

    // Open the frozen WAV reader (and rebuild every track's readers).
    playbackEngine.preparePlayback();
}

void AudioEngine::unfreezeTrack (int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks)
        return;
    auto& track = session.track (trackIndex);
    if (! track.frozen.load (std::memory_order_relaxed))
        return;

    // Restore the pre-freeze bypass (not a blanket false) before clearing the
    // flag so a block observed between the two reads still routes through a
    // plugin in the user's intended bypass state.
    strips[(size_t) trackIndex].getPluginSlot().setBypassed (
        track.frozenPluginBypass.load (std::memory_order_relaxed));
    track.frozen.store (false, std::memory_order_release);

    // Rebuild readers without the frozen stream (closes the frozen WAV handle)
    // BEFORE deleting the file.
    playbackEngine.preparePlayback();

    // Delete the baked WAV - but only one we created: inside the session's audio
    // dir and named like our freeze output. A corrupt session could otherwise
    // point frozenAudioPath at an unrelated user file, and deleteFile() is
    // destructive.
    const juce::File wav (track.frozenAudioPath);
    // Match the EXACT name freezePrepare generates for THIS track, not just the
    // "freeze_track*" prefix - so a corrupt path can't delete another track's
    // freeze WAV (or any other freeze_track*.wav) by accident.
    const auto expectedName = "freeze_track"
                            + juce::String (trackIndex + 1).paddedLeft ('0', 2) + ".wav";
    const auto audioDir  = session.getAudioDirectory();
    const auto freezeDir = audioDir.getChildFile ("freeze");
    const auto parent    = wav.getParentDirectory();
    // Current freezes live in <audio>/freeze/; older sessions wrote straight into
    // <audio>/. Accept either parent so both round-trip, but still require the exact
    // per-track name so a corrupt path can't delete an unrelated file.
    const bool engineOwned = (parent == freezeDir || parent == audioDir)
                          && wav.getFileName() == expectedName;
    if (engineOwned && wav.existsAsFile())
        wav.deleteFile();
    track.frozenAudioPath.clear();
    track.frozenRegion = AudioRegion {};
}

void AudioEngine::reapplyFreezeState() noexcept
{
    for (int t = 0; t < Session::kNumTracks; ++t)
        if (session.track (t).frozen.load (std::memory_order_relaxed))
            strips[(size_t) t].getPluginSlot().setBypassed (true);
}

void AudioEngine::reresolveTrackMidiFromSession()
{
    // Re-map saved per-track MIDI identifiers to runtime indices against the
    // EXISTING device banks. Only atomic index stores (the audio thread reads
    // these), no bank mutation - so no callback detach, no device reconfigure,
    // no audio glitch. Cheap enough to run on every session load. (The full
    // refreshMidiInputs hot-plug path would detach/reattach the audio callback
    // and disable/re-enable every MIDI device - seconds of frozen UI on load.)
    auto resolveByIdentifier = [] (const std::vector<duskstudio::midi::MidiDeviceInfo>& devices,
                                    const juce::String& wantedId)
    {
        for (int i = 0; i < (int) devices.size(); ++i)
            if (devices[(size_t) i].identifier == wantedId)
                return i;
        return -1;
    };
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        if (track.midiInputIdentifier.isNotEmpty())
            track.midiInputIndex.store (
                resolveByIdentifier (midiIn.getDevices(), track.midiInputIdentifier),
                std::memory_order_relaxed);
        else if (track.midiInputIndex.load (std::memory_order_relaxed) >= (int) midiIn.getDevices().size())
            track.midiInputIndex.store (-1, std::memory_order_relaxed);

        if (track.midiOutputIdentifier.isNotEmpty())
            track.midiOutputIndex.store (
                resolveByIdentifier (midiOut.getDevices(), track.midiOutputIdentifier),
                std::memory_order_relaxed);
        else if (track.midiOutputIndex.load (std::memory_order_relaxed) >= (int) midiOut.getDevices().size())
            track.midiOutputIndex.store (-1, std::memory_order_relaxed);
    }

    // Re-resolve the session-level MIDI wiring the same way: the sync
    // source/output and the MCU control surface each store a saved identifier,
    // so a session load must remap them to current bank indices too - else the
    // engine's sync + MCU consumers read stale indices until the user reopens
    // Audio Settings. release-ordered to match their setters in AudioSettingsPanel.
    auto resolveSessionIdx = [&] (std::atomic<int>& idx,
                                   const std::vector<duskstudio::midi::MidiDeviceInfo>& devices,
                                   const juce::String& wantedId)
    {
        idx.store (wantedId.isNotEmpty() ? resolveByIdentifier (devices, wantedId) : -1,
                   std::memory_order_release);
    };
    resolveSessionIdx (session.syncSourceInputIdx,    midiIn.getDevices(),  session.syncSourceInputIdentifier);
    resolveSessionIdx (session.syncOutputIdx,         midiOut.getDevices(), session.syncOutputIdentifier);
    resolveSessionIdx (session.mcu.resolvedInputIdx,  midiIn.getDevices(),  session.mcu.inputIdentifier);
    resolveSessionIdx (session.mcu.resolvedOutputIdx, midiOut.getDevices(), session.mcu.outputIdentifier);

    // A session load may have replaced the tempo map - republish the audio
    // thread's snapshot so the MIDI scheduler + metronome see the loaded points.
    publishTempoMap();

    // NB: this function stays atomic-stores-only so it's safe to run without
    // detaching the audio callback. The actual output-port opening for sync +
    // MCU happens in openConfiguredMidiOutputs(), called right after this on
    // load (same place the per-track outputs open).
}

void AudioEngine::rebuildMidiBanks()
{
    // Safe to mutate ONLY with the input + audio callbacks detached. Ctor's
    // first call satisfies that (callbacks not yet registered); refreshMidiInputs
    // does remove-then-call-then-add. Callbacks active during mutation = UB.
    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    midiIn.rebuild (sr);
    midiOut.rebuild();

    // One drained-block buffer per input, reserved off the RT path so the
    // per-block drain + test-inject merge never allocate on the audio thread.
    perInputMidi.assign ((size_t) midiIn.getNumInputs(), dusk::MidiBuffer{});
    for (auto& m : perInputMidi) m.reserveBytes (4096);

    // Re-resolve the session's sync + MCU wiring against the fresh banks so a
    // hot-plug doesn't strand an index at a stale slot (STAYS in the engine -
    // the banks don't know session identifiers). Empty / no match = -1. release
    // pairs with the audio-thread acquires and the AudioSettingsPanel setters.
    auto resolve = [] (const std::vector<duskstudio::midi::MidiDeviceInfo>& devices,
                        const juce::String& wantedId)
    {
        if (wantedId.isEmpty()) return -1;
        for (int i = 0; i < (int) devices.size(); ++i)
            if (devices[(size_t) i].identifier == wantedId) return i;
        return -1;
    };
    session.syncSourceInputIdx   .store (resolve (midiIn.getDevices(),  session.syncSourceInputIdentifier), std::memory_order_release);
    session.mcu.resolvedInputIdx .store (resolve (midiIn.getDevices(),  session.mcu.inputIdentifier),       std::memory_order_release);
    session.syncOutputIdx        .store (resolve (midiOut.getDevices(), session.syncOutputIdentifier),      std::memory_order_release);
    session.mcu.resolvedOutputIdx.store (resolve (midiOut.getDevices(), session.mcu.outputIdentifier),      std::memory_order_release);

    // Eager-open the sync + MCU output ports so the first clock byte / MCU tick
    // doesn't wait on a synchronous ALSA snd_seq_connect. Per-track outputs open
    // lazily via openConfiguredMidiOutputs.
    if (const int i = session.syncOutputIdx.load (std::memory_order_acquire); i >= 0)
        midiOut.ensureOpen (i);
    if (const int i = session.mcu.resolvedOutputIdx.load (std::memory_order_acquire); i >= 0)
        midiOut.ensureOpen (i);

    midiSyncReceiver.reset();
    midiTimeCodeReceiver.reset();
    midiClockEmitter.reset();
}

void AudioEngine::openConfiguredMidiOutputs()
{
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const int idx = session.track (t).midiOutputIndex.load (std::memory_order_relaxed);
        if (idx >= 0)
            midiOut.ensureOpen (idx);
    }

    // Sync clock + MCU feedback ports the loaded session resolved (see
    // reresolveTrackMidiFromSession). Opened here so clock / MCU feedback flow
    // without the user reopening Audio Settings, alongside the per-track opens.
    if (const int i = session.syncOutputIdx.load (std::memory_order_acquire); i >= 0)
        midiOut.ensureOpen (i);
    if (const int i = session.mcu.resolvedOutputIdx.load (std::memory_order_acquire); i >= 0)
        midiOut.ensureOpen (i);
}

void AudioEngine::openConfiguredMidiOutputsSafely()
{
    // Session-load calls this with the audio callback ATTACHED, but ensureOpen
    // mutates the output bank the audio thread reads (in the clock / MCU send
    // paths). Detach the callback for the brief open pass - same contract
    // refreshMidiInputs() relies on. The startup caller runs pre-attach and
    // uses openConfiguredMidiOutputs() directly.
    deviceManager.removeAudioCallback (this);
    openConfiguredMidiOutputs();
    deviceManager.addAudioCallback (this);
}

int AudioEngine::getBackendXRunCount() const noexcept
{
    // const_cast: getCurrentAudioDevice is non-const for historical
    // reasons. Benign - getXRunCount is noexcept and returns a counter.
    if (auto* dev = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentAudioDevice())
        return juce::jmax (0, dev->getXRunCount()
                                  - backendXrunBaseline.load (std::memory_order_relaxed));
    return 0;
}

void AudioEngine::resetXRunCounts() noexcept
{
    xrunCount.store (0, std::memory_order_relaxed);
    int devCount = 0;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        devCount = dev->getXRunCount();
    backendXrunBaseline.store (devCount, std::memory_order_relaxed);
}

void AudioEngine::setStage (Stage s) noexcept
{
    const auto current = stage.load (std::memory_order_relaxed);
    if (current == s) return;

    // Recording/Mixing/Aux share live track-to-master flow - only UI
    // changes across those swaps. Mastering = wholly separate path
    // (stereo file -> MasteringChain -> output) so transport must be
    // force-stopped when crossing into or out of it.
    const bool crossesMastering = (current == Stage::Mastering)
                                 || (s == Stage::Mastering);
    if (crossesMastering)
    {
        if (transport.isRecording())
            recordManager.stopRecording (transport.getPlayhead());
        transport.setState (Transport::State::Stopped);
        masteringPlayer.stop();
        playbackEngine.stopPlayback();
    }

    // Gates two different signal flows on the audio thread - release/acquire
    // so the stop sequencing above is visible before the path switches.
    stage.store (s, std::memory_order_release);
}

AudioEngine::~AudioEngine()
{
    diagTimer.stopTimer();
    if (transport.isRecording())
        recordManager.stopRecording (transport.getPlayhead());
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (this);
    midiIn.detachCallback();
    deviceManager.closeAudioDevice();
    // After the callback is gone - nothing pushes to the out-queue, the
    // pump drains or drops what's left, then the output bank can destruct.
    midiOut.stopPump();
}

void AudioEngine::drainCallbackDiagnostics()
{
    const auto gated = earlyOutBlocks.load (std::memory_order_relaxed);
    if (gated != lastReportedGated)
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] callback GATED (processingSuspended=true) - "
                              "%lld silent block(s) since last report; if persistent, the gate is stuck\n",
                      (long long) (gated - lastReportedGated));
        lastReportedGated = gated;
    }

    const auto silent = silentBlocks.load (std::memory_order_relaxed);
    if (silent != lastReportedSilent)
    {
        const auto dims = silentDims.load (std::memory_order_relaxed);
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] callback SILENT: engine buffers sized %d but device "
                              "delivered %d samples - re-prepare missed the block size (%lld block(s))\n",
                      (int) (dims >> 32),
                      (int) (dims & 0xffffffffu),
                      (long long) (silent - lastReportedSilent));
        lastReportedSilent = silent;
    }
}

void AudioEngine::play()
{
    if (transport.isPlaying() || transport.isRecording()) return;

    // The audio callback already wraps playhead >= loopEnd back, but
    // has no symmetric guard for before-loop - linear playback would
    // audibly run through the pre-loop area. Snap at Play-press so
    // "loop engaged" actually means "play inside the loop".
    if (transport.isLoopEnabled())
    {
        const auto lStart = transport.getLoopStart();
        const auto lEnd   = transport.getLoopEnd();
        if (lEnd > lStart)
        {
            const auto ph = transport.getPlayhead();
            if (ph < lStart || ph >= lEnd)
                transport.setPlayhead (lStart);
        }
    }

    playbackEngine.preparePlayback();
    transport.setState (Transport::State::Playing);
}

void AudioEngine::stop()
{
    if (transport.isStopped()) return;

    const bool wasRecording = transport.isRecording();
    transport.setState (Transport::State::Stopped);

    if (wasRecording)
    {
        recordManager.stopRecording (transport.getPlayhead());
        activeRecordStart.store (std::numeric_limits<std::int64_t>::min(),
                                  std::memory_order_relaxed);

        // RecordManager already mutated state; action captures the
        // before/after snapshot. First perform() is a no-op (state
        // already applied); redo-after-undo re-applies after-state.
        const auto& diff = recordManager.getLastCommitDiff();
        if (! diff.empty())
        {
            std::vector<RecordCommitAction::TrackDiff> wrapped;
            wrapped.reserve (diff.size());
            for (const auto& d : diff)
                wrapped.push_back ({ d.trackIndex,
                                      d.audioBefore, d.audioAfter,
                                      d.midiBefore,  d.midiAfter });
            undoManager.beginNewTransaction ("Record");
            undoManager.perform (new RecordCommitAction (
                session, *this, std::move (wrapped)));
            recordManager.clearLastCommitDiff();
        }
    }

    playbackEngine.stopPlayback();

    // Honour the user's Settings choice for Stop behaviour.
    // 0 = PauseInPlace (leave playhead where it landed)
    // 1 = ReturnToZero (rewind to origin)
    // 2 = ReturnToLastClicked (jump to last ruler-click position; falls
    //     back to pause-in-place when nothing was clicked yet).
    // Callers that want unconditional stop+rewind (the '.' hotkey, Home
    // key) still call setPlayhead(0) explicitly after stop().
    const int behavior = session.stopBehavior.load (std::memory_order_relaxed);
    if (behavior == 1)
    {
        transport.setPlayhead (0);
    }
    else if (behavior == 2)
    {
        const auto last = session.lastClickedTimelineSample.load (std::memory_order_relaxed);
        if (last >= 0)
            transport.setPlayhead (last);
    }
}

void AudioEngine::record()
{
    if (transport.isRecording())
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): already recording, ignored.\n");
        return;
    }

    // Defensive resync - some paths (RegionEditActions clone/restore)
    // write recordArmed directly and leave the counter stale, making
    // anyTrackArmed() incorrectly false. Cheap (24-track scan).
    session.recomputeRtCounters();
    if (! session.anyTrackArmed())
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): no track is armed; "
                              "click ARM on the strip you want to record into.\n");
        if (onRecordBlocked_)
            onRecordBlocked_ ("No track is armed.\n\nClick ARM on the strip you want "
                              "to record into, then press Record again.");
        return;
    }

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0)
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): no audio device open "
                              "(sample rate is 0); recording cannot start.\n");
        if (onRecordBlocked_)
            onRecordBlocked_ ("No audio device is open.\n\nOpen Settings \xE2\x86\x92 Audio "
                              "and select a device before recording.");
        return;
    }

    // With punch on, the WAV's first audible sample is at punchIn so
    // the region's timelineStart must be punchIn. Audio thread skips
    // writes until playhead reaches punchIn so WAV time-zero lines up.
    // Inside / past the window = fall back to playhead (no truncation).
    std::int64_t startSample = transport.getPlayhead();
    if (transport.isPunchEnabled()
        && transport.getPunchOut() > transport.getPunchIn()
        && startSample < transport.getPunchIn())
    {
        startSample = transport.getPunchIn();
    }

    if (! recordManager.startRecording (sr, startSample))
    {
        std::fprintf (stderr, "[Dusk Studio/AudioEngine] record(): startRecording failed; "
                              "no armed track could be set up (e.g. all frozen, or the take "
                              "file could not be created).\n");
        if (onRecordBlocked_)
            onRecordBlocked_ ("Recording could not start.\n\nThe armed track(s) could not be "
                              "set up - a frozen track can't record (unfreeze it first), or "
                              "the take file could not be created.");
        return;
    }

    activeRecordStart.store (startSample, std::memory_order_relaxed);

    // STOP+FFWD jumps the user back to their last take start. Persisted.
    session.lastRecordPointSamples.store (startSample, std::memory_order_relaxed);

    // Roll playhead back one bar; audio between pre-roll start and
    // startSample is gated out of the recorder so WAV time-zero still
    // maps to startSample.
    if (session.countInEnabled.load (std::memory_order_relaxed))
    {
        const float bpm     = session.tempoBpm.load    (std::memory_order_relaxed);
        const int   beatsBar = session.beatsPerBar.load (std::memory_order_relaxed);
        if (bpm > 0.0f && beatsBar > 0)
        {
            const auto countInSamples =
                (std::int64_t) (sr * 60.0 / (double) bpm * (double) beatsBar);
            transport.setPlayhead (startSample - countInSamples);
        }
    }

    // Hear existing material BEFORE punch-in. Same gating as count-in.
    // Stacks with count-in (whichever rolls back further wins).
    if (transport.isPunchEnabled())
    {
        const float pre = session.preRollEnabled.load (std::memory_order_relaxed)
                            ? session.preRollSeconds.load (std::memory_order_relaxed)
                            : 0.0f;
        if (pre > 0.0f)
        {
            const auto preSamples = (std::int64_t) ((double) pre * sr);
            const auto candidate = juce::jmax ((std::int64_t) 0, startSample - preSamples);
            if (candidate < transport.getPlayhead())
                transport.setPlayhead (candidate);
        }
    }

    playbackEngine.preparePlayback();  // un-armed tracks still play through
    transport.setState (Transport::State::Recording);
}

void AudioEngine::jumpToPrevMarker()
{
    const auto& markers = session.getMarkers();
    if (markers.empty()) { transport.setPlayhead (0); return; }
    const auto cur = transport.getPlayhead();
    // Walk in reverse for the largest marker strictly before the playhead.
    // "Strictly before" so a press while sitting ON a marker steps to the
    // previous one rather than restating the current position.
    std::int64_t target = 0;
    bool found = false;
    for (auto it = markers.rbegin(); it != markers.rend(); ++it)
    {
        if (it->timelineSamples < cur) { target = it->timelineSamples; found = true; break; }
    }
    transport.setPlayhead (found ? target : 0);
}

void AudioEngine::jumpToNextMarker()
{
    const auto& markers = session.getMarkers();
    if (markers.empty()) return;   // no overshoot beyond known points
    const auto cur = transport.getPlayhead();
    for (const auto& m : markers)
    {
        if (m.timelineSamples > cur) { transport.setPlayhead (m.timelineSamples); return; }
    }
    // Past the last marker: stay where we are. Tascam-style "stops at end".
}

void AudioEngine::jumpToZero()
{
    transport.setPlayhead (0);
}

void AudioEngine::jumpToLastRecordPoint()
{
    transport.setPlayhead (session.lastRecordPointSamples.load (std::memory_order_relaxed));
}

void AudioEngine::publishPluginStateForSave (bool audioCallbackDetached)
{
    // Snapshot each track's PluginSlot into its session-model strings so
    // SessionSerializer (which only sees Session) can serialise plugin
    // state alongside everything else. Empty strings = no plugin loaded.
    //
    // Plugin state I/O briefly suspends each plugin (releaseResources /
    // prepareToPlay around getStateInformation) - JUCE's contract says
    // state I/O must not race the renderer, and plugins like u-he Diva
    // crash hard when it does. The audio thread is parked across each
    // suspension, so the engine's master output goes silent for a few
    // milliseconds per loaded plugin during the save.
    //
    // For the autosave timer (audioCallbackDetached=false) that runs
    // every 30 s during normal playback, that dropout is unacceptable.
    // We skip plugin state entirely on that path: the existing values
    // in Session::pluginStateBase64 (set by the most recent manual save
    // or session load) are preserved, so a crash recovery still gets
    // valid plugin state - just possibly stale relative to in-flight
    // knob tweaks. Manual save (Ctrl+S, File>Save, quit-save) is the
    // path that captures fresh plugin state.
    if (! audioCallbackDetached)
    {
        // Plugin description/state strings on Session are kept as-is.
        // Future enhancement: a "Save (with plugin state)" UI option
        // that explicitly opts in to the audio dropout.
        return;
    }

    const int parkSleepMs = 0;  // audio thread already gone; no need to wait

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        auto& strip = strips[(size_t) t];
        auto& slot  = strip.getPluginSlot();
        track.pluginDescriptionXml = slot.getDescriptionXmlForSave (parkSleepMs);
        track.pluginStateBase64    = slot.getStateBase64ForSave   (parkSleepMs);

        // Native CLAP insert (parallel to the JUCE plugin; at most one is loaded).
        // Linux-only; on other platforms the saved path/state are preserved untouched
        // so a session authored on Linux round-trips through a non-Linux build.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        if (strip.isNativeClapLoaded())
        {
            track.nativeClapPath     = strip.getNativeClapSlot().getPath();
            track.nativeClapPluginId = strip.getNativeClapSlot().getPluginId();
            std::vector<uint8_t> blob;
            if (strip.getNativeClapSlot().saveState (blob) && ! blob.empty())
                track.nativeClapStateBase64 = juce::Base64::toBase64 (blob.data(), blob.size());
            else
                track.nativeClapStateBase64.clear();
        }
        else if (! strip.nativeClapReloadFailed())
        {
            // Slot genuinely empty (user removed it, or never had one) - drop the
            // persisted refs. When a restore FAILED (e.g. the .clap was missing this
            // launch) the slot is also empty, but we keep the path/state so the
            // reference survives to reconnect on a future launch - mirrors how an
            // offline JUCE plugin preserves its descXml.
            track.nativeClapPath.clear();
            track.nativeClapPluginId.clear();
            track.nativeClapStateBase64.clear();
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
        if (strip.isNativeLv2Loaded())
        {
            track.nativeLv2Path     = strip.getNativeLv2Slot().getPath();
            track.nativeLv2PluginId = strip.getNativeLv2Slot().getPluginId();
            std::vector<uint8_t> blob;
            strip.getNativeLv2Slot().setStateDirectory (
                lv2StateDirFor (session, "track" + juce::String (t + 1).paddedLeft ('0', 2)));
            // Preserve the carried blob when a plugin can't serialize (no state
            // extension / save failure) - don't wipe it on a save round-trip.
            if (strip.getNativeLv2Slot().saveState (blob) && ! blob.empty())
                track.nativeLv2StateBase64 = juce::Base64::toBase64 (blob.data(), blob.size());
        }
        else if (! strip.nativeLv2ReloadFailed())
        {
            track.nativeLv2Path.clear();
            track.nativeLv2PluginId.clear();
            track.nativeLv2StateBase64.clear();
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
        if (strip.isNativeVst3Loaded())
        {
            track.nativeVst3Path     = strip.getNativeVst3Slot().getPath();
            track.nativeVst3PluginId = strip.getNativeVst3Slot().getPluginId();
            std::vector<uint8_t> blob;
            // See the LV2 block above: preserve the carried blob when the plugin
            // can't serialize.
            if (strip.getNativeVst3Slot().saveState (blob) && ! blob.empty())
                track.nativeVst3StateBase64 = juce::Base64::toBase64 (blob.data(), blob.size());
        }
        else if (! strip.nativeVst3ReloadFailed())
        {
            track.nativeVst3Path.clear();
            track.nativeVst3PluginId.clear();
            track.nativeVst3StateBase64.clear();
        }
#endif
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = session.auxLane (a);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            auto& strip = auxLaneStrips[(size_t) a];
            auto& slot  = strip.getPluginSlot (s);
            lane.pluginDescriptionXml[(size_t) s] = slot.getDescriptionXmlForSave (parkSleepMs);
            lane.pluginStateBase64[(size_t) s]    = slot.getStateBase64ForSave   (parkSleepMs);

            // Native CLAP slot (parallel to the JUCE plugin; at most one is loaded).
            // Linux-only; preserved untouched elsewhere (see the track block above).
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            if (strip.isNativeClapLoaded (s))
            {
                lane.nativeClapPath[(size_t) s]     = strip.getNativeClapSlot (s).getPath();
                lane.nativeClapPluginId[(size_t) s] = strip.getNativeClapSlot (s).getPluginId();
                std::vector<uint8_t> blob;
                if (strip.getNativeClapSlot (s).saveState (blob) && ! blob.empty())
                    lane.nativeClapStateBase64[(size_t) s] = juce::Base64::toBase64 (blob.data(), blob.size());
                else
                    lane.nativeClapStateBase64[(size_t) s].clear();
            }
            else if (! strip.nativeClapReloadFailed (s))
            {
                // See the track block above: keep the persisted refs when a restore
                // failed (path preserved to reconnect later); clear only a genuinely
                // empty / user-removed slot.
                lane.nativeClapPath[(size_t) s].clear();
                lane.nativeClapPluginId[(size_t) s].clear();
                lane.nativeClapStateBase64[(size_t) s].clear();
            }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            if (strip.isNativeLv2Loaded (s))
            {
                lane.nativeLv2Path[(size_t) s]     = strip.getNativeLv2Slot (s).getPath();
                lane.nativeLv2PluginId[(size_t) s] = strip.getNativeLv2Slot (s).getPluginId();
                std::vector<uint8_t> blob;
                strip.getNativeLv2Slot (s).setStateDirectory (
                    lv2StateDirFor (session, "aux" + juce::String (a + 1)
                                                 + "_slot" + juce::String (s + 1)));
                // See the track block above: preserve the carried blob when the
                // plugin can't serialize.
                if (strip.getNativeLv2Slot (s).saveState (blob) && ! blob.empty())
                    lane.nativeLv2StateBase64[(size_t) s] = juce::Base64::toBase64 (blob.data(), blob.size());
            }
            else if (! strip.nativeLv2ReloadFailed (s))
            {
                lane.nativeLv2Path[(size_t) s].clear();
                lane.nativeLv2PluginId[(size_t) s].clear();
                lane.nativeLv2StateBase64[(size_t) s].clear();
            }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            if (strip.isNativeVst3Loaded (s))
            {
                lane.nativeVst3Path[(size_t) s]     = strip.getNativeVst3Slot (s).getPath();
                lane.nativeVst3PluginId[(size_t) s] = strip.getNativeVst3Slot (s).getPluginId();
                std::vector<uint8_t> blob;
                if (strip.getNativeVst3Slot (s).saveState (blob) && ! blob.empty())
                    lane.nativeVst3StateBase64[(size_t) s] = juce::Base64::toBase64 (blob.data(), blob.size());
            }
            else if (! strip.nativeVst3ReloadFailed (s))
            {
                lane.nativeVst3Path[(size_t) s].clear();
                lane.nativeVst3PluginId[(size_t) s].clear();
                lane.nativeVst3StateBase64[(size_t) s].clear();
            }
#endif
        }
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Master tape (TapeMachine) state. Mirrors the per-slot pattern: serialise
    // getStateInformation() into a base64 string on the session model so the
    // serializer (which only sees Session) can persist it.
    {
        juce::MemoryBlock mb;
        master.getTapeProcessor().getStateInformation (mb);
        session.master().tapeStateBase64 = mb.toBase64Encoding();
    }
#endif
}

void AudioEngine::releaseAllPluginResources()
{
    // Walk every plugin slot and call releaseResources on its loaded
    // instance. PluginSlot::releaseResources also clears currentInstance,
    // so any audio callback that does happen to fire after this (despite
    // the caller's contract) will see null and bypass.
    for (auto& strip : strips)
        strip.getPluginSlot().releaseResources();
    for (auto& laneStrip : auxLaneStrips)
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
            laneStrip.getPluginSlot (s).releaseResources();
}

void AudioEngine::leakAllPluginInstancesForShutdown()
{
    for (auto& strip : strips)
    {
        strip.getPluginSlot().leakInstanceForShutdown();
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        strip.getNativeClapSlot().leakForShutdown();   // u-he hangs in destroy/dlclose
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
        strip.getNativeLv2Slot().leakForShutdown();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
        strip.getNativeVst3Slot().leakForShutdown();
#endif
    }
    for (auto& laneStrip : auxLaneStrips)
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            laneStrip.getPluginSlot (s).leakInstanceForShutdown();
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            laneStrip.getNativeClapSlot (s).leakForShutdown();   // u-he hangs in destroy/dlclose
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            laneStrip.getNativeLv2Slot (s).leakForShutdown();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            laneStrip.getNativeVst3Slot (s).leakForShutdown();
#endif
        }
}

void AudioEngine::publishTransportStateForSave()
{
    session.savedLoopStart    = transport.getLoopStart();
    session.savedLoopEnd      = transport.getLoopEnd();
    session.savedLoopEnabled  = transport.isLoopEnabled();
    session.savedPunchIn      = transport.getPunchIn();
    session.savedPunchOut     = transport.getPunchOut();
    session.savedPunchEnabled = transport.isPunchEnabled();
}

void AudioEngine::consumeTransportStateAfterLoad()
{
    transport.setLoopRange (session.savedLoopStart, session.savedLoopEnd);
    transport.setLoopEnabled (session.savedLoopEnabled);
    transport.setPunchRange (session.savedPunchIn, session.savedPunchOut);
    transport.setPunchEnabled (session.savedPunchEnabled);
}

void AudioEngine::consumePluginStateAfterLoad()
{
    // Mirror of publish: read the (just-deserialised) strings on each track
    // and re-instantiate the plugin in the live PluginSlot. Failures log
    // and continue rather than aborting the whole session load - a missing
    // plugin shouldn't lose the rest of the user's saved state. Failures
    // are captured in lastPluginLoadFailures so the UI can surface a
    // single summary AlertWindow listing every slot that fell back to
    // empty - silent fallback is the worst case (user thinks the saved
    // mix is intact but the plugin chain it depended on is gone).
    lastPluginLoadFailures.clear();

    auto parsePluginName = [] (const juce::String& descXml) -> juce::String
    {
        if (descXml.isEmpty()) return {};
        if (auto xml = juce::XmlDocument::parse (descXml))
        {
            juce::PluginDescription desc;
            if (desc.loadFromXml (*xml)) return desc.name;
        }
        return juce::String ("(unknown)");
    };

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        auto& strip = strips[(size_t) t];
        auto& slot  = strip.getPluginSlot();

        // Native CLAP insert (takes precedence over the JUCE plugin). load() needs the
        // sample rate: load directly when prepared (device running), else stash a
        // pending restore that ChannelStrip::prepare() consummates. Linux-only; on
        // other platforms the saved path is ignored (the strip restores empty) but
        // kept in the session model so a re-save round-trips it back to Linux.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
        if (track.nativeClapPath.isNotEmpty())
        {
            slot.unload();
            strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);

            auto blob = decodeBase64Blob (track.nativeClapStateBase64);

            const juce::File clapFile (track.nativeClapPath);
            if (strip.isPrepared())
            {
                suspendProcessing();
                std::string err;
                const bool ok = strip.loadNativeClap (clapFile, err, track.nativeClapPluginId);
                if (ok && ! blob.empty())
                    strip.getNativeClapSlot().loadState (blob);
                resumeProcessing();
                if (! ok)
                {
                    // A failed RESTORE is not a user removal - mark it so the save
                    // path keeps the persisted refs (loadNativeClap cleared the flag).
                    strip.markNativeClapRestoreFailed();
                    lastPluginLoadFailures.push_back ({
                        "Track " + juce::String (t + 1),
                        clapFile.getFileNameWithoutExtension() });
                }
            }
            else
            {
                // Not prepared -> no live audio; safe to clear carried-over native
                // hosts unfenced (the prepared path's loadNativeClap evicts them).
                strip.unloadNativeLv2();
                strip.unloadNativeVst3();
                strip.setPendingNativeClap (clapFile, std::move (blob), track.nativeClapPluginId);
            }
            continue;
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
        if (track.nativeLv2Path.isNotEmpty())
        {
            slot.unload();
            strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);

            auto blob = decodeBase64Blob (track.nativeLv2StateBase64);

            const juce::File lv2File (track.nativeLv2Path);
            if (strip.isPrepared())
            {
                suspendProcessing();
                std::string err;
                const bool ok = strip.loadNativeLv2 (lv2File, err, track.nativeLv2PluginId);
                if (ok && ! blob.empty())
                {
                    strip.getNativeLv2Slot().setStateDirectory (
                        lv2StateDirFor (session, "track" + juce::String (t + 1).paddedLeft ('0', 2)));
                    strip.getNativeLv2Slot().loadState (blob);
                }
                resumeProcessing();
                if (! ok)
                {
                    strip.markNativeLv2RestoreFailed();   // keep refs - see the CLAP twin
                    lastPluginLoadFailures.push_back ({
                        "Track " + juce::String (t + 1),
                        lv2File.getFileNameWithoutExtension() });
                }
            }
            else
            {
                strip.unloadNativeClap();   // see the CLAP pending branch above
                strip.unloadNativeVst3();
                strip.setPendingNativeLv2 (lv2File, std::move (blob), track.nativeLv2PluginId,
                                           lv2StateDirFor (session,
                                               "track" + juce::String (t + 1).paddedLeft ('0', 2)));
            }
            continue;
        }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
        if (track.nativeVst3Path.isNotEmpty())
        {
            slot.unload();
            strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);

            auto blob = decodeBase64Blob (track.nativeVst3StateBase64);

            const juce::File vst3File (track.nativeVst3Path);
            if (strip.isPrepared())
            {
                suspendProcessing();
                std::string err;
                const bool ok = strip.loadNativeVst3 (vst3File, err, track.nativeVst3PluginId);
                if (ok && ! blob.empty())
                    strip.getNativeVst3Slot().loadState (blob);
                resumeProcessing();
                if (! ok)
                {
                    strip.markNativeVst3RestoreFailed();   // keep refs - see the CLAP twin
                    lastPluginLoadFailures.push_back ({
                        "Track " + juce::String (t + 1),
                        vst3File.getFileNameWithoutExtension() });
                }
            }
            else
            {
                strip.unloadNativeClap();   // see the CLAP pending branch above
                strip.unloadNativeLv2();
                strip.setPendingNativeVst3 (vst3File, std::move (blob), track.nativeVst3PluginId);
            }
            continue;
        }
#endif

        // No native host in this session for this strip - tear down any native
        // instance carried over from the previously-loaded session before the JUCE
        // restore below (unload destroys the instance, so fence it when live).
        if (strip.isNativeClapLoaded() || strip.isNativeLv2Loaded() || strip.isNativeVst3Loaded())
        {
            suspendProcessing();
            strip.unloadNativeClap();
            strip.unloadNativeLv2();
            strip.unloadNativeVst3();
            resumeProcessing();
        }
        else
        {
            strip.unloadNativeClap();   // no live instance: just clears any stale pending
            strip.unloadNativeLv2();
            strip.unloadNativeVst3();
        }

        if (track.pluginDescriptionXml.isEmpty())
        {
            slot.unload();   // session has no plugin here - clear any carried over from the prior one
            // Reconstruct the insert mode from session state so a strip that ran
            // a plugin in the previous session doesn't stay in kInsertPlugin and
            // route around an enabled hardware insert (mirrors the aux path below).
            strip.insertMode.store (
                track.hardwareInsert.enabled.load (std::memory_order_relaxed)
                    ? ChannelStrip::kInsertHardware : ChannelStrip::kInsertEmpty,
                std::memory_order_release);
            continue;
        }
        // Plugin intended on this track - pin the mode to Plugin (offline if the
        // restore below fails) so a prior session's kInsertHardware can't linger.
        strip.insertMode.store (ChannelStrip::kInsertPlugin, std::memory_order_release);
        juce::String error;
        if (! slot.restoreFromSavedState (track.pluginDescriptionXml,
                                            track.pluginStateBase64, error))
        {
            DBG ("AudioEngine: failed to restore plugin on track " << (t + 1)
                  << ": " << error);
            lastPluginLoadFailures.push_back ({
                "Track " + juce::String (t + 1),
                parsePluginName (track.pluginDescriptionXml) });
        }
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = session.auxLane (a);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            auto& strip = auxLaneStrips[(size_t) a];
            auto& slot  = strip.getPluginSlot (s);

            // Native CLAP slot (takes precedence over the JUCE plugin). load() needs
            // the sample rate: load directly when the engine is already prepared (a
            // session opened while the device runs), else stash a pending restore that
            // AuxLaneStrip::prepare() consummates when the SR is known. Linux-only;
            // elsewhere the saved path is ignored but preserved in the session model.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
            if (lane.nativeClapPath[(size_t) s].isNotEmpty())
            {
                slot.unload();   // ensure no JUCE plugin lingers in this slot
                strip.insertMode[(size_t) s].store (AuxLaneStrip::kInsertPlugin, std::memory_order_release);

                auto blob = decodeBase64Blob (lane.nativeClapStateBase64[(size_t) s]);

                const juce::File clapFile (lane.nativeClapPath[(size_t) s]);
                if (strip.isPrepared())
                {
                    suspendProcessing();   // load is not RT-safe; fence the audio thread
                    std::string err;
                    const bool ok = strip.loadNativeClap (s, clapFile, err,
                                                          lane.nativeClapPluginId[(size_t) s]);
                    if (ok && ! blob.empty())
                        strip.getNativeClapSlot (s).loadState (blob);
                    resumeProcessing();
                    if (! ok)
                    {
                        strip.markNativeClapRestoreFailed (s);   // keep refs - restore, not removal
                        lastPluginLoadFailures.push_back ({
                            "Aux " + juce::String (a + 1) + " slot " + juce::String (s + 1),
                            clapFile.getFileNameWithoutExtension() });
                    }
                }
                else
                {
                    // Not prepared -> no live audio; clear carried-over native hosts unfenced.
                    strip.unloadNativeLv2 (s);
                    strip.unloadNativeVst3 (s);
                    strip.setPendingNativeClap (s, clapFile, std::move (blob),
                                                lane.nativeClapPluginId[(size_t) s]);
                }
                continue;   // native handled - skip the JUCE restore for this slot
            }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
            if (lane.nativeLv2Path[(size_t) s].isNotEmpty())
            {
                slot.unload();
                strip.insertMode[(size_t) s].store (AuxLaneStrip::kInsertPlugin, std::memory_order_release);

                auto blob = decodeBase64Blob (lane.nativeLv2StateBase64[(size_t) s]);

                const juce::File lv2File (lane.nativeLv2Path[(size_t) s]);
                if (strip.isPrepared())
                {
                    suspendProcessing();   // load is not RT-safe; fence the audio thread
                    std::string err;
                    const bool ok = strip.loadNativeLv2 (s, lv2File, err,
                                                         lane.nativeLv2PluginId[(size_t) s]);
                    if (ok && ! blob.empty())
                    {
                        strip.getNativeLv2Slot (s).setStateDirectory (
                            lv2StateDirFor (session, "aux" + juce::String (a + 1)
                                                         + "_slot" + juce::String (s + 1)));
                        strip.getNativeLv2Slot (s).loadState (blob);
                    }
                    resumeProcessing();
                    if (! ok)
                    {
                        strip.markNativeLv2RestoreFailed (s);   // keep refs - restore, not removal
                        lastPluginLoadFailures.push_back ({
                            "Aux " + juce::String (a + 1) + " slot " + juce::String (s + 1),
                            lv2File.getFileNameWithoutExtension() });
                    }
                }
                else
                {
                    strip.unloadNativeClap (s);   // see the CLAP pending branch above
                    strip.unloadNativeVst3 (s);
                    strip.setPendingNativeLv2 (s, lv2File, std::move (blob),
                                               lane.nativeLv2PluginId[(size_t) s],
                                               lv2StateDirFor (session,
                                                   "aux" + juce::String (a + 1)
                                                       + "_slot" + juce::String (s + 1)));
                }
                continue;   // native handled - skip the JUCE restore for this slot
            }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
            if (lane.nativeVst3Path[(size_t) s].isNotEmpty())
            {
                slot.unload();
                strip.insertMode[(size_t) s].store (AuxLaneStrip::kInsertPlugin, std::memory_order_release);

                auto blob = decodeBase64Blob (lane.nativeVst3StateBase64[(size_t) s]);

                const juce::File vst3File (lane.nativeVst3Path[(size_t) s]);
                if (strip.isPrepared())
                {
                    suspendProcessing();   // load is not RT-safe; fence the audio thread
                    std::string err;
                    const bool ok = strip.loadNativeVst3 (s, vst3File, err,
                                                          lane.nativeVst3PluginId[(size_t) s]);
                    if (ok && ! blob.empty())
                        strip.getNativeVst3Slot (s).loadState (blob);
                    resumeProcessing();
                    if (! ok)
                    {
                        strip.markNativeVst3RestoreFailed (s);   // keep refs - restore, not removal
                        lastPluginLoadFailures.push_back ({
                            "Aux " + juce::String (a + 1) + " slot " + juce::String (s + 1),
                            vst3File.getFileNameWithoutExtension() });
                    }
                }
                else
                {
                    strip.unloadNativeClap (s);   // see the CLAP pending branch above
                    strip.unloadNativeLv2 (s);
                    strip.setPendingNativeVst3 (s, vst3File, std::move (blob),
                                                lane.nativeVst3PluginId[(size_t) s]);
                }
                continue;   // native handled - skip the JUCE restore for this slot
            }
#endif

            // No native host for this slot - tear down any instance carried over from
            // the previous session before the JUCE restore (fence when live).
            if (strip.isNativeClapLoaded (s) || strip.isNativeLv2Loaded (s) || strip.isNativeVst3Loaded (s))
            {
                suspendProcessing();
                strip.unloadNativeClap (s);
                strip.unloadNativeLv2 (s);
                strip.unloadNativeVst3 (s);
                resumeProcessing();
            }
            else
            {
                strip.unloadNativeClap (s);   // clears any stale pending restore
                strip.unloadNativeLv2 (s);
                strip.unloadNativeVst3 (s);
            }

            const auto& descXml = lane.pluginDescriptionXml[(size_t) s];
            if (descXml.isEmpty())
            {
                // No plugin in this slot - unload any instance left from the
                // prior session and route around it (or through the hardware
                // insert when this slot is in HW mode).
                slot.unload();
                const bool hw = lane.hardwareInserts[(size_t) s].enabled.load (std::memory_order_relaxed);
                auxLaneStrips[(size_t) a].insertMode[(size_t) s].store (
                    hw ? AuxLaneStrip::kInsertHardware : AuxLaneStrip::kInsertEmpty,
                    std::memory_order_release);
                continue;
            }
            // Plugin intended on this aux slot - pin the mode to Plugin BEFORE the
            // restore (offline if it fails) so a prior session's mode can't linger.
            // The crossfade gate defaults to kInsertEmpty after prepare() (routes
            // AROUND the plugin); without this a reload leaves a restored plugin
            // live but starved (meters dark, AUX output is the raw send sum).
            // Mirrors the channel-strip path above.
            auxLaneStrips[(size_t) a].insertMode[(size_t) s].store (
                AuxLaneStrip::kInsertPlugin, std::memory_order_release);
            juce::String error;
            if (! slot.restoreFromSavedState (descXml,
                                                lane.pluginStateBase64[(size_t) s], error))
            {
                DBG ("AudioEngine: failed to restore plugin on aux lane " << (a + 1)
                      << " slot " << (s + 1) << ": " << error);
                lastPluginLoadFailures.push_back ({
                    "Aux " + juce::String (a + 1) + " / slot " + juce::String (s + 1),
                    parsePluginName (descXml) });
            }
        }
    }

#if DUSKSTUDIO_HAS_DUSK_DSP
    // Master tape state: push the deserialised base64 blob back into the
    // hosted TapeMachineAudioProcessor. fromBase64Encoding fails-soft (no
    // exception); empty / malformed data leaves the processor at its
    // donor defaults rather than blowing up the load.
    {
        const auto& s64 = session.master().tapeStateBase64;
        if (s64.isNotEmpty())
        {
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (s64) && mb.getSize() > 0)
                master.getTapeProcessor().setStateInformation (mb.getData(),
                                                                  (int) mb.getSize());
        }
    }
#endif
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Mark that a live device is present so the next
    // changeListenerCallback observing a null current device can
    // distinguish hot-unplug from steady-state "device was never up".
    // Release ordering so the message-thread reader in
    // changeListenerCallback sees the bump before it inspects
    // getCurrentAudioDevice().
    hadLiveDevice_.store (true, std::memory_order_release);

    // Baseline the readout to the device's CURRENT xrun count, not 0: a reused
    // device object can carry the previous session's count into aboutToStart
    // (it resets its own counter slightly later, in start()), and other backends
    // give no such guarantee. Subtracting the live count makes getBackendXRunCount
    // report only xruns accrued from here on.
    backendXrunBaseline.store (device->getXRunCount(), std::memory_order_relaxed);

    // Detect the silent-failure mode where a per-device ALSA name resolves
    // to 0 active output channels (PipeWire's ALSA shim does this for the
    // surround/front entries - JUCE accepts the asymmetric setup, the
    // engine writes the master mix to a non-existent destination, and the
    // user gets silence with no error). Loud stderr line + a flag the UI
    // surfaces so the failure is visible instead of mysterious.
    const int activeIn  = device->getActiveInputChannels().countNumberOfSetBits();
    const int activeOut = device->getActiveOutputChannels().countNumberOfSetBits();
    if (activeOut <= 0)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/AudioEngine] WARNING: device \"%s\" (type %s) opened with "
                      "0 output channels (in=%d). Engine output will be silent. Pick a "
                      "different device - \"Default ALSA Output\" or another backend - or "
                      "stop PipeWire if you want raw ALSA on this interface.\n",
                      device->getName().toRawUTF8(),
                      deviceManager.getCurrentAudioDeviceType().toRawUTF8(),
                      activeIn);
        usableOutputs.store (false, std::memory_order_relaxed);
    }
    else
    {
        usableOutputs.store (true, std::memory_order_relaxed);
    }

    // Reset every MIDI collector with the current sample rate so it can
    // convert the MIDI thread's millisecond timestamps into per-block sample
    // positions. Without this, the first drain would emit garbage timestamps.
    // Safe to call here - the audio callback hasn't fired yet for this open.
    midiIn.resetCollectors (device->getCurrentSampleRate());

    prepareForSelfTest (device->getCurrentSampleRate(),
                         device->getCurrentBufferSizeSamples());
}

void AudioEngine::stageTestMidiInjection (int inputIdx, juce::MidiBuffer events)
{
    // SPSC: wait for any previously staged buffer to be consumed before we
    // touch testInjectMidi. The synchronous self-test caller drives the
    // audio callback after every stage, so this normally observes false on
    // the first load - the bounded spin is just defensive against future
    // callers that forget to pump the callback between stages. Bound it
    // so a misuse can't hang the message thread; on timeout we drop the
    // pending buffer (the test will surface the dropped injection as a
    // failure rather than freezing the UI).
    constexpr int kMaxYieldIterations = 5000;
    for (int n = 0; n < kMaxYieldIterations
                     && testInjectReady.load (std::memory_order_acquire); ++n)
        std::this_thread::yield();
    // If the audio thread still holds the slot after the bounded spin,
    // bail without swapping - touching testInjectMidi while the audio
    // thread is mid-read races on the MidiBuffer state. The dropped
    // injection surfaces as a test failure; that's strictly better than
    // a release-build data race.
    if (testInjectReady.load (std::memory_order_acquire))
    {
        jassertfalse;
        return;
    }

    testInjectMidi.swapWith (events);
    testInjectInputIdx.store (inputIdx, std::memory_order_relaxed);
    testInjectReady.store (true, std::memory_order_release);
}

void AudioEngine::prepareForSelfTest (double sr, int bs)
{
    // Must run on the message thread. The function mutates shared DSP
    // state (every strip's prepare, every smoother reset, every scratch
    // buffer resize) without locks. If a future caller invokes this
    // from another thread while the audio callback is live, the audio
    // thread reads half-prepared state and corrupts buffers silently.
    // The existing call paths (audioDeviceAboutToStart, BounceEngine,
    // DuskStudioApp startup, AudioPipelineSelfTest) all run here; assert
    // catches a future caller that gets this wrong.
    jassert (juce::MessageManager::existsAndIsCurrentThread());

    // Gate the audio callback out for the whole re-prepare, then drain worker
    // lanes. The gate makes the no-concurrent-callback invariant ENFORCED
    // rather than assumed: any callback that still fires (a backend whose stop
    // didn't join, a force-killed I/O thread's sibling, a path nobody
    // enumerated) emits one silent buffer instead of racing the resizes and
    // plugin prepareToPlay calls below. Quiesce then covers lanes orphaned by
    // a force-killed dispatcher (they don't go through the gate).
    suspendProcessing();
    workerPool.quiesce();
    struct ResumeGuard
    {
        AudioEngine& e;
        ~ResumeGuard() { e.resumeProcessing(); }
    };
    const ResumeGuard resumeGuard { *this };

    // A re-prepare invalidates an armed realtime bounce: its capture
    // scratches were sized for the old block, so a grown one would overrun
    // them from the audio thread (the mixL guard tracks the NEW size and
    // passes). The gate above guarantees no callback is mid-sink-loop; kill
    // the arming here and let the bounce worker fail the render cleanly.
    if (rtBounceSinkCount.load (std::memory_order_relaxed) > 0)
    {
        rtBounceSinkCount.store (0, std::memory_order_relaxed);
        rtBounceAborted.store (true, std::memory_order_release);
        for (auto& s : strips) s.setStemCapture (nullptr, nullptr);
        for (int a = 0; a < Session::kNumBuses; ++a)    setBusStemCapture (a, nullptr, nullptr);
        for (int a = 0; a < Session::kNumAuxLanes; ++a) setAuxStemCapture (a, nullptr, nullptr);
    }

    currentSampleRate.store (sr, std::memory_order_relaxed);
    currentBlockSize.store  (bs, std::memory_order_relaxed);

    // Cache the tick->seconds reciprocal once - the xrun watchdog hits this
    // twice per callback otherwise, and highResolutionTicksToSeconds does a
    // 64-bit divide internally.
    {
        const auto tps = juce::Time::getHighResolutionTicksPerSecond();
        secondsPerTick = (tps > 0) ? 1.0 / (double) tps : 0.0;
    }

    // Read the global oversampling factor once per prepare and propagate it to
    // every strip/bus/master that wraps EQ+Comp in a Dusk Studio-side
    // oversampler (each rebuilds its juce::dsp::Oversampling at this factor in
    // prepare). Aux lanes host plugin chains only (no Dusk EQ/Comp), so they
    // don't take the factor. The cost only materialises when a strip is
    // actually processing audio - silent / skipped strips dodge the chain.
    // An offline bounce overrides the factor (e.g. 4×) so the printed mix is
    // alias-free even though realtime monitoring runs at the user's lighter
    // setting; the override is cleared before the engine re-prepares for live.
    const int oxOverride = renderOversamplingOverride.load (std::memory_order_relaxed);
    const int oxFactor   = (oxOverride > 0)
                             ? oxOverride
                             : session.oversamplingFactor.load (std::memory_order_relaxed);

    for (auto& s : strips)        s.prepare (sr, bs, oxFactor);
    for (auto& a : busStrips)     a.prepare (sr, bs, oxFactor);
    for (auto& a : auxLaneStrips) a.prepare (sr, bs);
    master.prepare (sr, bs, oxFactor);

    auxSilentRunSamples.fill (0);

    // Strips just re-prepared their plugins (re-caching latency) and rebuilt
    // their PDC delay lines - recompute the compensation against the fresh
    // latencies.
    recomputePdc();

    // Sync receiver uses the same sample-clock the engine derives its
    // per-block playhead from. Reset on every prepare so a sample-rate
    // change drops stale BPM history.
    midiSyncReceiver.prepare (sr);
    midiTimeCodeReceiver.prepare (sr);
    midiClockEmitter.prepare (sr);
    midiTimeCodeEmitter.prepare (sr);
#if DUSKSTUDIO_HAS_DUSK_DSP
    // TapeMachine animates its reels + level-integration timing from
    // getPlayHead()->getPosition(). Without a playhead the donor reads
    // null and the reels stay still even while audio passes through.
    master.getTapeProcessor().setPlayHead (playHead.get());
#endif

    // Push the playhead onto every per-channel plugin slot so hosted
    // synths/effects see the session's BPM, transport state, and sample
    // position. Without this, tempo-synced LFOs / arps / delays in
    // plugins like Diva default to 120 BPM regardless of session tempo.
    for (auto& s : strips)
        s.getPluginSlot().setHostPlayHead (playHead.get());
    for (auto& a : auxLaneStrips)
        for (int p = 0; p < AuxLaneParams::kMaxLanePlugins; ++p)
            a.getPluginSlot (p).setHostPlayHead (playHead.get());
    masteringChain.prepare (sr, bs, oxFactor);
    masteringPlayer.prepare (bs, sr);

    {
        const juce::dsp::ProcessSpec monoSpec { sr, (std::uint32_t) bs, 1 };
        auto prepPdc = [&monoSpec] (MasterPdcDelay& d)
        {
            d.prepare (monoSpec);
            d.setMaximumDelayInSamples (ChannelStrip::kMaxPdcSamples);
            d.reset();
        };
        prepPdc (masterDryPdcL);
        prepPdc (masterDryPdcR);
        for (int a = 0; a < Session::kNumAuxLanes; ++a)
        {
            prepPdc (auxReturnPdcL[(size_t) a]);
            prepPdc (auxReturnPdcR[(size_t) a]);
        }
        masterDryPdcApplied = 0;
        auxReturnPdcApplied.fill (0);
        masterDryPdcL.setDelay (0.0f);
        masterDryPdcR.setDelay (0.0f);
        for (int a = 0; a < Session::kNumAuxLanes; ++a)
        {
            auxReturnPdcL[(size_t) a].setDelay (0.0f);
            auxReturnPdcR[(size_t) a].setDelay (0.0f);
        }
    }
    metronome.prepare (sr);
    playbackEngine.prepare (bs);  // size the playback read scratch - audio thread mustn't allocate
    pitchDetector.prepare (sr);   // ~46 ms history at the device rate

    mixL.assign ((size_t) bs, 0.0f);
    mixR.assign ((size_t) bs, 0.0f);
    for (auto& v : busL)      v.assign ((size_t) bs, 0.0f);
    for (auto& v : busR)      v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxLaneL)  v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxLaneR)  v.assign ((size_t) bs, 0.0f);
    for (auto& v : playbackScratch)  v.assign ((size_t) bs, 0.0f);
    for (auto& v : playbackScratchR) v.assign ((size_t) bs, 0.0f);
    for (auto& m : perTrackMidiScratch) m.ensureSize (4096);
    midiClockOutScratch.reserveBytes (4096);
    midiOutTrackScratch.reserveBytes (4096);
    silentInputScratch.assign ((size_t) bs, 0.0f);

    for (auto& a : laneAccum)
    {
        a.mixL.assign ((size_t) bs, 0.0f);
        a.mixR.assign ((size_t) bs, 0.0f);
        for (auto& v : a.busL) v.assign ((size_t) bs, 0.0f);
        for (auto& v : a.busR) v.assign ((size_t) bs, 0.0f);
        for (auto& v : a.auxL) v.assign ((size_t) bs, 0.0f);
        for (auto& v : a.auxR) v.assign ((size_t) bs, 0.0f);
    }

    // Parallel strip DSP. Start the worker pool ONCE, here on the first prepare
    // (this initial call has no audio thread running yet, so the start is safe).
    // The pool is block-size independent - its threads park until runBlock
    // dispatches and only touch the per-block laneAccum buffers (resized above),
    // so later device re-opens (buffer/rate changes) must NOT stop+start it:
    // their prepare runs while the audio callback is still attached and a
    // stop()/start() would race a live runBlock. Live worker-count changes go
    // through applyDesiredWorkers (callback detached). 0 -> serial.
    if (! workerPoolStarted)
    {
        workerPoolStarted = true;
        reconcileWorkerPool (resolveTargetWorkers());
    }
}

int AudioEngine::resolveTargetWorkers() const noexcept
{
    if (const char* env = std::getenv ("DUSKSTUDIO_AUDIO_WORKERS"))
        return juce::String (env).getIntValue();
    return desiredWorkers;
}

void AudioEngine::suspendProcessing()
{
    processingSuspended.store (true, std::memory_order_release);
    int waitedMs = 0;
    while (callbacksInFlight.load (std::memory_order_acquire) > 0)
    {
        juce::Thread::sleep (1);
        if (++waitedMs % 1000 == 0)
            std::fprintf (stderr,
                          "[Dusk Studio/AudioEngine] suspendProcessing waiting %d s for an "
                          "in-flight audio callback to drain (wedged plugin?)\n",
                          waitedMs / 1000);
    }
}

void AudioEngine::resumeProcessing() noexcept
{
    processingSuspended.store (false, std::memory_order_release);
}

void AudioEngine::applyDesiredWorkers()
{
    jassert (juce::MessageManager::existsAndIsCurrentThread());
    workerPoolStarted = true;
    reconcileWorkerPool (resolveTargetWorkers());
}

void AudioEngine::reconcileWorkerPool (int target)
{
    // Cap = cores - 2. The workers run at real-time priority alongside the
    // audio callback thread, so `workers + audio-thread` must stay BELOW the
    // core count - otherwise the real-time set saturates every core and starves
    // the normal-priority message (UI) thread, freezing the app exactly under
    // the heavy load this is meant to help. cores - 2 leaves one core for the
    // UI + the OS on top of the audio thread.
    const int cap = juce::jmin (juce::SystemStats::getNumCpus() - 2, kMaxDspLanes - 1);
    const int n   = juce::jlimit (0, juce::jmax (0, cap), target);
    const int cur = workerPool.isActive() ? workerPool.laneCount() - 1 : 0;
    if (n == cur)
        return;

    workerPool.stop();
    if (n > 0)
        workerPool.start (n, [this] (int lane) { processStripLane (lane); },
                          rt::queryRealtimePriority().jucePriority);

    // One-line stderr marker so it's obvious whether the parallel path is live.
    std::fprintf (stderr, "[DuskStudio] parallel strip DSP: %d worker(s)\n", n);
}

void AudioEngine::setWorkerCountForTest (int n)
{
    reconcileWorkerPool (n);
}

void AudioEngine::audioDeviceError (const juce::String& errorMessage)
{
    // Fires from the audio thread (ALSA run() at fatal-recover exit;
    // macOS / Windows backends similar). stderr fprintf is RT-safe
    // enough for a dying-device path; everything else (FileLogger
    // write, RecordManager::stopRecording, Transport state-flip) is
    // message-thread-only and gets dispatched via callAsync. Without
    // the dispatch we'd join the disk thread + flush a ThreadedWriter
    // on the audio thread - RecordManager's stopRecording contract
    // forbids it. JUCE doesn't guarantee a follow-up
    // audioDeviceStopped after this callback, so we force it.
    std::fprintf (stderr, "[Dusk Studio/AudioEngine] audioDeviceError: %s\n",
                  errorMessage.toRawUTF8());

    juce::MessageManager::callAsync (
        [this, errorMessage]
        {
            juce::Logger::writeToLog ("[Dusk Studio/AudioEngine] audioDeviceError: "
                                          + errorMessage);
            audioDeviceStopped();
        });
}

void AudioEngine::audioDeviceStopped()
{
    // Drain any in-flight worker lanes FIRST, while the device's input buffers
    // (which trackJobs points into) are still alive - the device close() that
    // follows this callback frees them. Covers the force-killed-I/O-thread case
    // where dispatched lanes outlive their dispatcher; a healthy stop makes
    // this a no-op.
    workerPool.quiesce();

    // Same force-kill case for the callback-level gate: an I/O thread killed by
    // stopThread()'s timeout can die mid-callback, before InFlightGuard
    // decrements callbacksInFlight - leaking the count so the NEXT
    // suspendProcessing() would wait on it forever. The I/O thread is confirmed
    // stopped by the time this hook runs (no callback can be in flight), so
    // reset the counter to reconcile any such leak.
    callbacksInFlight.store (0, std::memory_order_release);

    // Stop any in-flight recording BEFORE clearing the sample rate: the
    // writer drain in stopRecording depends on recordSampleRate being
    // non-zero to map sample positions to seconds. Without this, a
    // device disconnect mid-take leaves recordManager.active = true,
    // and any audio block that slips in while the device is being torn
    // down would call writeInputBlock at sr=0 with stale writers - data
    // corruption shape. Also flips Transport state so the UI's
    // record button immediately reflects "stopped" rather than the
    // pre-disconnect "Recording".
    if (recordManager.isActive())
        recordManager.stopRecording (transport.getPlayhead());
    // Force-stop the transport on ANY device loss (not just
    // Recording). A hot-unplug while playing leaves the transport
    // in Playing with no device to render - user gets a silent
    // running transport which is confusing. Stop it explicitly so
    // the next device come-up doesn't pick up mid-play.
    if (transport.isPlaying() || transport.isRecording())
        transport.setState (Transport::State::Stopped);

    currentSampleRate.store (0.0, std::memory_order_relaxed);
    currentBlockSize.store  (0,   std::memory_order_relaxed);

    // Reclaim retired tempo-map copies. Only safe here: no callback can be
    // in flight, so nothing holds a pointer into the pool except the live
    // map the next device start will read.
    if (rtTempoMapPool.size() > 1)
        rtTempoMapPool.erase (rtTempoMapPool.begin(), rtTempoMapPool.end() - 1);
    // Reset to the optimistic default so a transient "no device open" state
    // doesn't stick a NO OUTPUTS warning on the UI.
    usableOutputs.store (true, std::memory_order_relaxed);
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    // H5 hot-unplug detector. AudioDeviceManager broadcasts change
    // messages whenever its device list / current-device state
    // changes; we ignore broadcasts from any OTHER source (the
    // engine itself broadcasts via its ChangeBroadcaster base; we
    // don't want to react to our own messages).
    if (source != &deviceManager) return;

    // Persist the chosen setup. createStateXml returns null until the
    // user has explicitly picked something (treatAsChosenDevice), so the
    // first-launch default pick is never frozen into the file - only
    // deliberate choices survive a restart.
    if (deviceManager.getCurrentAudioDevice() != nullptr)
        if (const auto xml = deviceManager.createStateXml())
            audioDeviceStateFile().replaceWithText (xml->toString());

    // The hot-unplug signal is "we had a live device, now the
    // current device is null." User-driven settings changes also
    // close the device, but they're followed by addAudioCallback /
    // audioDeviceAboutToStart for the new selection within the same
    // dispatch loop tick - hadLiveDevice_ would still be true and
    // we'd false-alarm. To avoid that, we re-check after a one-shot
    // callAsync defer: if a new device came up in the meantime,
    // hadLiveDevice_ is true AND getCurrentAudioDevice() is non-null,
    // so we bail. If still null, real loss; fire the alert.
    if (! hadLiveDevice_.load (std::memory_order_acquire)) return;
    if (deviceManager.getCurrentAudioDevice() != nullptr) return;

    // Defer one tick so a settings-driven device swap can complete
    // before we decide it's a hot-unplug. The audioDeviceAboutToStart
    // for the new device will republish hadLiveDevice_ = true and
    // the deferred check below sees getCurrentAudioDevice() != null,
    // bailing without spurious alert.
    juce::MessageManager::callAsync ([this]
    {
        if (deviceManager.getCurrentAudioDevice() != nullptr) return;
        if (! hadLiveDevice_.load (std::memory_order_acquire)) return;

        // Confirmed loss. Clear the flag so a subsequent change
        // broadcast for the same loss doesn't re-fire.
        hadLiveDevice_.store (false, std::memory_order_release);

        std::fprintf (stderr,
                      "[Dusk Studio/AudioEngine] hot-unplug detected - no current "
                      "audio device. Transport stopped.\n");

        // audioDeviceStopped already force-stopped transport +
        // recording; this is belt-and-braces in case the order of
        // callbacks differs across backends. setState is no-op when
        // already Stopped.
        if (transport.isPlaying() || transport.isRecording())
        {
            if (transport.isRecording())
                recordManager.stopRecording (transport.getPlayhead());
            transport.setState (Transport::State::Stopped);
        }

        if (onDeviceLostAlert_)
        {
            onDeviceLostAlert_ (
                "The active audio device has disconnected. Open Audio Settings to "
                "select a new device.");
        }
    });
}

void AudioEngine::writeMasterMixToOutput (float* const* outputChannelData,
                                          int numOutputChannels, int numSamples) noexcept
{
    const int mp  = session.master().outputPair.load (std::memory_order_relaxed);
    const int enc = mp > 0 ? mp : outputpair::encodePair (0, 1);
    outputpair::tapStereoPairInto (outputChannelData, numOutputChannels,
                                     mixL.data(), mixR.data(), numSamples, enc);
}

void AudioEngine::accumulateStrip (int t, float* mL, float* mR,
                                   const std::array<float*, ChannelStrip::kNumBuses>& bL,
                                   const std::array<float*, ChannelStrip::kNumBuses>& bR,
                                   const std::array<float*, ChannelStripParams::kNumAuxSends>& aL,
                                   const std::array<float*, ChannelStripParams::kNumAuxSends>& aR,
                                   int numSamples) noexcept
{
    const auto& job = trackJobs[(size_t) t];
    strips[(size_t) t].processAndAccumulate (job.monoIn, job.monoInR,
                                             perTrackMidiScratch[(size_t) t], job.isMidi,
                                             mL, mR, bL, bR, aL, aR,
                                             numSamples, job.passes,
                                             currentDeviceInputs,  numCurrentDeviceInputs,
                                             currentDeviceOutputs, numCurrentDeviceOutputs,
                                             job.frozen);
}

void AudioEngine::processStripLane (int lane) noexcept
{
    const int lanes = workerPool.laneCount();
    const int n     = currentBlockSamples;
    auto& acc = laneAccum[(size_t) lane];

    // Each lane owns its accum set - clear it, build pointer views, accumulate
    // a contiguous strip subset. No lane writes another lane's buffers.
    juce::FloatVectorOperations::clear (acc.mixL.data(), n);
    juce::FloatVectorOperations::clear (acc.mixR.data(), n);
    std::array<float*, ChannelStrip::kNumBuses> bL {}, bR {};
    for (int a = 0; a < ChannelStrip::kNumBuses; ++a)
    {
        juce::FloatVectorOperations::clear (acc.busL[(size_t) a].data(), n);
        juce::FloatVectorOperations::clear (acc.busR[(size_t) a].data(), n);
        bL[(size_t) a] = acc.busL[(size_t) a].data();
        bR[(size_t) a] = acc.busR[(size_t) a].data();
    }
    std::array<float*, ChannelStripParams::kNumAuxSends> aL {}, aR {};
    for (int a = 0; a < ChannelStripParams::kNumAuxSends; ++a)
    {
        juce::FloatVectorOperations::clear (acc.auxL[(size_t) a].data(), n);
        juce::FloatVectorOperations::clear (acc.auxR[(size_t) a].data(), n);
        aL[(size_t) a] = acc.auxL[(size_t) a].data();
        aR[(size_t) a] = acc.auxR[(size_t) a].data();
    }

    const int lo = (Session::kNumTracks * lane)       / lanes;
    const int hi = (Session::kNumTracks * (lane + 1)) / lanes;
    // Hardware-insert strips write the SHARED device-output buffer; a worker
    // lane can't do that concurrently, so they're skipped here and summed on
    // the audio thread after the reduce.
    for (int t = lo; t < hi; ++t)
        if (! trackJobs[(size_t) t].hardwareInsert)
            accumulateStrip (t, acc.mixL.data(), acc.mixR.data(), bL, bR, aL, aR, n);
}

void AudioEngine::reduceLaneAccum (int numSamples) noexcept
{
    const int lanes = workerPool.laneCount();
    for (int lane = 0; lane < lanes; ++lane)
    {
        auto& acc = laneAccum[(size_t) lane];
        juce::FloatVectorOperations::add (mixL.data(), acc.mixL.data(), numSamples);
        juce::FloatVectorOperations::add (mixR.data(), acc.mixR.data(), numSamples);
        for (int a = 0; a < Session::kNumBuses; ++a)
        {
            juce::FloatVectorOperations::add (busL[(size_t) a].data(), acc.busL[(size_t) a].data(), numSamples);
            juce::FloatVectorOperations::add (busR[(size_t) a].data(), acc.busR[(size_t) a].data(), numSamples);
        }
        for (int a = 0; a < Session::kNumAuxLanes; ++a)
        {
            juce::FloatVectorOperations::add (auxLaneL[(size_t) a].data(), acc.auxL[(size_t) a].data(), numSamples);
            juce::FloatVectorOperations::add (auxLaneR[(size_t) a].data(), acc.auxR[(size_t) a].data(), numSamples);
        }
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    // Empty callbacks happen during device transitions (JUCE/JACK both
    // can send numSamples==0). Skip the entire pipeline - MIDI parsing,
    // automation routing, strip DSP, master bus, recording - so we don't
    // burn cycles or risk corrupting downstream invariants (smoothers,
    // playhead) on a zero-length block.
    if (numSamples <= 0) return;

    // Process gate (the Ardour _process_lock pattern, lock-free flavour).
    // suspendProcessing() (message thread) raises the flag and waits for
    // in-flight callbacks to drain; from then until resumeProcessing() every
    // callback - from ANY backend thread, joined or not - emits one silent
    // buffer instead of touching engine state mid-reconfigure. The double
    // check around the counter closes the raise-after-first-load window.
    auto clearOutputs = [&]
    {
        if (outputChannelData == nullptr) return;   // matches the guard below
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (auto* out = outputChannelData[ch])
                juce::FloatVectorOperations::clear (out, numSamples);
    };
    if (processingSuspended.load (std::memory_order_acquire))
    {
        earlyOutBlocks.fetch_add (1, std::memory_order_relaxed);   // drained by diagTimer
        clearOutputs();
        return;
    }
    callbacksInFlight.fetch_add (1, std::memory_order_acq_rel);
    if (processingSuspended.load (std::memory_order_acquire))
    {
        callbacksInFlight.fetch_sub (1, std::memory_order_acq_rel);
        clearOutputs();
        return;
    }
    struct InFlightGuard
    {
        std::atomic<int>& counter;
        ~InFlightGuard() { counter.fetch_sub (1, std::memory_order_acq_rel); }
    };
    const InFlightGuard inFlightGuard { callbacksInFlight };

    const auto callbackStart = juce::Time::getHighResolutionTicks();

    // Section attribution (see PerfSections). One cached-bool branch per
    // lap when disabled; the mastering-stage shortcut and other early
    // returns simply never lap, so only full mixer passes are counted.
    const bool perfOn = perf.enabled.load (std::memory_order_relaxed);
    std::int64_t perfMark = callbackStart;
    auto perfLap = [&] (int section) noexcept
    {
        if (! perfOn) return;
        const auto now = juce::Time::getHighResolutionTicks();
        perf.ticks[(size_t) section].fetch_add (now - perfMark,
                                                std::memory_order_relaxed);
        perfMark = now;
    };

    // Cache the device-I/O pointers + channel counts for the strips'
    // hardware-insert slots. Strips read/write through these for the
    // duration of THIS callback only - never cached across blocks.
    currentDeviceInputs     = inputChannelData;
    numCurrentDeviceInputs  = numInputChannels;
    currentDeviceOutputs    = outputChannelData;
    numCurrentDeviceOutputs = numOutputChannels;

    // Zero every device output up front. The master bus + click
    // overwrite their assigned channels at the end of the callback via
    // memcpy so pre-zeroing them is harmless. Hardware-insert sends
    // accumulate into their assigned channels via `+=`, which requires
    // a clean zero baseline. Pre-zeroing everything avoids baking in a
    // "master is always 0/1" assumption.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
            std::memset (outputChannelData[ch], 0,
                          sizeof (float) * (size_t) numSamples);

    // Drain each MIDI input into its per-input buffer for this block. The
    // layer's drain is lock-free with respect to the MIDI thread's enqueue,
    // so the audio thread never contends. Empty buffers cost ~nothing.
    for (int i = 0; i < midiIn.getNumInputs() && i < (int) perInputMidi.size(); ++i)
        midiIn.drainBlock (i, perInputMidi[(size_t) i], numSamples);

    // Test-hook: if the message thread staged a buffer via
    // stageTestMidiInjection, merge it into the requested input's
    // per-block buffer, clear the staging slot, and release the SPSC
    // ready flag so the next stage call may proceed. Empty in production -
    // the relaxed-equivalent acquire load costs a single load + branch.
    if (testInjectReady.load (std::memory_order_acquire))
    {
        const int idx = testInjectInputIdx.load (std::memory_order_relaxed);
        if (idx >= 0 && (size_t) idx < perInputMidi.size())
            for (const auto meta : testInjectMidi)
                if (meta.samplePosition >= 0 && meta.samplePosition < numSamples)
                    perInputMidi[(size_t) idx].addEvent (meta.data, meta.numBytes,
                                                         meta.samplePosition);
        testInjectMidi.clear();
        testInjectReady.store (false, std::memory_order_release);
    }

    // External MIDI Clock sync. Feed the chosen input's per-block
    // buffer to the receiver; if it derived a fresh BPM and the
    // follow-tempo flag is on, push the value into session.tempoBpm
    // so PlaybackEngine + the metronome track the master. Status
    // atoms feed the UI's "EXT" badge.
    //
    // Timestamping uses a private monotonic sample clock that ticks by
    // numSamples each block. transport.getPlayhead() jumps on loop
    // wrap / Cmd-jump / scrub - using it for clock intervals would
    // produce wild BPM swings whenever the user moves the playhead.
    const int syncIdx = session.syncSourceInputIdx.load (std::memory_order_acquire);
    if (syncIdx != lastSyncSourceIdx)
    {
        // Source changed (or first run). Reset both the receiver's
        // history and the local sample clock so the new source starts
        // from a clean baseline. Also clear lastExtRolling - without
        // this, if the old source was rolling and the new source is
        // stopped, the first block fires a spurious Stop edge; if the
        // new source is rolling first-block, an old "rolling" latch can
        // suppress the Start edge that should fire.
        midiSyncReceiver.reset();
        midiTimeCodeReceiver.reset();
        midiSyncSampleClock = 0;
        lastSyncSourceIdx = syncIdx;
        lastExtRolling = false;
        lastMtcRolling = false;
        mtcDriftWindowFrames = 0;
        lastSeenMtcFrames = -1;
    }
    // Mackie Control surface input. Independent of sync source - the
    // user could run MIDI Clock on device A and the MCU on device B.
    // Gate on the resolved index so MCU decode is a no-op when no
    // controller is selected. Audio-thread only; receiver writes
    // directly to Session atoms.
    if (mcuReceiver != nullptr)
    {
        const int mcuIdx = session.mcu.resolvedInputIdx.load (std::memory_order_acquire);
        if (mcuIdx >= 0 && (size_t) mcuIdx < perInputMidi.size())
            mcuReceiver->process (perInputMidi[(size_t) mcuIdx], midiSyncSampleClock);
    }

    if (syncIdx >= 0 && (size_t) syncIdx < perInputMidi.size())
    {
        // The drained input block is already a dusk::MidiBuffer - feed it
        // straight to the receivers (Clock + MTC multiplex on the same port).
        midiSyncReceiver.process (perInputMidi[(size_t) syncIdx], midiSyncSampleClock);
        midiTimeCodeReceiver.process (perInputMidi[(size_t) syncIdx],
                                         midiSyncSampleClock, numSamples);
        // Publish MTC decoded state. Same input port - Clock + MTC
        // multiplex on it; both decoders ignore bytes they don't own.
        const auto mtcFrames  = midiTimeCodeReceiver.getFrames();
        const bool mtcRolling = midiTimeCodeReceiver.isRolling();
        const auto mtcRate    = midiTimeCodeReceiver.getFrameRate();
        session.externalTimeCodeFrames   .store (mtcFrames, std::memory_order_relaxed);
        session.externalTimeCodeRolling  .store (mtcRolling, std::memory_order_relaxed);
        session.externalTimeCodeReversed .store (midiTimeCodeReceiver.isReversed(),
                                                   std::memory_order_relaxed);
        session.externalTimeCodeFrameRate.store ((int) mtcRate,
                                                   std::memory_order_relaxed);

        // MTC transport chase (freewheeling model).
        //   Initial lock: rolling false->true -> set playhead + Play.
        //   Freewheel:    |transport - mtc| ≤ kFreewheelToleranceFrames
        //                 -> trust internal clock, no relocate.
        //   Soft re-locate: drift > tolerance for kFreewheelReSyncWindow
        //                   consecutive MTC frames -> set playhead.
        //   Stop:         rolling true->false -> Stop (UNLESS the false
        //                 came from a reverse-scrub park; reverse keeps
        //                 transport rolling forward per plan).
        const bool chaseEnabled =
            session.externalTimeCodeChasesTransport.load (std::memory_order_relaxed);
        const bool reversed = midiTimeCodeReceiver.isReversed();

        // Force re-lock when user just enabled chase mid-roll. Without
        // this, the rising-edge happened while chase was off and the
        // edge detector missed it; transport would never snap.
        if (chaseEnabled && ! lastChaseEnabled && mtcRolling)
            lastMtcRolling = false;
        lastChaseEnabled = chaseEnabled;

        if (chaseEnabled)
        {
            constexpr int kFreewheelToleranceFrames = 2;
            constexpr int kFreewheelReSyncWindow    = 4;

            // Samples-per-frame at the current SMPTE rate. Reads sr
            // off the engine atom - well-defined because this block
            // can't execute until audioDeviceAboutToStart published a
            // non-zero rate.
            const double sr = currentSampleRate.load (std::memory_order_relaxed);
            const double sPerFrame =
                  mtcRate == MidiTimeCodeReceiver::Fps24      ? sr / 24.0
                : mtcRate == MidiTimeCodeReceiver::Fps25      ? sr / 25.0
                : mtcRate == MidiTimeCodeReceiver::Fps29_97DF ? sr * 1001.0 / 30000.0
                                                              : sr / 30.0;

            if (mtcRolling && ! lastMtcRolling)
            {
                // Initial lock on rising edge.
                session.pendingTransportPlayhead.store (
                    (std::int64_t) ((double) mtcFrames * sPerFrame),
                    std::memory_order_relaxed);
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Play,
                    std::memory_order_relaxed);
                mtcDriftWindowFrames = 0;
                lastSeenMtcFrames    = mtcFrames;
            }
            else if (! mtcRolling && lastMtcRolling && ! reversed)
            {
                // Falling edge -> stop. Gated on !reversed so a reverse-
                // scrub (which the receiver reports as rolling=false +
                // reversed=true) doesn't drag the transport into Stop;
                // master scrubbing back leaves Dusk Studio rolling forward.
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Stop,
                    std::memory_order_relaxed);
                mtcDriftWindowFrames = 0;
            }
            else if (mtcRolling && ! reversed && sPerFrame > 0.0)
            {
                // Freewheel + soft re-locate. Drift counter ticks only
                // when MTC actually advances a frame (not every audio
                // block) so kFreewheelReSyncWindow is in MTC-frame
                // units as the comment promises. Without the gate the
                // counter ticks at audio-block rate (~21 ms @ 48 k /
                // 1024 samples) and the window collapses to ~80 ms.
                if (mtcFrames != lastSeenMtcFrames)
                {
                    const auto transportSamples = transport.getPlayhead();
                    const auto mtcSamples =
                        (std::int64_t) ((double) mtcFrames * sPerFrame);
                    const auto deltaSamples = transportSamples - mtcSamples;
                    const auto deltaFrames = std::llabs (
                        (std::int64_t) ((double) deltaSamples / sPerFrame));
                    if (deltaFrames > kFreewheelToleranceFrames)
                    {
                        if (++mtcDriftWindowFrames >= kFreewheelReSyncWindow)
                        {
                            session.pendingTransportPlayhead.store (
                                mtcSamples, std::memory_order_relaxed);
                            mtcDriftWindowFrames = 0;
                        }
                    }
                    else
                    {
                        mtcDriftWindowFrames = 0;
                    }
                    lastSeenMtcFrames = mtcFrames;
                }
            }
        }

        // Edge detector advances every block regardless of chaseEnabled
        // so re-enabling chase mid-roll observes the correct current
        // state (combined with the force-relock above this gives the
        // user "snap to MTC now" UX without missing edges that
        // happened while disabled).
        lastMtcRolling = mtcRolling;

        const float ext = midiSyncReceiver.getBpm();
        const bool extRolling = midiSyncReceiver.isRolling();
        session.externalBpm.store (ext, std::memory_order_relaxed);
        session.externalSyncRolling.store (extRolling, std::memory_order_relaxed);
        if (ext > 0.0f
            && session.externalSyncFollowsTempo.load (std::memory_order_relaxed))
        {
            session.tempoBpm.store (ext, std::memory_order_relaxed);
        }

        // Transport chase. Edge-triggered on the rolling flag so a
        // long-running Start signal doesn't restart the transport
        // every block. The actual play()/stop() calls are message-
        // thread - we signal via pendingTransportAction and let the
        // engine's existing timer drain it (same path MIDI Learn
        // transport bindings use).
        if (session.externalSyncChasesTransport.load (std::memory_order_relaxed))
        {
            if (extRolling && ! lastExtRolling)
            {
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Play,
                    std::memory_order_relaxed);
            }
            else if (! extRolling && lastExtRolling)
            {
                session.pendingTransportAction.store (
                    (int) PendingTransportAction::Stop,
                    std::memory_order_relaxed);
            }
        }
        lastExtRolling = extRolling;
    }
    else
    {
        // Clear the readouts when no source is selected so the UI badge
        // doesn't stay frozen at the last value after the user turns
        // sync off.
        session.externalBpm.store (0.0f, std::memory_order_relaxed);
        session.externalSyncRolling.store (false, std::memory_order_relaxed);
        session.externalTimeCodeFrames  .store (0,     std::memory_order_relaxed);
        session.externalTimeCodeRolling .store (false, std::memory_order_relaxed);
        session.externalTimeCodeReversed.store (false, std::memory_order_relaxed);
        lastExtRolling = false;
    }
    midiSyncSampleClock += numSamples;

    // MIDI Clock OUTPUT (master mode). Generates F8 + FA/FC (+ MTC) bytes into
    // a dusk scratch buffer and hands it to the out-bank's RT queue for the
    // pump thread to deliver. Uses the same monotonic sync clock so receiver +
    // emitter share a sample-time origin (matters when a user has Dusk Studio
    // as both slave AND master - rare but possible via a MIDI thru).
    // acquire pairs with the release stores in rebuildMidiBanks and
    // AudioSettingsPanel: any state the writer published before flipping the
    // index (the opened output port + its background-thread state) is visible
    // here before we route bytes to it.
    const int syncOutIdx = session.syncOutputIdx.load (std::memory_order_acquire);
    if (syncOutIdx != lastSyncOutputIdx)
    {
        midiClockEmitter.reset();
        midiTimeCodeEmitter.reset();
        lastSyncOutputIdx = syncOutIdx;
    }
    const bool portReady = syncOutIdx >= 0 && midiOut.isOpen (syncOutIdx);
    const bool emitClock = portReady
                         && session.syncOutputEmitClock.load (std::memory_order_relaxed);
    const bool emitTimeCode = portReady
                            && session.syncOutputEmitTimeCode.load (std::memory_order_relaxed);
    if (emitClock || emitTimeCode)
    {
        // Rolling state from the engine's transport - the bytes drive
        // SLAVES, so they need to mirror Dusk Studio's local state, not the
        // external master's.
        const bool rolling = transport.isPlaying() || transport.isRecording();
        midiClockOutScratch.clear();

        if (emitClock)
        {
            const float emitBpm = session.tempoBpm.load (std::memory_order_relaxed);
            midiClockEmitter.generateBlock (midiSyncSampleClock - numSamples,
                                              numSamples, emitBpm, rolling,
                                              midiClockOutScratch);
        }
        if (emitTimeCode)
        {
            // MTC + Clock multiplex onto the same buffer; the emitter appends
            // its QF/sysex bytes after the clock bytes, delivered together.
            const auto rate = (MidiTimeCodeEmitter::FrameRate)
                session.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed);
            midiTimeCodeEmitter.generateBlock (midiSyncSampleClock - numSamples,
                                                  numSamples,
                                                  transport.getPlayhead(),
                                                  rolling, rate,
                                                  midiClockOutScratch);
        }

        if (! midiClockOutScratch.isEmpty())
            midiOut.queueRt (syncOutIdx, midiClockOutScratch,
                             currentSampleRate.load (std::memory_order_relaxed));
    }

    // Held-MIDI-notes tracking for the chord display. Walk every drained
    // MIDI buffer and flip the per-note atomic on Note On / Note Off.
    // Treat NoteOn vel=0 as NoteOff (running-status convention). Cheap -
    // ~100 ns per event - and the SystemStatusBar's chord poll reads a
    // snapshot of the array off-thread.
    for (const auto& buf : perInputMidi)
    {
        if (buf.isEmpty()) continue;
        for (const auto meta : buf)
        {
            // Bridge the dusk event to juce for its message-parsing API (same
            // per-event cost as the pre-flip juce buffer's getMessage()).
            const auto& v = meta.getMessage();
            const juce::MidiMessage m (v.getRawData(), v.getRawDataSize());
            if (m.isNoteOn() && m.getVelocity() > 0)
            {
                const int n = m.getNoteNumber();
                if (n >= 0 && n < Session::kNumMidiNotes)
                    session.heldMidiNotes[(size_t) n].store (true,
                                                                std::memory_order_relaxed);
            }
            else if (m.isNoteOff() || (m.isNoteOn() && m.getVelocity() == 0))
            {
                const int n = m.getNoteNumber();
                if (n >= 0 && n < Session::kNumMidiNotes)
                    session.heldMidiNotes[(size_t) n].store (false,
                                                                std::memory_order_relaxed);
            }
        }
    }

    // MIDI controller bindings - apply BEFORE the per-track filter so a
    // bound CC drives its target regardless of which track the message
    // happens to be addressed to. Lock-free acquire-load of the binding
    // snapshot (mutated only on the message thread via AtomicSnapshot's
    // copy-and-swap). Continuous targets write atoms directly; transport
    // actions queue into pendingTransportAction (engine.play/stop/record
    // aren't RT-safe). Note triggers fire on press only (NoteOn vel > 0);
    // NoteOff and CC release are ignored per the v1 spec.
    //
    // Also enter the loop when a MIDI Learn capture is pending - the
    // capture lives inside this loop, and gating it on bindings being
    // non-empty would make it impossible to learn the FIRST binding
    // (no bindings -> no loop -> no capture -> no first binding ever).
    const auto* bindings = session.midiBindings.read();
    const bool hasBindings = bindings != nullptr && ! bindings->empty();
    const bool learnPending = session.midiLearnPending.load (std::memory_order_relaxed) >= 0;
    if (hasBindings || learnPending)
    {
        for (const auto& buf : perInputMidi)
        {
            if (buf.isEmpty()) continue;
            for (const auto meta : buf)
            {
                const auto& v = meta.getMessage();
                const juce::MidiMessage m (v.getRawData(), v.getRawDataSize());
                MidiBindingTrigger tg;
                int dn = 0, val = 0;
                if      (m.isController())                    { tg = MidiBindingTrigger::CC;         dn = m.getControllerNumber(); val = m.getControllerValue(); }
                else if (m.isNoteOn() && m.getVelocity() > 0) { tg = MidiBindingTrigger::Note;       dn = m.getNoteNumber();       val = m.getVelocity(); }
                else if (m.isPitchWheel())                    { tg = MidiBindingTrigger::PitchBend;  dn = 0;                       val = m.getPitchWheelValue(); }
                else if (m.isMidiMachineControlMessage())     { tg = MidiBindingTrigger::MmcCommand; dn = (int) m.getMidiMachineControlCommand(); val = 127; }
                else continue;
                const int ch = m.getChannel();

                // Learn capture - take the first matching event when a
                // learn target is pending. Audio-thread CAS-store; the
                // message thread drains and appends a binding, then
                // clears midiLearnPending. We still apply normal binding
                // matching below so a freshly-captured CC also drives
                // its target on the same block (cheap, harmless).
                if (session.midiLearnPending.load (std::memory_order_relaxed) >= 0
                    && ! learnCaptureIsValid (session.midiLearnCapture.load (std::memory_order_relaxed)))
                {
                    session.midiLearnCapture.store (
                        packLearnCapture (tg, ch, dn), std::memory_order_relaxed);
                }

                if (! hasBindings) continue;
                for (const auto& sourceBinding : *bindings)
                {
                    if (! sourceBinding.sourceMatches (ch, dn, tg)) continue;
                    if (! sourceBinding.isValid()) continue;

                    // Resolve bank-relative targets up front so the switch
                    // below stays one big case per absolute target. For
                    // bank-relative variants `targetIndex` is a position
                    // 0..kBankSize-1 (or packed pos*N+sub); rewrite into the
                    // matching absolute target + absolute track index by
                    // adding activeBank * kBankSize to the position. The
                    // load is relaxed because the bank index is owned by
                    // the message thread (ConsoleView::setBank) and only
                    // changes on explicit user action.
                    MidiBinding b = sourceBinding;
                    if (isBankRelativeTarget (b.target))
                    {
                        const int bank = session.activeBank
                                              .load (std::memory_order_relaxed);
                        const int base = bank * Session::kBankSize;
                        switch (b.target)
                        {
                            case MidiBindingTarget::TrackFaderBank:
                                b.target = MidiBindingTarget::TrackFader;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackPanBank:
                                b.target = MidiBindingTarget::TrackPan;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackMuteBank:
                                b.target = MidiBindingTarget::TrackMute;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackSoloBank:
                                b.target = MidiBindingTarget::TrackSolo;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackArmBank:
                                b.target = MidiBindingTarget::TrackArm;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackAuxSendBank:
                            {
                                const int pos = b.targetIndex / kPackedAuxLanes;
                                const int aux = b.targetIndex % kPackedAuxLanes;
                                b.target = MidiBindingTarget::TrackAuxSend;
                                b.targetIndex = packTrackAux (base + pos, aux);
                                break;
                            }
                            case MidiBindingTarget::TrackHpfFreqBank:
                                b.target = MidiBindingTarget::TrackHpfFreq;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackEqGainBank:
                            case MidiBindingTarget::TrackEqFreqBank:
                            case MidiBindingTarget::TrackEqQBank:
                            {
                                const int pos = b.targetIndex / kPackedEqBands;
                                const int band = b.targetIndex % kPackedEqBands;
                                b.target = b.target == MidiBindingTarget::TrackEqGainBank ? MidiBindingTarget::TrackEqGain
                                         : b.target == MidiBindingTarget::TrackEqFreqBank ? MidiBindingTarget::TrackEqFreq
                                                                                          : MidiBindingTarget::TrackEqQ;
                                b.targetIndex = packTrackEqBand (base + pos, band);
                                break;
                            }
                            case MidiBindingTarget::TrackCompThreshBank:
                                b.target = MidiBindingTarget::TrackCompThresh;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackCompMakeupBank:
                                b.target = MidiBindingTarget::TrackCompMakeup;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            case MidiBindingTarget::TrackPluginParamBank:
                                b.target = MidiBindingTarget::TrackPluginParam;
                                b.targetIndex = base + b.targetIndex;
                                break;
                            default: break;
                        }
                    }

                    // 7-bit (CC/Note vel) and 14-bit (pitch-bend) sources
                    // normalize to a common 0..1 fraction. `pressed` is the
                    // trigger for discrete (toggle) targets - its
                    // definition depends on `buttonMode` so latching
                    // controllers (D-type buttons that alternate 127/0
                    // each press, e.g. Panorama T6 in some modes) toggle
                    // once per physical press instead of every other.
                    //
                    // MMC commands are one-shot events with no value - treat
                    // them as fully-pressed (frac = 1, pressed = true) so a
                    // bound transport / arm target fires on receipt.
                    float frac;
                    bool pressed;
                    if (b.trigger == MidiBindingTrigger::MmcCommand)
                    {
                        frac = 1.0f; pressed = true;
                    }
                    else if (b.trigger == MidiBindingTrigger::PitchBend)
                    {
                        frac = (float) val / 16383.0f; pressed = val >= 8192;
                    }
                    else
                    {
                        frac = (float) val / 127.0f;
                        pressed = (b.buttonMode == MidiButtonMode::Toggle)
                                      ? true              // fire every received CC / Note
                                      : (val >= 64);      // rising-edge / level threshold
                    }

                    // Soft takeover (pickup): an absolute controller snaps the
                    // target to its own position on first touch. When enabled,
                    // a continuous binding stays dormant until the controller
                    // lands near - or sweeps across - the parameter's current
                    // position, then latches and tracks 1:1. Latch state is
                    // keyed by snapshot index and reset whenever the bindings
                    // snapshot is republished (edits re-arm the pickup).
                    if (midiSoftTakeover.load (std::memory_order_relaxed)
                        && b.trigger != MidiBindingTrigger::MmcCommand)
                    {
                        if ((const void*) bindings != pickupSnapshotKey)
                        {
                            pickupSnapshotKey = bindings;
                            pickupLatched.fill (0);
                            pickupPrevIn.fill (-1.0f);
                        }
                        const auto idx = (size_t) (&sourceBinding - bindings->data());
                        if (idx < pickupLatched.size() && ! pickupLatched[idx])
                        {
                            const float cur = currentFracForTarget (session, b);
                            if (cur >= 0.0f)   // continuous, readable target
                            {
                                constexpr float kPickupEps = 2.0f / 127.0f;
                                const float prev = pickupPrevIn[idx];
                                const bool near    = std::abs (frac - cur) <= kPickupEps;
                                const bool crossed = prev >= 0.0f
                                                      && (prev - cur) * (frac - cur) <= 0.0f;
                                if (near || crossed)
                                {
                                    pickupLatched[idx] = 1;
                                }
                                else
                                {
                                    pickupPrevIn[idx] = frac;
                                    continue;   // dormant until pickup
                                }
                            }
                        }
                    }

                    switch (b.target)
                    {
                        case MidiBindingTarget::TransportPlay:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Play, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::TransportStop:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Stop, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::TransportRecord:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Record, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::TransportToggle:
                            session.pendingTransportAction.store (
                                (int) PendingTransportAction::Toggle, std::memory_order_relaxed);
                            break;

                        case MidiBindingTarget::TrackFader:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                // CC/Note: linear -90..+12 dB. PitchBend: the
                                // Mackie taper (faderBindingFracToDb) so an
                                // MCU-style motor fader tracks its printed
                                // scale - 0 dB at ~3/4 throw, bottom at -100 dB
                                // (below kFaderInfThreshDb, hard-muted).
                                const float db = faderBindingFracToDb (
                                    frac, b.trigger == MidiBindingTrigger::PitchBend);
                                session.setTrackFaderGrouped (b.targetIndex, db);
                            }
                            break;
                        case MidiBindingTarget::TrackPan:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                const float p = frac * 2.0f - 1.0f;
                                session.track (b.targetIndex).strip.pan.store (
                                    p, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackMute:
                            if (pressed && b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex).strip.mute;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackSolo:
                            if (pressed && b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                // Route through Session helper so the
                                // soloTrackCount RT counter stays in sync.
                                // Direct atom store bypasses the counter
                                // -> audio thread misjudges "any soloed?".
                                const bool was = session.track (b.targetIndex).strip.solo
                                                       .load (std::memory_order_relaxed);
                                session.setTrackSoloed (b.targetIndex, ! was);
                            }
                            break;
                        case MidiBindingTarget::TrackArm:
                            if (pressed && b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                // Same counter-consistency reason as solo
                                // above: setTrackArmed maintains
                                // armedTrackCount which gates the
                                // "any-armed" fast path.
                                const bool was = session.track (b.targetIndex).recordArmed
                                                       .load (std::memory_order_relaxed);
                                session.setTrackArmed (b.targetIndex, ! was);
                            }
                            break;
                        case MidiBindingTarget::TrackAuxSend:
                        {
                            // Decode the packed (track, aux) index and map
                            // the 0..1 fraction onto the aux-send dB range.
                            // A zero-position source lands on the kAuxSendOffDb
                            // sentinel so a fully-down controller hard-mutes the
                            // send (matches the UI knob's CCW behaviour).
                            const int trk = unpackTrackAuxTrack (b.targetIndex);
                            const int aux = unpackTrackAuxLane  (b.targetIndex);
                            if (trk >= 0 && trk < Session::kNumTracks
                                && aux >= 0 && aux < ChannelStripParams::kNumAuxSends)
                            {
                                const float db = (val == 0)
                                    ? ChannelStripParams::kAuxSendOffDb
                                    : ChannelStripParams::kAuxSendMinDb
                                       + frac * (ChannelStripParams::kAuxSendMaxDb
                                                 - ChannelStripParams::kAuxSendMinDb);
                                session.track (trk).strip.auxSendDb[(size_t) aux]
                                    .store (db, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackHpfFreq:
                        {
                            // Zero-position maps to kHpfOffHz (bypass sentinel).
                            // Above zero is log-mapped across the band so the
                            // perceived sweep stays linear.
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumTracks)
                            {
                                float freq;
                                if (val == 0)
                                {
                                    freq = ChannelStripParams::kHpfOffHz;
                                }
                                else
                                {
                                    freq = ChannelStripParams::kHpfMinHz
                                         * std::exp (kHpfLogRange * frac);
                                }
                                session.track (b.targetIndex).strip.hpfFreq
                                    .store (freq, std::memory_order_relaxed);
                                session.track (b.targetIndex).strip.hpfEnabled
                                    .store (freq > ChannelStripParams::kHpfOffHz + 0.5f,
                                             std::memory_order_relaxed);
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackEqGain:
                        {
                            // 0..1 fraction -> -15..+15 dB linearly. Band index
                            // 0=LF, 1=LM, 2=HM, 3=HF; the apply path writes
                            // the matching atom on the strip.
                            const int trk  = unpackTrackEqTrack (b.targetIndex);
                            const int band = unpackTrackEqBand  (b.targetIndex);
                            if (trk >= 0 && trk < Session::kNumTracks
                                && band >= 0 && band < kPackedEqBands)
                            {
                                const float db = -15.0f + frac * 30.0f;
                                auto& strip = session.track (trk).strip;
                                switch (band)
                                {
                                    case 0: strip.lfGainDb.store (db, std::memory_order_relaxed); break;
                                    case 1: strip.lmGainDb.store (db, std::memory_order_relaxed); break;
                                    case 2: strip.hmGainDb.store (db, std::memory_order_relaxed); break;
                                    case 3: strip.hfGainDb.store (db, std::memory_order_relaxed); break;
                                }
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackEqFreq:
                        {
                            // Per-band frequency, log-mapped over each band's range
                            // (LF/LM/HM/HF have distinct min/max - see ChannelStripParams).
                            const int trk  = unpackTrackEqTrack (b.targetIndex);
                            const int band = unpackTrackEqBand  (b.targetIndex);
                            if (trk >= 0 && trk < Session::kNumTracks
                                && band >= 0 && band < kPackedEqBands)
                            {
                                auto logFreq = [frac] (float lo, float hi)
                                { return lo * std::exp (frac * std::log (hi / lo)); };
                                auto& strip = session.track (trk).strip;
                                switch (band)
                                {
                                    case 0: strip.lfFreq.store (logFreq (ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax), std::memory_order_relaxed); break;
                                    case 1: strip.lmFreq.store (logFreq (ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax), std::memory_order_relaxed); break;
                                    case 2: strip.hmFreq.store (logFreq (ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax), std::memory_order_relaxed); break;
                                    case 3: strip.hfFreq.store (logFreq (ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax), std::memory_order_relaxed); break;
                                }
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackEqQ:
                        {
                            // Only the two bell bands carry a Q (LM / HM); LF / HF are
                            // shelves and ignore this target.
                            const int trk  = unpackTrackEqTrack (b.targetIndex);
                            const int band = unpackTrackEqBand  (b.targetIndex);
                            if (trk >= 0 && trk < Session::kNumTracks)
                            {
                                const float q = ChannelStripParams::kBandQMin
                                    + frac * (ChannelStripParams::kBandQMax - ChannelStripParams::kBandQMin);
                                auto& strip = session.track (trk).strip;
                                if (band == 1) strip.lmQ.store (q, std::memory_order_relaxed);
                                else if (band == 2) strip.hmQ.store (q, std::memory_order_relaxed);
                            }
                            break;
                        }
                        case MidiBindingTarget::TrackCompThresh:
                        case MidiBindingTarget::TrackCompMakeup:
                        {
                            // Mode-aware: the strip exposes Opto / FET / VCA
                            // each with its own "threshold-ish" and "makeup-
                            // ish" knob, on different ranges. Pick the atom
                            // matching the CURRENT mode and remap CC 0..127
                            // onto that range. A single binding therefore
                            // tracks whatever mode the user has selected.
                            if (b.targetIndex < 0 || b.targetIndex >= Session::kNumTracks)
                                break;
                            auto& strip = session.track (b.targetIndex).strip;
                            const int mode = juce::jlimit (0, 2,
                                strip.compMode.load (std::memory_order_relaxed));
                            const bool isMakeup = (b.target == MidiBindingTarget::TrackCompMakeup);
                            auto remap = [frac] (float lo, float hi)
                            {
                                return lo + frac * (hi - lo);
                            };
                            if (isMakeup)
                            {
                                switch (mode)
                                {
                                    case 0: strip.compOptoGain.store (remap (0.0f, 100.0f), std::memory_order_relaxed); break;
                                    case 1: strip.compFetOutput.store (remap (-20.0f, 20.0f), std::memory_order_relaxed); break;
                                    case 2: strip.compVcaOutput.store (remap (-20.0f, 20.0f), std::memory_order_relaxed); break;
                                }
                                // Mirror to the unified makeup atom so the
                                // FET threshold math stays composed (see
                                // ChannelStripParams::compMakeupDb comment).
                                strip.compMakeupDb.store (remap (-20.0f, 20.0f), std::memory_order_relaxed);
                            }
                            else // threshold
                            {
                                switch (mode)
                                {
                                    case 0: strip.compOptoPeakRed.store (remap (0.0f, 100.0f), std::memory_order_relaxed); break;
                                    case 1: strip.compFetInput.store    (remap (-20.0f, 40.0f), std::memory_order_relaxed); break;
                                    case 2: strip.compVcaThreshDb.store (remap (-38.0f, 12.0f), std::memory_order_relaxed); break;
                                }
                            }
                            break;
                        }
                        case MidiBindingTarget::BusFader:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                const float db = faderBindingFracToDb (
                                    frac, b.trigger == MidiBindingTrigger::PitchBend);
                                session.bus (b.targetIndex).strip.faderDb.store (
                                    db, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::BusPan:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                const float p = frac * 2.0f - 1.0f;
                                session.bus (b.targetIndex).strip.pan.store (
                                    p, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::BusMute:
                            if (pressed && b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                auto& a = session.bus (b.targetIndex).strip.mute;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::BusSolo:
                            // Routes through Session::setBusSoloed so the
                            // anyBusSoloed counter stays in sync (the audio
                            // thread relies on it for O(1) "is any bus
                            // soloed?" checks).
                            if (pressed && b.targetIndex >= 0 && b.targetIndex < Session::kNumBuses)
                            {
                                const bool was = session.bus (b.targetIndex)
                                                       .strip.solo.load (std::memory_order_relaxed);
                                session.setBusSoloed (b.targetIndex, ! was);
                            }
                            break;
                        case MidiBindingTarget::TrackPluginParam:
                        {
                            // 0..1 fraction targets the strip's plugin slot
                            // at the param index stored in the binding
                            // (filled at learn-resolve time from the slot's
                            // last-touched tracker). paramIndex >= 0 is
                            // enforced here so the apply site matches the
                            // inline-validation pattern other targets use;
                            // the setters also no-op on out-of-range as a
                            // second line of defence. A native host owns the
                            // insert when loaded - same precedence as the
                            // audio chain (CLAP -> LV2 -> VST3 -> JUCE).
                            if (b.targetIndex >= 0
                                && b.targetIndex < Session::kNumTracks
                                && b.paramIndex >= 0)
                            {
                                auto& strip = getChannelStrip (b.targetIndex);
#if DUSKSTUDIO_HAS_NATIVE_CLAP
                                if (strip.isNativeClapLoaded())
                                {
                                    strip.getNativeClapSlot()
                                        .queueParamBinding ((uint32_t) b.paramIndex, frac);
                                    break;
                                }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
                                if (strip.isNativeLv2Loaded())
                                {
                                    strip.getNativeLv2Slot()
                                        .queueParamBinding ((uint32_t) b.paramIndex, frac);
                                    break;
                                }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
                                if (strip.isNativeVst3Loaded())
                                {
                                    strip.getNativeVst3Slot()
                                        .queueParamBinding ((uint32_t) b.paramIndex, frac);
                                    break;
                                }
#endif
                                strip.getPluginSlot()
                                    .setParamNormalised (b.paramIndex, frac);
                            }
                            break;
                        }
                        case MidiBindingTarget::AuxPluginParam:
                        {
                            // Same shape as TrackPluginParam: the loaded host
                            // owns the slot (CLAP -> LV2 -> VST3 -> JUCE).
                            if (b.targetIndex >= 0
                                && b.targetIndex < Session::kNumAuxLanes
                                && b.paramIndex >= 0)
                            {
                                auto& lane = auxLaneStrips[(size_t) b.targetIndex];
#if DUSKSTUDIO_HAS_NATIVE_CLAP
                                if (lane.isNativeClapLoaded (0))
                                {
                                    lane.getNativeClapSlot (0)
                                        .queueParamBinding ((uint32_t) b.paramIndex, frac);
                                    break;
                                }
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
                                if (lane.isNativeLv2Loaded (0))
                                {
                                    lane.getNativeLv2Slot (0)
                                        .queueParamBinding ((uint32_t) b.paramIndex, frac);
                                    break;
                                }
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
                                if (lane.isNativeVst3Loaded (0))
                                {
                                    lane.getNativeVst3Slot (0)
                                        .queueParamBinding ((uint32_t) b.paramIndex, frac);
                                    break;
                                }
#endif
                                lane.getPluginSlot (0)
                                    .setParamNormalised (b.paramIndex, frac);
                            }
                            break;
                        }
                        case MidiBindingTarget::AuxLaneFader:
                            if (b.targetIndex >= 0 && b.targetIndex < Session::kNumAuxLanes)
                            {
                                const float db = faderBindingFracToDb (
                                    frac, b.trigger == MidiBindingTrigger::PitchBend);
                                session.auxLane (b.targetIndex).params.returnLevelDb
                                    .store (db, std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::AuxLaneMute:
                            if (pressed && b.targetIndex >= 0 && b.targetIndex < Session::kNumAuxLanes)
                            {
                                auto& a = session.auxLane (b.targetIndex).params.mute;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;

                        case MidiBindingTarget::MasterFader:
                        {
                            const float db = faderBindingFracToDb (
                                frac, b.trigger == MidiBindingTrigger::PitchBend);
                            session.master().faderDb.store (db, std::memory_order_relaxed);
                            break;
                        }

                        // Per-track discrete expansion toggles. `pressed` covers
                        // both Press (rising-edge from upstream) and
                        // Toggle (every-message from upstream) button
                        // modes - the binding-decoder normalises both
                        // into a single rising edge here.
                        case MidiBindingTarget::TrackEqEnabled:
                            if (pressed && b.targetIndex >= 0
                                && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex).strip.eqEnabled;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackCompEnabled:
                            if (pressed && b.targetIndex >= 0
                                && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex).strip.compEnabled;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;
                        case MidiBindingTarget::TrackInsertBypass:
                            // Bypasses the per-channel HARDWARE insert. The
                            // channel-strip mixer routes around the
                            // hardware-insert slot when enabled=false; the
                            // plugin slot is a separate gate (bypassed
                            // through the slot's own bypassed flag, not
                            // this MIDI target).
                            if (pressed && b.targetIndex >= 0
                                && b.targetIndex < Session::kNumTracks)
                            {
                                auto& a = session.track (b.targetIndex)
                                              .hardwareInsert.enabled;
                                a.store (! a.load (std::memory_order_relaxed),
                                          std::memory_order_relaxed);
                            }
                            break;

                        case MidiBindingTarget::TrackAuxSendPrePost:
                        {
                            if (! pressed) break;
                            const int track = unpackTrackAuxTrack (b.targetIndex);
                            const int aux   = unpackTrackAuxLane  (b.targetIndex);
                            if (track < 0 || track >= Session::kNumTracks) break;
                            if (aux   < 0 || aux   >= ChannelStripParams::kNumAuxSends) break;
                            auto& a = session.track (track).strip
                                          .auxSendPreFader[(size_t) aux];
                            a.store (! a.load (std::memory_order_relaxed),
                                      std::memory_order_relaxed);
                            break;
                        }

                        // Bus EQ gain (packed bus * kBusEqBands + band)
                        case MidiBindingTarget::BusEqGain:
                        {
                            const int bus  = unpackBusEqBus  (b.targetIndex);
                            const int band = unpackBusEqBand (b.targetIndex);
                            if (bus < 0 || bus >= Session::kNumBuses) break;
                            // -9..+9 dB matches BusParams::eqLfGainDb's
                            // doc comment ("Mixbus-style, musical").
                            const float db = -9.0f + frac * 18.0f;
                            auto& strip = session.bus (bus).strip;
                            switch (band)
                            {
                                case 0: strip.eqLfGainDb .store (db, std::memory_order_relaxed); break;
                                case 1: strip.eqMidGainDb.store (db, std::memory_order_relaxed); break;
                                case 2: strip.eqHfGainDb .store (db, std::memory_order_relaxed); break;
                                default: break;
                            }
                            break;
                        }

                        // Master section (one master - targetIndex unused)
                        case MidiBindingTarget::MasterEqLfBoost:
                            // Pultec low-boost: 0..10 (per
                            // MasterBusParams::eqLfBoost doc comment).
                            session.master().eqLfBoost.store (
                                frac * 10.0f, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::MasterEqHfBoost:
                            session.master().eqHfBoost.store (
                                frac * 10.0f, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::MasterCompThresh:
                            // -38..+12 dB matches the channel-strip
                            // VCA threshold range so a learned CC feels
                            // identical when re-bound to the master.
                            session.master().compThreshDb.store (
                                -38.0f + frac * 50.0f, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::MasterCompMakeup:
                            // -20..+20 dB matches the per-track comp
                            // makeup range.
                            session.master().compMakeupDb.store (
                                -20.0f + frac * 40.0f, std::memory_order_relaxed);
                            break;
                        case MidiBindingTarget::MasterCompRatio:
                            // 1..20:1 covers gentle bus glue through
                            // brick-wall summing.
                            session.master().compRatio.store (
                                1.0f + frac * 19.0f, std::memory_order_relaxed);
                            break;

                        case MidiBindingTarget::None:
                            break;
                    }
                }
            }
        }
    }

    if ((int) mixL.size() < numSamples)
    {
        silentDims.store (((std::uint64_t) (std::uint32_t) mixL.size() << 32)
                          | (std::uint32_t) numSamples, std::memory_order_relaxed);
        silentBlocks.fetch_add (1, std::memory_order_relaxed);   // drained by diagTimer
        if (outputChannelData != nullptr)
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (auto* out = outputChannelData[ch])
                    std::memset (out, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    // Mastering stage runs an entirely different signal flow: stereo file ->
    // MasteringChain -> output. No track processing, no aux/master, no
    // recorder. We share `mixL/mixR` as scratch so we don't allocate on
    // the audio thread.
    if (stage.load (std::memory_order_acquire) == Stage::Mastering)
    {
        masteringPlayer.process (mixL.data(), mixR.data(), numSamples);
        masteringChain.processInPlace (mixL.data(), mixR.data(), numSamples);

        writeMasterMixToOutput (outputChannelData, numOutputChannels, numSamples);

        const auto sr = currentSampleRate.load (std::memory_order_relaxed);
        if (sr > 0.0)
        {
            const double bufferMs = 1000.0 * (double) numSamples / sr;
            // Multiply by cached reciprocal instead of calling
            // highResolutionTicksToSeconds (which does an internal divide
            // every call). secondsPerTick is set in prepareForSelfTest.
            const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - callbackStart)
                                         * secondsPerTick * 1000.0;
            if (elapsedMs > bufferMs)
                xrunCount.fetch_add (1, std::memory_order_relaxed);

            // Drive the same DSP-load smoother the mix path uses, so the badge
            // keeps updating in Mastering (this branch returns before the
            // shared cpuUsage update below).
            if (bufferMs > 0.0)
            {
                const float instant = (float) juce::jlimit (0.0, 2.0, elapsedMs / bufferMs);
                const float prev = cpuUsage.load (std::memory_order_relaxed);
                cpuUsage.store (prev + 0.2f * (instant - prev), std::memory_order_relaxed);
            }
        }
        return;
    }

    // SIMD-friendly clear via JUCE's FloatVectorOperations - it dispatches to
    // the platform's vector zero-store on x86 (SSE) / aarch64 (NEON) where
    // memset would still call into libc and miss the alignment-aware path.
    juce::FloatVectorOperations::clear (mixL.data(), numSamples);
    juce::FloatVectorOperations::clear (mixR.data(), numSamples);
    for (auto& v : busL) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : busR) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : auxLaneL) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : auxLaneR) juce::FloatVectorOperations::clear (v.data(), numSamples);

    // Refresh PDC compensation before the strip loop reads it. Cheap atomic
    // sweep; auto-tracks any latency change without a separate trigger.
    recomputePdc();

    // Solo automation routes into per-track liveSolo (in the strip loop below)
    // and bypasses the manual-solo RT counter anyTrackSoloed() reads - so an
    // automation-only solo would never engage the aggregate mute gate. Scan
    // liveSolo too (last block's routed value: one block stale, imperceptible
    // for a discrete solo) so the gate fires for automated solos as well.
    bool anyChannelSolo = session.anyTrackSoloed();
    if (! anyChannelSolo)
        for (int t = 0; t < Session::kNumTracks; ++t)
            if (session.track (t).strip.liveSolo.load (std::memory_order_relaxed))
            { anyChannelSolo = true; break; }
    const bool anyBusSolo     = session.anyBusSoloed();

    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {};
    std::array<float*, ChannelStrip::kNumBuses> busRPtrs {};
    for (int a = 0; a < Session::kNumBuses; ++a)
    {
        busLPtrs[(size_t) a] = busL[(size_t) a].data();
        busRPtrs[(size_t) a] = busR[(size_t) a].data();
    }
    std::array<float*, ChannelStripParams::kNumAuxSends> auxLanePtrsL {};
    std::array<float*, ChannelStripParams::kNumAuxSends> auxLanePtrsR {};
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auxLanePtrsL[(size_t) a] = auxLaneL[(size_t) a].data();
        auxLanePtrsR[(size_t) a] = auxLaneR[(size_t) a].data();
    }

    const auto state = transport.getState();
    const bool isPlaying   = (state == Transport::State::Playing);
    const bool isRecording = (state == Transport::State::Recording);
    const std::int64_t blockStartSamples = transport.getPlayhead();

    // Offline renders must be deterministic - live input never prints. An offline
    // bounce/stem/freeze drives this callback from a worker thread while the MIDI
    // input path stays open, so this latch gates every live-MIDI pull below. Read
    // once per callback (relaxed: the render worker sets it and drives this call).
    const bool offlineRender = offlineRenderActive.load (std::memory_order_relaxed);

    // Loop-aware disk reads: mirrors the wrap gate at the bottom of the
    // callback (plain playback only - recording keeps the playhead linear).
    const bool loopReadActive = isPlaying && ! isRecording && transport.isLoopEnabled()
                                 && transport.getLoopEnd() > transport.getLoopStart();
    const std::int64_t loopReadStart = loopReadActive ? transport.getLoopStart() : -1;
    const std::int64_t loopReadEnd   = loopReadActive ? transport.getLoopEnd()   : -1;

    // Hanging-note protection. Detect two events that warrant a per-MIDI-
    // track "All Notes Off" flush this block:
    //   - Transport rolling -> stopped (held notes won't get their Note
    //     Off from the region or input stream).
    //   - Playhead discontinuity while still rolling (loop wrap, scrub).
    // Both cases produce stuck synth voices without an explicit flush.
    const bool isRolling = isPlaying || isRecording;
    const bool transportJustStopped = wasRolling && ! isRolling;
    const bool playheadJumped = isRolling
                              && lastBlockEndSample != 0
                              && blockStartSamples != lastBlockEndSample;
    const bool flushHangingMidi = transportJustStopped || playheadJumped;
    wasRolling         = isRolling;
    lastBlockEndSample = blockStartSamples + numSamples;

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& trackParams = session.track (t).strip;

        // Automation routing runs FIRST so the per-strip routing decisions
        // below (passes / monitorPasses / etc.) see the automated values
        // when in Read or Touch mode. Writes liveFaderDb / livePan /
        // liveAuxSendDb / liveMute. ChannelStrip's processAndAccumulate
        // also reads these live atoms when computing per-sample gains.
        //
        // The mode atom is loaded with `acquire` so the audio thread sees
        // every prior write to the lane's points vector (made on the
        // message thread during a Write pass) before reading it - the UI
        // release-stores the new mode after appending, so the load-acquire
        // here pairs with the store-release there.
        {
            const int amode = session.track (t).automationMode.load (std::memory_order_acquire);

            // Per-param continuous routing: Read pulls from the lane
            // unconditionally; Touch pulls from the lane only while the
            // user is NOT grabbing that specific control; Off / Write
            // always pass the manual setpoint through. Per-control
            // `touched` flags so a grab on the fader doesn't release pan.
            auto routeContinuous = [&] (AutomationParam param,
                                          const std::atomic<float>& manual,
                                          const std::atomic<bool>* touched,
                                          std::atomic<float>& live)
            {
                const auto& pts = session.track (t).automationLanes[(size_t) param].pointsForRead();
                const bool readsLane =
                       amode == (int) AutomationMode::Read
                    || (amode == (int) AutomationMode::Touch
                        && touched != nullptr
                        && ! touched->load (std::memory_order_acquire));
                const float v = (readsLane && ! pts.empty())
                    ? evaluateLane (pts, blockStartSamples, param)
                    : manual.load (std::memory_order_relaxed);
                live.store (v, std::memory_order_relaxed);
            };

            routeContinuous (AutomationParam::FaderDb,
                              trackParams.faderDb, &trackParams.faderTouched,
                              trackParams.liveFaderDb);
            routeContinuous (AutomationParam::Pan,
                              trackParams.pan, &trackParams.panTouched,
                              trackParams.livePan);
            for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
            {
                const auto param = (AutomationParam) ((int) AutomationParam::AuxSend1 + i);
                routeContinuous (param,
                                  trackParams.auxSendDb[(size_t) i],
                                  &trackParams.auxSendTouched[(size_t) i],
                                  trackParams.liveAuxSendDb[(size_t) i]);
            }

            // Mute / Solo - discrete, no Touch flag. Read or Touch reads
            // lane; Off or Write reads manual. Discrete params return
            // 0.0 or 1.0 from evaluateLane (after denormalize); we
            // threshold at 0.5 to a bool. Empty lane falls through to
            // manual.
            auto routeDiscrete = [&] (AutomationParam param,
                                       const std::atomic<bool>& manual,
                                       std::atomic<bool>& live)
            {
                const auto& pts = session.track (t).automationLanes[(size_t) param].pointsForRead();
                const bool readsLane =
                       amode == (int) AutomationMode::Read
                    || amode == (int) AutomationMode::Touch;
                const bool effective = (readsLane && ! pts.empty())
                    ? (evaluateLane (pts, blockStartSamples, param) >= 0.5f)
                    : manual.load (std::memory_order_relaxed);
                live.store (effective, std::memory_order_relaxed);
            };
            routeDiscrete (AutomationParam::Mute, trackParams.mute, trackParams.liveMute);
            routeDiscrete (AutomationParam::Solo, trackParams.solo, trackParams.liveSolo);
        }

        // Reads liveMute / liveSolo - just-routed by the block above, so
        // the strip's passes / monitorPasses calculation sees automated
        // mute and solo state.
        const bool muted   = trackParams.liveMute.load (std::memory_order_relaxed);
        const bool soloed  = trackParams.liveSolo.load (std::memory_order_relaxed);
        const bool armed   = session.track (t).recordArmed.load (std::memory_order_relaxed);
        const bool monitorEnabled = session.track (t).inputMonitor.load (std::memory_order_relaxed);
        const bool midiTrack = session.track (t).mode.load (std::memory_order_relaxed)
                                   == (int) Track::Mode::Midi;
        // Frozen: this track plays a pre-rendered WAV through the strip with the
        // instrument/insert + EQ/comp bypassed (baked in). Works for MIDI and
        // audio tracks alike. Acquire pairs with the release store in
        // commitFreeze so the source switch + the frozen-strip path observe a
        // consistent frozenRegion. A frozen track's mode is locked, so the flag
        // can't strand on a mode that didn't bake the WAV.
        const bool isFrozen = session.track (t).frozen.load (std::memory_order_acquire);

        // The track will read from disk during Play, or during Record on
        // un-armed tracks (so other tracks keep playing while we record into
        // an armed one). Otherwise the strip's source is live device input.
        // A frozen track reads its baked WAV during Play/Record even if armed
        // (frozen playback is the render, never live input) - but it stays silent
        // when stopped, like any other disk track.
        const bool willReadFromDisk = isPlaying || (isRecording && (isFrozen || ! armed));

        // Live-input monitor gate. IN is the ONLY control over live-input
        // monitoring. ARM is purely a recorder-enable; it does NOT gate
        // monitor passthrough. Safety against feedback comes from IN
        // defaulting to false - the user must explicitly click IN to
        // open the live-input -> master path. Disk playback always passes
        // (independent of IN).
        // A frozen track plays its baked WAV only - never live input. When stopped it
        // stays silent instead of monitoring the device through a frozen (bypassed) strip.
        const bool monitorPasses = willReadFromDisk ? true : (monitorEnabled && ! isFrozen);

        // SIP-style bus solo (matches Pro Tools / Logic / Cubase): when any
        // bus is soloed, a track is audible ONLY if it feeds a soloed bus.
        // Tracks routed solely to non-soloed buses - and direct-to-master
        // (unassigned) tracks like a guitar with no bus - are muted entirely,
        // so their dry, bus, AND aux sends all stop. Aux RETURNS stay
        // solo-safe (summed unconditionally further down). Without this, an
        // unassigned track bypassed the bus-solo gate and leaked to master.
        bool busSoloPasses = true;
        if (anyBusSolo)
        {
            busSoloPasses = false;
            for (int a = 0; a < Session::kNumBuses; ++a)
                if (trackParams.busAssign[(size_t) a].load (std::memory_order_relaxed)
                    && session.bus (a).strip.solo.load (std::memory_order_relaxed))
                { busSoloPasses = true; break; }
        }

        const bool passes = ! muted && (anyChannelSolo ? soloed : true)
                          && monitorPasses && busSoloPasses;

        // Resolve the input source for this track.
        const int inputIdx = session.resolveInputForTrack (t);
        const float* deviceInput = (inputIdx >= 0 && inputIdx < numInputChannels)
                                    ? inputChannelData[(size_t) inputIdx] : nullptr;

        // Tuner: when this track is the selected target, feed the device
        // input to the YIN-style PitchDetector. Allocation-free; the
        // detector publishes the latest Hz / level into Session atoms
        // that the TunerOverlay polls at 30 Hz on the message thread.
        // Inactive when tuneTrackIndex < 0.
        if (deviceInput != nullptr
            && session.tuneTrackIndex.load (std::memory_order_relaxed) == t)
        {
            pitchDetector.pushBlock (deviceInput, numSamples);
            session.tuneLatestHz   .store (pitchDetector.getLatestHz(),    std::memory_order_relaxed);
            session.tuneLatestLevel.store (pitchDetector.getLatestLevel(), std::memory_order_relaxed);
        }

        // Choose the source the channel strip will process.
        //   - Playing & not armed: read previous take from disk (un-armed
        //     playback tracks).
        //   - Playing & armed: ALSO read previous take from disk. Standard
        //     DAW behavior - stopping a record pass leaves the track armed
        //     for the next take, but Play should still reproduce what was
        //     recorded. Without this, hitting Play after a stop produced
        //     silence on the just-recorded track (the live input branch
        //     fired instead of disk read).
        //   - Recording & armed: live input feeds the strip; the recorder
        //     captures via writeInputBlock below. The IN toggle gates
        //     master accumulation (passByGate / monitorPasses), not whether
        //     the strip processes.
        //   - Stopped & armed (or stopped + IN on, un-armed): live input
        //     for monitoring.
        //   - Stopped & un-armed & IN off: monoIn null -> strip is silent.
        const float* monoIn = nullptr;
        const bool stereoTrackInput = session.track (t).mode.load (std::memory_order_relaxed)
                                          == (int) Track::Mode::Stereo;

        if (willReadFromDisk)
        {
            // Stereo tracks read both channels from disk; mono tracks pass
            // nullptr for outR which makes readForTrack skip the second
            // channel entirely. A frozen track's baked WAV is always stereo.
            float* outR = (stereoTrackInput || isFrozen) ? playbackScratchR[(size_t) t].data() : nullptr;
            playbackEngine.readForTrack (t, blockStartSamples,
                                          playbackScratch[(size_t) t].data(), outR,
                                          numSamples, loopReadStart, loopReadEnd);
            monoIn = playbackScratch[(size_t) t].data();
        }
        else if (deviceInput != nullptr && (armed || monitorEnabled) && ! isFrozen)
        {
            monoIn = deviceInput;
        }
        else if (! isFrozen
                 && (strips[(size_t) t].getPluginSlot().isLoaded()
                     || session.track (t).hardwareInsert.pingPending.load (std::memory_order_acquire)))
        {
            // Generator-style insert with no input source: feed a
            // pre-zeroed buffer so the chain runs and the plugin can
            // emit its output. Costs one extra strip per loaded-but-
            // unfed channel; effects feeding zero just produce zero.
            // Same feed while a hardware-insert ping is in flight -
            // with transport stopped and IN off the track has no
            // source, and the slot needs to be serviced to run the
            // chirp + capture.
            monoIn = silentInputScratch.data();
        }

        // Input meter - armed tracks always show live input (so the user can
        // set gain for tracking even with monitor muted). Un-armed tracks
        // show their playback signal during transport, or live input when IN
        // is engaged. With IN off and no transport, the meter reads silence
        // even though the strip's DSP is still running on deviceInput (so a
        // PRINT-mode recorder can capture post-effects audio).
        const float* meterSrc = nullptr;
        if (armed && deviceInput != nullptr)
            meterSrc = deviceInput;
        else if ((isPlaying || isRecording) && ! armed)
            meterSrc = monoIn;
        else if (monitorEnabled && deviceInput != nullptr)
            meterSrc = deviceInput;

        // SIMD'd absolute-peak via findMinAndMax then |min| vs max - one
        // vector pass over the buffer instead of a scalar abs-and-compare
        // loop per sample. Same numeric result, much faster on long blocks.
        float inputPeak = 0.0f;
        if (meterSrc != nullptr && numSamples > 0)
        {
            const auto rng = juce::FloatVectorOperations::findMinAndMax (meterSrc, numSamples);
            inputPeak = juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
        }
        const float inputDb = (inputPeak > 1e-5f)
                              ? juce::Decibels::gainToDecibels (inputPeak, -100.0f)
                              : -100.0f;
        session.track (t).meterInputDb.store (inputDb, std::memory_order_relaxed);

        // R-channel input meter for stereo tracks. Phase 1 only renders the
        // peak; the strip's audio path is still mono until Phase 2 wires in
        // stereo recording. Mono / Midi tracks get -100 so the UI can fall
        // back to a single LED bar without gating on mode.
        float inputRDb = -100.0f;
        const int rIdx = session.resolveInputRForTrack (t);
        if (rIdx >= 0 && rIdx < numInputChannels)
        {
            const float* deviceInputR = inputChannelData[(size_t) rIdx];
            if (deviceInputR != nullptr && (armed || monitorEnabled) && numSamples > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (deviceInputR, numSamples);
                const float peak = juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
                if (peak > 1e-5f)
                    inputRDb = juce::Decibels::gainToDecibels (peak, -100.0f);
            }
        }
        session.track (t).meterInputRDb.store (inputRDb, std::memory_order_relaxed);

        // MIDI input filter - pull events from the track's selected input
        // Hanging-note flush. Emitted FIRST (sample 0) so the synth
        // releases any held voices before we add this block's new events
        // on top. CC 64 = sustain pedal off, CC 123 = all notes off.
        // We hit all 16 channels because we don't track which channels
        // had active notes - cheap brute-force is fine, the synth ignores
        // the redundant channels in the same processBlock pass.
        // Skipped on non-MIDI tracks; perTrackMidiScratch on those tracks
        // stays an empty buffer for effect inserts.
        //
        // Three triggers warrant a flush, all OR'd together:
        //   - engine-wide flushHangingMidi (transport stop / playhead jump)
        //   - this track's midiInputIndex changed since last block (the
        //     user swapped MIDI controllers - held notes from the old
        //     device would otherwise hang on the synth forever).
        const int currentMidiIdx = session.track (t).midiInputIndex.load (
                                       std::memory_order_relaxed);
        const bool midiInputSwapped = (currentMidiIdx != lastMidiInputIndex[(size_t) t]);
        lastMidiInputIndex[(size_t) t] = currentMidiIdx;
        const bool perTrackFlush = flushHangingMidi || midiInputSwapped;

        perTrackMidiScratch[(size_t) t].clear();
        if (midiTrack && perTrackFlush)
        {
            for (int ch = 1; ch <= 16; ++ch)
            {
                perTrackMidiScratch[(size_t) t].addEvent (
                    juce::MidiMessage::controllerEvent (ch, 64,  0), 0);
                perTrackMidiScratch[(size_t) t].addEvent (
                    juce::MidiMessage::controllerEvent (ch, 123, 0), 0);
            }
        }

        // Build this block's per-track MIDI buffer. Two source paths,
        // mutually exclusive (matches the audio source decision above):
        //   - Disk playback (willReadFromDisk): walk the track's
        //     midiRegions and emit any note/CC events whose absolute
        //     sample-position falls inside this block. This is the
        //     scheduled-playback path - the synth hears notes that were
        //     previously recorded onto the timeline.
        //   - Live monitoring (else): pull from the input MIDI collector
        //     and apply the per-track channel filter.
        // The strip's instrument plugin then sees one unified buffer.
        // Note: scratch already cleared above by the flush block, plus any
        // emitted All Notes Off events. Both source paths add to those.
        // Live-input MIDI pull. Shared by the stopped/record live-monitor
        // branch and the play-along overlay in the disk-playback branch:
        // applies the per-track channel filter, mirrors the dusk->juce raw-byte
        // copy that feeds the instrument + recorder, and - on an armed track -
        // always auditions the on-screen keyboard. Only meaningful on MIDI
        // tracks; harmless (no matching input events consumed) otherwise.
        auto pullInput = [&] (int srcIdx, int chFilter)
        {
            if (srcIdx < 0 || srcIdx >= (int) perInputMidi.size()) return;
            if (perInputMidi[(size_t) srcIdx].isEmpty()) return;
            for (const auto meta : perInputMidi[(size_t) srcIdx])
            {
                // dusk (perInputMidi) -> juce (perTrackMidiScratch, feeds
                // the instrument + recorder): raw-byte copy, no message
                // object. Channel read mirrors the juce getChannel
                // convention: 1..16 for channel messages, 0 for system.
                const auto& v = meta.getMessage();
                if (chFilter != 0)
                {
                    const std::uint8_t status = v.getRawDataSize() > 0 ? v.getRawData()[0] : 0;
                    const int ch = (status & 0xF0) != 0xF0 ? (int) (status & 0x0F) + 1 : 0;
                    if (ch != chFilter) continue;
                }
                perTrackMidiScratch[(size_t) t].addEvent (v.getRawData(),
                                                          v.getRawDataSize(),
                                                          meta.samplePosition);
            }
        };
        auto pullLiveMidi = [&] ()
        {
            // Channel filter loaded once per pull, so only tracks that actually
            // monitor pay the read (0 = omni).
            const int chFilter = session.track (t).midiChannel.load (std::memory_order_relaxed);

            // The track's explicitly-selected MIDI input. Reuse currentMidiIdx
            // (loaded above for the hanging-note flush) rather than re-loading:
            // a message-thread device swap between the two loads would otherwise
            // pull from the new device here while the flush targeted the old one.
            pullInput (currentMidiIdx, chFilter);

            // A record-armed MIDI track ALWAYS auditions the on-screen
            // keyboard, even when its explicit input isn't the VK. This
            // makes the keyboard "just work" no matter how the instrument
            // was loaded (editor Browse, session restore, picker) - the
            // user expects an armed instrument track to play from the
            // virtual keyboard. Skip when the explicit input already IS
            // the VK so events aren't doubled.
            const bool trackArmed = session.track (t).recordArmed.load (std::memory_order_relaxed);
            const int vkbIdx = midiIn.getVirtualKeyboardIndex();
            if (trackArmed && vkbIdx != currentMidiIdx)
                pullInput (vkbIdx, chFilter);
        };

        if (willReadFromDisk && ! isFrozen)
        {
            // Frozen tracks skip MIDI scheduling - the instrument is bypassed
            // and the notes are already baked into the WAV being read above.
            // Acquire on tempoBpm pairs with the release store in
            // duskstudio::applyTempoChange so this MIDI scheduling block
            // observes the BPM that matches the published-with-release
            // MidiRegion sample positions. Without acquire here, a stale
            // BPM combined with new region positions could schedule notes
            // at the wrong tick->sample translation for one audio block
            // following a BPM change.
            const float bpm = session.tempoBpm.load (std::memory_order_acquire);
            const double sr = currentSampleRate.load (std::memory_order_relaxed);
            // Lock-free tempo-map snapshot. When non-empty, note/CC ticks resolve
            // to absolute timeline samples through the map (so playback follows
            // tempo changes); when null/empty this is the exact constant-bpm path.
            const TempoMap* tm = rtTempoMap.load (std::memory_order_acquire);
            const bool useMap = (tm != nullptr && ! tm->empty());
            // Plugin latency comp for instrument tracks: shift the scheduling
            // window forward by the plugin's reported latency so the
            // delayed audio output aligns to the correct timeline sample.
            // 0 latency = no shift (the common case for synths). Audio
            // tracks have no instrument plugin so latency is 0 even when
            // the slot is loaded with an effect.
            const std::int64_t pluginLatency = midiTrack
                ? (std::int64_t) strips[(size_t) t].getPluginSlot().getLatencySamples()
                : 0;
            const auto schedStart = blockStartSamples + pluginLatency;
            const auto blockEnd   = schedStart + numSamples;

            // Acquire-load the track's MIDI region snapshot once for the
            // block. Mutated on the message thread by RecordManager (when
            // a take finishes) and SessionSerializer (load); the snapshot
            // pointer is stable for the rest of this callback.
            // AtomicSnapshot's default ctor publishes an empty vector at
            // construction time so this pointer is non-null.
            const auto& midiRegionsForBlock = *session.track (t).midiRegions.read();

            // Chase pass: when the playhead jumps INTO the middle of a
            // sustained note (transport start after seek, loop wrap, etc.)
            // the synth would otherwise sit silent until the Note Off fires
            // since the Note On is in the past. Emit Note On at sample 1
            // (one sample after the All Notes Off the flush block already
            // emitted at sample 0, so the synth doesn't immediately silence
            // the chase) for every note whose on-time is before blockStart
            // and off-time is after blockStart.
            if (flushHangingMidi)
            {
                for (const auto& region : midiRegionsForBlock)
                {
                    if (region.muted) continue;
                    const auto regStart = region.timelineStart;
                    const auto regStartTick = useMap ? tm->samplesToTicks (regStart, sr) : (std::int64_t) 0;
                    auto absOf = [&] (std::int64_t relTick)
                    {
                        return useMap ? tm->ticksToSamples (regStartTick + relTick, sr)
                                      : regStart + ticksToSamples (relTick, sr, bpm);
                    };
                    for (const auto& n : region.notes)
                    {
                        const auto onAbs  = absOf (n.startTick);
                        const auto offAbs = absOf (n.startTick + n.lengthInTicks);
                        if (onAbs < blockStartSamples && offAbs > blockStartSamples)
                        {
                            perTrackMidiScratch[(size_t) t].addEvent (
                                juce::MidiMessage::noteOn (n.channel, n.noteNumber,
                                                            (std::uint8_t) n.velocity),
                                1);
                        }
                    }
                }
            }

            for (const auto& region : midiRegionsForBlock)
            {
                if (region.muted) continue;
                const auto regStart = region.timelineStart;
                const auto regStartTick = useMap ? tm->samplesToTicks (regStart, sr) : (std::int64_t) 0;
                auto absOf = [&] (std::int64_t relTick)
                {
                    return useMap ? tm->ticksToSamples (regStartTick + relTick, sr)
                                  : regStart + ticksToSamples (relTick, sr, bpm);
                };
                // Musical region end (lengthInTicks is the source of truth) so a
                // region that got longer in samples under a tempo slowdown isn't
                // skipped by the overlap test.
                const auto regEnd = useMap ? absOf (region.lengthInTicks)
                                           : regStart + region.lengthInSamples;
                // Skip regions that don't overlap the SHIFTED block at all.
                // schedStart/blockEnd already include the plugin-latency
                // offset for MIDI tracks; on audio tracks pluginLatency=0
                // so this collapses to the original [blockStart, blockEnd)
                // window.
                if (regEnd <= schedStart || regStart >= blockEnd) continue;

                for (const auto& n : region.notes)
                {
                    const auto onAbs  = absOf (n.startTick);
                    const auto offAbs = absOf (n.startTick + n.lengthInTicks);
                    if (onAbs >= schedStart && onAbs < blockEnd)
                    {
                        perTrackMidiScratch[(size_t) t].addEvent (
                            juce::MidiMessage::noteOn (n.channel, n.noteNumber,
                                                        (std::uint8_t) n.velocity),
                            (int) (onAbs - schedStart));
                    }
                    if (offAbs >= schedStart && offAbs < blockEnd)
                    {
                        perTrackMidiScratch[(size_t) t].addEvent (
                            juce::MidiMessage::noteOff (n.channel, n.noteNumber),
                            (int) (offAbs - schedStart));
                    }
                }
                for (const auto& c : region.ccs)
                {
                    const auto sAbs = absOf (c.atTick);
                    if (sAbs >= schedStart && sAbs < blockEnd)
                    {
                        perTrackMidiScratch[(size_t) t].addEvent (
                            juce::MidiMessage::controllerEvent (c.channel,
                                                                  c.controller,
                                                                  c.value),
                            (int) (sAbs - schedStart));
                    }
                }
            }

            // Play-along overlay. Live-MIDI delivery is a PER-BRANCH obligation:
            // every transport-source branch that builds perTrackMidiScratch must
            // pull live input itself, or play-along goes silent in that state.
            // The same IN control is checked through two different audibility
            // gates depending on the branch:
            //   - stopped/record (the else branch below): monitorPasses is the
            //     master-accumulation gate, and it already folds in IN.
            //   - playing (here): monitorPasses is forced open during playback,
            //     so IN has to be checked explicitly instead.
            // A future third source branch must likewise pull live input under
            // whichever gate governs its audibility. With IN engaged the live
            // controller (and, on an armed track, the on-screen keyboard) merges
            // ON TOP of the scheduled timeline notes so the instrument sounds
            // both the recorded part and what the user plays while the transport
            // rolls. Overlay events are additive, so the recorder (armed +
            // Recording only, below) and scheduled playback are untouched.
            if (midiTrack && monitorEnabled && ! offlineRender)
                pullLiveMidi();
        }
        else if (midiTrack && ! offlineRender)
        {
            // Live monitoring path - only meaningful on MIDI tracks; effect
            // inserts on Mono / Stereo strips don't consume per-track MIDI
            // (their pluginMidiScratch is built fresh inside the strip).
            // Gated off during offline render: a frozen MIDI track reaches this
            // branch under an offline drive (transport is Playing, so
            // willReadFromDisk holds, but the ! isFrozen guard on the disk branch
            // fails), and live input must never print into a deterministic file.
            pullLiveMidi();
        }

        if (midiTrack && ! perTrackMidiScratch[(size_t) t].isEmpty())
            session.track (t).midiActivity.store (true, std::memory_order_relaxed);

        // Recording capture has to happen BEFORE the strip's instrument
        // plugin sees the buffer. AudioPluginInstance::processBlock is
        // allowed (and most VST3 instruments do) to consume / replace the
        // MIDI buffer in place, so by the time processAndAccumulate
        // returns, perTrackMidiScratch is typically empty. Writing here
        // captures the events the user actually played.
        if (isRecording && armed && midiTrack && ! perTrackMidiScratch[(size_t) t].isEmpty())
        {
            const auto recStart = activeRecordStart.load (std::memory_order_relaxed);
            const auto blockOffsetFromRecord = blockStartSamples - recStart;
            recordManager.writeMidiBlock (t, perTrackMidiScratch[(size_t) t], blockOffsetFromRecord);
        }

        // External MIDI output. When this MIDI track has a hardware port
        // selected, mirror the just-built per-track buffer to that port
        // so an external synth/drum machine receives the same notes the
        // loaded instrument plugin (if any) does. Handed to the pump thread
        // via the out-bank's lock-free RT queue, which validates the port
        // under its mutex. Empty buffer skipped to avoid pointless slot use.
        if (midiTrack && ! perTrackMidiScratch[(size_t) t].isEmpty())
        {
            const int outIdx = session.track (t).midiOutputIndex.load (
                                  std::memory_order_relaxed);
            if (outIdx >= 0)
            {
                // Bridge the juce per-track buffer into the dusk out-scratch
                // (pre-reserved) for queueRt's dusk boundary. Whole-block or
                // nothing: a partial copy on cap overflow could split paired
                // events (note on/off), so mirror the pre-flip whole-block
                // drop semantics.
                midiOutTrackScratch.clear();
                bool fits = true;
                for (const auto meta : perTrackMidiScratch[(size_t) t])
                    if (! midiOutTrackScratch.addEvent (meta.data, meta.numBytes,
                                                        meta.samplePosition))
                    { fits = false; break; }
                if (fits)
                    midiOut.queueRt (outIdx, midiOutTrackScratch,
                                     currentSampleRate.load (std::memory_order_relaxed));
            }
        }

        // Tell the strip whether the recorder is going to ask for the
        // post-effects buffer this block - if so, the strip MUST run its
        // DSP even when it's not passing to master. Otherwise we let it
        // skip the heavy pass to save CPU on silent tracks.
        const bool needPrintBuffer = isRecording && armed && deviceInput != nullptr
                                  && session.track (t).printEffects.load (std::memory_order_relaxed);
        strips[(size_t) t].setNeedsProcessedMono (needPrintBuffer);

        // Stereo input source for stereo tracks. Two paths:
        //   - Disk playback: PlaybackEngine wrote the R channel into
        //     playbackScratchR above (when stereoTrackInput is true), so
        //     point monoInR at that buffer.
        //   - Live input: source from the user's R input mapping.
        const float* monoInR = nullptr;
        if ((stereoTrackInput || isFrozen) && monoIn != nullptr)
        {
            if (willReadFromDisk)
            {
                monoInR = playbackScratchR[(size_t) t].data();
            }
            else if (monoIn == silentInputScratch.data())
            {
                // Generator-style insert on a stereo track: feed a silent
                // R buffer so ChannelStrip enters the stereo path and the
                // donor plugin's stereo output is preserved end-to-end.
                monoInR = silentInputScratch.data();
            }
            else
            {
                const int rIdxStrip = session.resolveInputRForTrack (t);
                if (rIdxStrip >= 0 && rIdxStrip < numInputChannels)
                    monoInR = inputChannelData[(size_t) rIdxStrip];
            }
        }

        // MIDI tracks have no audio input - their gate is just mute/solo.
        // Audio tracks (and frozen tracks, which read a WAV) require a non-null
        // source to pass.
        const bool stripPasses = (midiTrack && ! isFrozen) ? passes
                                                           : (passes && monoIn != nullptr);

        // A frozen track runs the strip's frozen path: stereo source from the
        // baked WAV (monoIn/monoInR), instrument + EQ/comp skipped. isMidi is
        // therefore false (the plugin path is bypassed); the frozen flag drives
        // the strip. stereoInput true so the R channel is recognised.
        trackJobs[(size_t) t] = { monoIn, monoInR, deviceInput,
                                  midiTrack && ! isFrozen, stripPasses, armed,
                                  stereoTrackInput || isFrozen, isFrozen,
                                  strips[(size_t) t].insertMode.load (std::memory_order_relaxed)
                                      == ChannelStrip::kInsertHardware };
    }

    // DSP pass
    // Heavy per-strip DSP (the only thing that fans out). Serial path
    // accumulates straight into mixL/busL/auxLaneL; the opt-in worker pool
    // instead has each lane accumulate its strip subset into its own buffer
    // set, then a serial reduce sums the lanes in. Metering + recording run on
    // the serial tail below - the audio thread, after all strips are done -
    // reading each strip's just-produced output, so worker threads only ever
    // run pure DSP. (The parallel reduce regroups the float sum, so the master
    // is bit-identical only in serial mode; the parallel difference is a
    // deterministic ~1e-7, far below audibility.)
    perfLap (PerfSections::kPre);

    if (workerPool.isActive())
    {
        currentBlockSamples = numSamples;
        workerPool.runBlock();
        reduceLaneAccum (numSamples);
        // Hardware-insert strips were skipped in the lanes (processStripLane)
        // because their send writes the shared device-output buffer; sum them
        // here on the audio thread, where that write is single-threaded.
        for (int t = 0; t < Session::kNumTracks; ++t)
            if (trackJobs[(size_t) t].hardwareInsert)
                accumulateStrip (t, mixL.data(), mixR.data(),
                                 busLPtrs, busRPtrs, auxLanePtrsL, auxLanePtrsR, numSamples);
    }
    else
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
            accumulateStrip (t, mixL.data(), mixR.data(),
                             busLPtrs, busRPtrs, auxLanePtrsL, auxLanePtrsR, numSamples);
    }

    perfLap (PerfSections::kStrips);

    // Post-DSP serial tail: metering + recording (audio thread).
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& job = trackJobs[(size_t) t];
        session.track (t).meterGrDb.store (strips[(size_t) t].getCurrentGrDb(),
                                            std::memory_order_relaxed);
        session.track (t).meterOutLDb.store (strips[(size_t) t].getOutLDb(),
                                              std::memory_order_relaxed);
        session.track (t).meterOutRDb.store (strips[(size_t) t].getOutRDb(),
                                              std::memory_order_relaxed);

        // Output peak for MIDI tracks: the strip's "input" meter only
        // measures the audio source feeding the strip, which is null on
        // a MIDI track since the audio is generated INSIDE the strip by
        // the instrument plugin. Without this branch the strip meter
        // stays flat even while the synth is audible at master. Write
        // the post-DSP peak (same buffer the recorder uses for printed-
        // FX captures) into the input-meter atom so the UI's existing
        // poll renders a real level for MIDI tracks.
        if (job.isMidi)
        {
            const int n = strips[(size_t) t].getLastProcessedSamples();
            if (auto* lp = strips[(size_t) t].getLastProcessedMono(); lp != nullptr && n > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (lp, n);
                const float pk = juce::jmax (std::abs (rng.getStart()),
                                              std::abs (rng.getEnd()));
                session.track (t).meterInputDb.store (
                    pk > 1e-5f ? juce::Decibels::gainToDecibels (pk, -100.0f) : -100.0f,
                    std::memory_order_relaxed);
            }
            if (auto* rp = strips[(size_t) t].getLastProcessedR(); rp != nullptr && n > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (rp, n);
                const float pk = juce::jmax (std::abs (rng.getStart()),
                                              std::abs (rng.getEnd()));
                session.track (t).meterInputRDb.store (
                    pk > 1e-5f ? juce::Decibels::gainToDecibels (pk, -100.0f) : -100.0f,
                    std::memory_order_relaxed);
            }
        }

        // Recording capture - armed tracks always commit their input to
        // disk while recording. By default the raw deviceInput is written;
        // when `printEffects` is engaged, the post-EQ/post-comp buffer that
        // the strip just produced is written instead, "printing" the
        // channel-strip processing to the WAV.
        //
        // Punch: when transport.isPunchEnabled(), only samples whose timeline
        // position lies inside [punchIn, punchOut) are committed. The block
        // is sliced to the intersection of [blockStart, blockEnd) and the
        // punch window; outside samples are silently discarded (NOT written
        // as zeros, so the WAV's frame count equals the punch window length).
        if (isRecording && job.armed && job.deviceInput != nullptr)
        {
            const bool printEfx = session.track (t).printEffects.load (std::memory_order_relaxed);

            // Default sources: deviceInput on L; deviceInputR on R for stereo
            // tracks (resolved earlier as monoInR). printEffects swaps both
            // L and R to the strip's post-effect buffers when available.
            const float* recL = job.deviceInput;
            const float* recR = job.stereoInput ? job.monoInR : nullptr;
            if (printEfx)
            {
                auto& strip = strips[(size_t) t];
                if (auto* processed = strip.getLastProcessedMono();
                    processed != nullptr
                    && strip.getLastProcessedSamples() >= numSamples)
                {
                    recL = processed;
                    if (job.stereoInput)
                        recR = strip.getLastProcessedR();  // may be nullptr -> recorder duplicates L
                }
            }

            // Unified write-gate - honours BOTH:
            //   - activeRecordStart  (count-in pre-roll: skip writes until
            //     the playhead reaches the take's intended start)
            //   - punch-in / punch-out  (only commit samples in the window
            //     when punch is on)
            // Both reduce to a single [from, to) intersection with the
            // current block.
            const auto recStart  = activeRecordStart.load (std::memory_order_relaxed);
            std::int64_t effIn    = recStart;  // floor; never write before this
            std::int64_t effOut   = std::numeric_limits<std::int64_t>::max();
            if (transport.isPunchEnabled())
            {
                const auto pIn  = transport.getPunchIn();
                const auto pOut = transport.getPunchOut();
                if (pOut > pIn)
                {
                    effIn  = juce::jmax (effIn, pIn);
                    effOut = pOut;
                }
                else
                {
                    effIn = effOut;  // empty/inverted punch window -> no capture
                }
            }

            const auto blockEnd = blockStartSamples + numSamples;
            const auto sliceStart = juce::jmax (blockStartSamples, effIn);
            const auto sliceEnd   = juce::jmin (blockEnd,        effOut);
            if (sliceEnd > sliceStart)
            {
                const int writeOffset = (int) (sliceStart - blockStartSamples);
                const int writeLength = (int) (sliceEnd - sliceStart);
                const float* writeL = recL + writeOffset;
                const float* writeR = (recR != nullptr) ? recR + writeOffset : nullptr;
                recordManager.writeInputBlock (t, writeL, writeR, writeLength);
            }
        }

    }

    perfLap (PerfSections::kMeterRecordTail);

    for (int a = 0; a < Session::kNumBuses; ++a)
    {
        const auto& params = session.bus (a).strip;

        // Bus automation routing - mirror of the per-track / per-aux blocks.
        // Drive liveFaderDb / livePan / liveMute from the lane in Read or
        // (Touch && !touched) mode; pass the manual setpoint through
        // otherwise. BusStrip::updateGainTargets reads liveFaderDb / livePan;
        // the mute gate below reads liveMute. Solo isn't automated.
        {
            const int amode = params.automationMode.load (std::memory_order_acquire);
            {
                const auto& pts = params.automationLanes[(size_t) AutomationParam::FaderDb].pointsForRead();
                const bool touched = params.faderTouched.load (std::memory_order_acquire);
                const bool readsLane = amode == (int) AutomationMode::Read
                                     || (amode == (int) AutomationMode::Touch && ! touched);
                const float v = (readsLane && ! pts.empty())
                    ? evaluateLane (pts, blockStartSamples, AutomationParam::FaderDb)
                    : params.faderDb.load (std::memory_order_relaxed);
                params.liveFaderDb.store (v, std::memory_order_relaxed);
            }
            {
                const auto& pts = params.automationLanes[(size_t) AutomationParam::Pan].pointsForRead();
                const bool touched = params.panTouched.load (std::memory_order_acquire);
                const bool readsLane = amode == (int) AutomationMode::Read
                                     || (amode == (int) AutomationMode::Touch && ! touched);
                const float v = (readsLane && ! pts.empty())
                    ? evaluateLane (pts, blockStartSamples, AutomationParam::Pan)
                    : params.pan.load (std::memory_order_relaxed);
                params.livePan.store (v, std::memory_order_relaxed);
            }
            {
                const auto& pts = params.automationLanes[(size_t) AutomationParam::Mute].pointsForRead();
                const bool readsLane = amode == (int) AutomationMode::Read
                                     || amode == (int) AutomationMode::Touch;
                const bool effective = (readsLane && ! pts.empty())
                    ? (evaluateLane (pts, blockStartSamples, AutomationParam::Mute) >= 0.5f)
                    : params.mute.load (std::memory_order_relaxed);
                params.liveMute.store (effective, std::memory_order_relaxed);
            }
        }

        const bool muted   = params.liveMute.load (std::memory_order_relaxed);
        const bool soloed  = params.solo.load (std::memory_order_relaxed);
        const bool passes  = ! muted && (anyBusSolo ? soloed : true);

        if (! passes) continue;

        // Skip aux DSP entirely when the bus buffer is silent - no channel
        // routed to it AND no smoothing tail from a recently-unassigned
        // channel. UniversalCompressor (Bus mode) is one of the heaviest
        // per-block ops in the engine; skipping the whole EQ + comp pass on
        // an idle aux is a big saving when only 1-2 of the 4 buses are in
        // use. Aux's own fader/pan smoothers don't tick during the skip,
        // but they pick up correctly from their last-known current value
        // when audio resumes, so no click on re-engage.
        {
            const auto rngL = juce::FloatVectorOperations::findMinAndMax (
                                  busL[(size_t) a].data(), numSamples);
            const auto rngR = juce::FloatVectorOperations::findMinAndMax (
                                  busR[(size_t) a].data(), numSamples);
            const float peak = juce::jmax (
                juce::jmax (std::abs (rngL.getStart()), std::abs (rngL.getEnd())),
                juce::jmax (std::abs (rngR.getStart()), std::abs (rngR.getEnd())));
            if (peak <= 1e-6f)
            {
                // Bus pass is being skipped (no audio routed). Reset the
                // post-bus meter to silence so the UI doesn't keep showing
                // the last-played level - otherwise the LED freezes at
                // whatever value the meter held right before the skip
                // engaged.
                params.meterPostBusLDb.store (-100.0f, std::memory_order_relaxed);
                params.meterPostBusRDb.store (-100.0f, std::memory_order_relaxed);
                params.meterPostBusRmsL.store (0.0f, std::memory_order_relaxed);
                params.meterPostBusRmsR.store (0.0f, std::memory_order_relaxed);
                continue;
            }
        }

        busStrips[(size_t) a].processInPlace (busL[(size_t) a].data(),
                                              busR[(size_t) a].data(),
                                              numSamples);
        auto* capBusL = stemBusCapL[(size_t) a].load (std::memory_order_acquire);
        auto* capBusR = stemBusCapR[(size_t) a].load (std::memory_order_relaxed);
        if (capBusL != nullptr && capBusR != nullptr)
        {
            juce::FloatVectorOperations::add (capBusL,
                                                busL[(size_t) a].data(), numSamples);
            juce::FloatVectorOperations::add (capBusR,
                                                busR[(size_t) a].data(), numSamples);
        }
        // SIMD'd mix accumulate - hot inner loop, runs once per active aux
        // per callback. JUCE picks the right SSE/NEON path based on the
        // platform; cheaper than scalar [i]+= even with -O3.
        juce::FloatVectorOperations::add (mixL.data(),
                                            busL[(size_t) a].data(),
                                            numSamples);
        juce::FloatVectorOperations::add (mixR.data(),
                                            busR[(size_t) a].data(),
                                            numSamples);
    }

    perfLap (PerfSections::kBuses);

    // Master-stage PDC (see recomputePdc): the dry mix - tracks + buses, all
    // summed by here - waits for the deepest aux-lane latency so the wet
    // returns added below land aligned. Retarget only while stopped; a
    // mid-roll delay-length change on the full mix would click.
    {
        const int dryTarget = masterDryPdcTarget.load (std::memory_order_relaxed);
        if (! (isPlaying || isRecording))
        {
            if (dryTarget != masterDryPdcApplied)
            {
                masterDryPdcApplied = dryTarget;
                masterDryPdcL.reset();
                masterDryPdcR.reset();
                masterDryPdcL.setDelay ((float) dryTarget);
                masterDryPdcR.setDelay ((float) dryTarget);
            }
            for (int a = 0; a < Session::kNumAuxLanes; ++a)
            {
                const int t = auxReturnPdcTarget[(size_t) a].load (std::memory_order_relaxed);
                if (t != auxReturnPdcApplied[(size_t) a])
                {
                    auxReturnPdcApplied[(size_t) a] = t;
                    auxReturnPdcL[(size_t) a].reset();
                    auxReturnPdcR[(size_t) a].reset();
                    auxReturnPdcL[(size_t) a].setDelay ((float) t);
                    auxReturnPdcR[(size_t) a].setDelay ((float) t);
                }
            }
        }
        if (masterDryPdcApplied > 0)
        {
            auto* mL = mixL.data();
            auto* mR = mixR.data();
            for (int i = 0; i < numSamples; ++i)
            {
                masterDryPdcL.pushSample (0, mL[i]);
                masterDryPdcR.pushSample (0, mR[i]);
                mL[i] = masterDryPdcL.popSample (0);
                mR[i] = masterDryPdcR.popSample (0);
            }
        }
    }

    // AUX automation routing. Mirror of the per-track per-block block
    // up at the top: for each aux lane, drive liveReturnLevelDb /
    // liveMute from the lane in Read or (Touch && !touched) mode,
    // and pass the manual setpoint through otherwise. AuxLaneStrip
    // reads the live atoms - see updateGainTarget / the mute check
    // in AuxLaneStrip::processStereoBlock.
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& aparams = session.auxLane (a).params;
        const int amode = aparams.automationMode.load (std::memory_order_acquire);

        // FaderDb (continuous).
        {
            const auto& pts = aparams.automationLanes[(size_t) AutomationParam::FaderDb].pointsForRead();
            const bool touched = aparams.faderTouched.load (std::memory_order_acquire);
            const bool readsLane = amode == (int) AutomationMode::Read
                                 || (amode == (int) AutomationMode::Touch && ! touched);
            const float v = (readsLane && ! pts.empty())
                ? evaluateLane (pts, blockStartSamples, AutomationParam::FaderDb)
                : aparams.returnLevelDb.load (std::memory_order_relaxed);
            aparams.liveReturnLevelDb.store (v, std::memory_order_relaxed);
        }
        // Mute (discrete).
        {
            const auto& pts = aparams.automationLanes[(size_t) AutomationParam::Mute].pointsForRead();
            const bool readsLane = amode == (int) AutomationMode::Read
                                 || amode == (int) AutomationMode::Touch;
            const bool effective = (readsLane && ! pts.empty())
                ? (evaluateLane (pts, blockStartSamples, AutomationParam::Mute) >= 0.5f)
                : aparams.mute.load (std::memory_order_relaxed);
            aparams.liveMute.store (effective, std::memory_order_relaxed);
        }
    }

    // Master automation routing - single FaderDb lane.
    {
        auto& mparams = session.master();
        const int amode = mparams.automationMode.load (std::memory_order_acquire);
        const auto& pts = mparams.automationLanes[(size_t) AutomationParam::FaderDb].pointsForRead();
        const bool touched = mparams.faderTouched.load (std::memory_order_acquire);
        const bool readsLane = amode == (int) AutomationMode::Read
                             || (amode == (int) AutomationMode::Touch && ! touched);
        const float v = (readsLane && ! pts.empty())
            ? evaluateLane (pts, blockStartSamples, AutomationParam::FaderDb)
            : mparams.faderDb.load (std::memory_order_relaxed);
        mparams.liveFaderDb.store (v, std::memory_order_relaxed);
    }

    // AUX return lanes - process each lane's accumulated send buffer
    // through its plugin chain, then sum the wet output into master. Same
    // silence-skip optimisation as the bus pass above so idle lanes (no
    // channel sending to them) don't run their plugins.
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        // Tail-aware skip. A loaded send effect (reverb / delay) keeps
        // ringing after its input goes silent, so an input-peak skip
        // would hard-cut the tail at a block boundary. Instead, a lane
        // with loaded plugin slots only sleeps once its wet OUTPUT has
        // measured <= 1e-6 (~-120 dBFS) for kAuxTailSilenceSeconds of
        // CONSECUTIVE blocks - any audible output resets the clock
        // below, so the skip cannot engage mid-decay. What freezes is
        // the plugin's sub-audible residue, which resumes (still
        // sub-audible) on the next send. Hardware-insert lanes never
        // skip (outboard gear returns audio regardless of the send
        // level, and measuring that return requires running the block).
        bool laneHasPlugin = false, laneHasHardware = false;
        for (auto& m : auxLaneStrips[(size_t) a].insertMode)
        {
            const int mode = m.load (std::memory_order_relaxed);
            if (mode == AuxLaneStrip::kInsertPlugin)   laneHasPlugin   = true;
            if (mode == AuxLaneStrip::kInsertHardware) laneHasHardware = true;
        }

        const auto lanePeak = [this, numSamples] (int lane) -> float
        {
            const auto rngL = juce::FloatVectorOperations::findMinAndMax (
                                  auxLaneL[(size_t) lane].data(), numSamples);
            const auto rngR = juce::FloatVectorOperations::findMinAndMax (
                                  auxLaneR[(size_t) lane].data(), numSamples);
            return juce::jmax (
                juce::jmax (std::abs (rngL.getStart()), std::abs (rngL.getEnd())),
                juce::jmax (std::abs (rngR.getStart()), std::abs (rngR.getEnd())));
        };

        if (! laneHasHardware)
        {
            const float inPeak = lanePeak (a);
            const bool inputSilent = inPeak <= 1e-6f;

            // auxSilentRunSamples is zeroed in prepareForSelfTest, so a
            // sample-rate change can't hold a stale count against the
            // new window.
            const double srNow = currentSampleRate.load (std::memory_order_relaxed);
            const std::int64_t tailSamples = (std::int64_t) (kAuxTailSilenceSeconds
                                                            * (srNow > 0.0 ? srNow : 48000.0));
            // Empty lanes skip immediately on silent input (no tail to
            // protect); plugin lanes wait out the tail window.
            const bool canSkip = inputSilent
                              && (! laneHasPlugin
                                  || auxSilentRunSamples[(size_t) a] >= tailSamples);
            if (canSkip)
            {
                // Reset the aux-lane meter on skip - same rationale as the
                // bus-pass skip above. Without this the lane LED freezes
                // at the last-played level.
                auto& auxLaneRef = session.auxLane (a);
                auxLaneRef.params.meterPostL.store (-100.0f, std::memory_order_relaxed);
                auxLaneRef.params.meterPostR.store (-100.0f, std::memory_order_relaxed);
                continue;
            }
            if (! inputSilent)
                auxSilentRunSamples[(size_t) a] = 0;
        }

        auxLaneStrips[(size_t) a].processStereoBlock (auxLaneL[(size_t) a].data(),
                                                        auxLaneR[(size_t) a].data(),
                                                        numSamples,
                                                        currentDeviceInputs,
                                                        numCurrentDeviceInputs,
                                                        currentDeviceOutputs,
                                                        numCurrentDeviceOutputs);
        // Master-stage PDC: shorter-latency returns wait for the deepest lane
        // so every wet path lands on the (equally delayed) dry mix.
        if (auxReturnPdcApplied[(size_t) a] > 0)
        {
            auto* wL = auxLaneL[(size_t) a].data();
            auto* wR = auxLaneR[(size_t) a].data();
            auto& dL = auxReturnPdcL[(size_t) a];
            auto& dR = auxReturnPdcR[(size_t) a];
            for (int i = 0; i < numSamples; ++i)
            {
                dL.pushSample (0, wL[i]);
                dR.pushSample (0, wR[i]);
                wL[i] = dL.popSample (0);
                wR[i] = dR.popSample (0);
            }
        }
        auto* capAuxL = stemAuxCapL[(size_t) a].load (std::memory_order_acquire);
        auto* capAuxR = stemAuxCapR[(size_t) a].load (std::memory_order_relaxed);
        if (capAuxL != nullptr && capAuxR != nullptr)
        {
            juce::FloatVectorOperations::add (capAuxL,
                                                auxLaneL[(size_t) a].data(), numSamples);
            juce::FloatVectorOperations::add (capAuxR,
                                                auxLaneR[(size_t) a].data(), numSamples);
        }
        juce::FloatVectorOperations::add (mixL.data(),
                                            auxLaneL[(size_t) a].data(),
                                            numSamples);
        juce::FloatVectorOperations::add (mixR.data(),
                                            auxLaneR[(size_t) a].data(),
                                            numSamples);

        // Tail clock: advances only while the wet output has decayed to
        // silence (input silence is implied - a loud input resets the
        // run above before processing). Once it passes the tail window
        // the skip engages on the next silent-input block.
        if (laneHasPlugin && ! laneHasHardware)
        {
            if (lanePeak (a) <= 1e-6f)
                auxSilentRunSamples[(size_t) a] += numSamples;
            else
                auxSilentRunSamples[(size_t) a] = 0;
        }
    }

    perfLap (PerfSections::kAuxes);

    master.processInPlace (mixL.data(), mixR.data(), numSamples);

    // Realtime bounce: push each armed sink's block to its disk writer.
    // Post-master, pre-metronome, so the monitoring click never prints.
    // Writes are gated on the transport actually rolling - the sinks are
    // armed while stopped, and capturing the pre-roll would front-pad every
    // file with silence. The scratch WIPES are not gated: the taps
    // accumulate during stopped callbacks too (input monitoring runs the
    // strips), and an unwiped pre-roll prints as a summed burst at sample 0.
    // ThreadedWriter::write is a lock-free FIFO push (its TimeSliceThread
    // drains to disk), so this stays RT-safe.
    if (const int nSinks = rtBounceSinkCount.load (std::memory_order_acquire);
        nSinks > 0)
    {
        auto* sinks = rtBounceSinks.load (std::memory_order_relaxed);
        for (int s = 0; sinks != nullptr && s < nSinks; ++s)
        {
            auto& sink = sinks[s];
            if (isPlaying)
            {
                const float* srcL = sink.srcL != nullptr ? sink.srcL : mixL.data();
                const float* srcR = sink.srcR != nullptr ? sink.srcR : mixR.data();

                int offset = 0;
                if (sink.leadRemaining > 0)
                {
                    offset = (int) juce::jmin ((std::int64_t) numSamples, sink.leadRemaining);
                    sink.leadRemaining -= offset;
                }
                const auto already = sink.written.load (std::memory_order_relaxed);
                const int n = (int) juce::jmin ((std::int64_t) (numSamples - offset),
                                                  sink.cap - already);
                if (n > 0)
                {
                    const float* chans[2] = { srcL + offset, srcR + offset };
                    if (sink.writer->write (chans, n))
                        sink.written.store (already + n, std::memory_order_release);
                    else
                        sink.writeFailed.store (true, std::memory_order_release);
                }
            }
            if (sink.wipeL != nullptr)
            {
                juce::FloatVectorOperations::clear (sink.wipeL, numSamples);
                juce::FloatVectorOperations::clear (sink.wipeR, numSamples);
            }
        }
    }

    // Metronome - push session BPM + enable into the click generator each
    // block (cheap atomic loads), then mix the click into the post-master
    // bus. Click is post-master so it never gets EQ'd or compressed by
    // the master strip - it's purely a monitoring aid.
    // Apply per-mode metronome flags from the right-click menu:
    //   - clickWhileRecording / clickWhilePlaying gate the engine-side
    //     "is the click eligible at all this block?" decision.
    //   - onlyDuringCountIn overrides both during a record pass - the
    //     click stops the instant the take's first sample arrives.
    //   - polyphonic forwarded to Metronome.setPolyphonic so a new
    //     click can trigger before the previous body finishes.
    const bool wantWhileRec  = session.metronomeClickWhileRecording.load (std::memory_order_relaxed);
    const bool wantWhilePlay = session.metronomeClickWhilePlaying  .load (std::memory_order_relaxed);
    const bool onlyCountIn   = session.metronomeOnlyDuringCountIn  .load (std::memory_order_relaxed);
    const bool polyphonic    = session.metronomePolyphonic         .load (std::memory_order_relaxed);

    metronome.setEnabled    (session.metronomeEnabled.load (std::memory_order_relaxed));
    {
        // Click follows the tempo map: use the tempo in effect at the block's
        // playhead (per-block is plenty for a monitoring click). Falls back to
        // the constant tempo when no map is published.
        const TempoMap* tmMetro = rtTempoMap.load (std::memory_order_acquire);
        const float metroBpm = (tmMetro != nullptr && ! tmMetro->empty())
                                 ? tmMetro->bpmAt (blockStartSamples)
                                 : session.tempoBpm.load (std::memory_order_relaxed);
        metronome.setBpm (metroBpm);
    }
    metronome.setBeatsPerBar(session.beatsPerBar.load (std::memory_order_relaxed));
    metronome.setVolumeDb   (session.metronomeVolDb.load (std::memory_order_relaxed));
    metronome.setPolyphonic (polyphonic);
    {
        const auto recStart   = activeRecordStart.load (std::memory_order_relaxed);
        const bool inCountIn  = isRecording && blockStartSamples < recStart;

        // Decide whether the click is eligible this block.
        //   - Recording (post-count-in): gated by clickWhileRecording AND
        //     NOT (onlyDuringCountIn). If onlyDuringCountIn is set the
        //     click is suppressed once the take begins.
        //   - Playing: gated by clickWhilePlaying.
        //   - Count-in pre-roll: ALWAYS forced on - user enabled count-in
        //     specifically to get the lead-in clicks regardless of the
        //     CLICK toggle.
        bool clickRolling = false;
        if (isRecording)
            clickRolling = inCountIn
                              ? true
                              : (wantWhileRec && ! onlyCountIn);
        else if (isPlaying)
            clickRolling = wantWhilePlay;

        // The click is a monitoring aid and must never print: an offline
        // bounce captures the mix it is about to be added to, so suppress it
        // for the render. (Realtime bounces capture pre-click, upstream.)
        if (offlineRenderActive.load (std::memory_order_relaxed))
            clickRolling = false;

        // The click mixes post-master, AFTER the master-stage aux PDC delayed
        // the program by masterDryPdcApplied - reference the click to the
        // delayed position or it leads everything it's supposed to mark.
        metronome.process (blockStartSamples - (std::int64_t) masterDryPdcApplied,
                            clickRolling,
                            mixL.data(), mixR.data(), numSamples,
                            /*forceEnable*/ inCountIn);
    }

    writeMasterMixToOutput (outputChannelData, numOutputChannels, numSamples);

    if (isPlaying || isRecording)
    {
        transport.advancePlayhead (numSamples);

        // Loop wrap-around. Only honoured during plain playback - during
        // Recording we keep the playhead linear so the captured WAV maps
        // cleanly onto the timeline (loop-take-stacking is a future
        // feature). Wrap is whole-block accurate: we do not split the
        // current block, so the playhead may briefly read up to one block
        // past loopEnd before snapping back. That overshoot is silent
        // because PlaybackEngine returns silence outside region bounds.
        if (isPlaying && ! isRecording && transport.isLoopEnabled())
        {
            const auto lStart = transport.getLoopStart();
            const auto lEnd   = transport.getLoopEnd();
            if (lEnd > lStart)
            {
                const auto curr = transport.getPlayhead();
                if (curr >= lEnd)
                {
                    const auto loopLen   = lEnd - lStart;
                    const auto overshoot = (curr - lEnd) % loopLen;
                    transport.setPlayhead (lStart + overshoot);
                }
            }
        }
    }

    // Detect xrun: callback work shouldn't exceed the buffer's wall-clock
    // budget. If it does, we'd glitch on the next callback. Track the count
    // for the status bar. Same pass also updates the smoothed CPU usage
    // (callback wall-time / buffer audio-time, one-pole LPF) which the
    // status bar polls.
    perfLap (PerfSections::kMasterOut);
    if (perfOn)
    {
        perf.totalTicks.fetch_add (juce::Time::getHighResolutionTicks() - callbackStart,
                                   std::memory_order_relaxed);
        perf.blocks.fetch_add (1, std::memory_order_relaxed);
    }

    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double bufferMs = 1000.0 * (double) numSamples / sr;
        const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - callbackStart)
                                     * secondsPerTick * 1000.0;
        if (elapsedMs > bufferMs)
            xrunCount.fetch_add (1, std::memory_order_relaxed);

        if (bufferMs > 0.0)
        {
            const float instant = (float) juce::jlimit (0.0, 2.0, elapsedMs / bufferMs);
            // 0.2 coefficient -> ~5-block smoothing; fast enough that the
            // user sees real spikes, slow enough to mask single-block jitter.
            const float prev = cpuUsage.load (std::memory_order_relaxed);
            cpuUsage.store (prev + 0.2f * (instant - prev), std::memory_order_relaxed);
        }
    }
}
} // namespace duskstudio
