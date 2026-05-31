#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <vector>
#include "../dsp/AuxLaneStrip.h"
#include "../dsp/BusStrip.h"
#include "../dsp/ChannelStrip.h"
#include "../dsp/MasterBus.h"
#include "../dsp/MasteringChain.h"
#include "../dsp/Metronome.h"
#include "../dsp/PitchDetector.h"
#include "MidiSyncReceiver.h"
#include "MidiTimeCodeReceiver.h"
#include "MidiClockEmitter.h"
#include "MidiTimeCodeEmitter.h"
#include "../session/Session.h"
#include "MasteringPlayer.h"
#include "PlaybackEngine.h"
#include "PluginManager.h"
#include "RecordManager.h"
#include "Transport.h"
#include "DuskStudioPlayHead.h"

namespace duskstudio
{
// input -> channel strip (live or playback source) -> aux/master.
// Owns Transport, recording, playback, plugin host.
class AudioEngine final : public juce::AudioIODeviceCallback,
                            public juce::MidiInputCallback,
                            public juce::ChangeBroadcaster,
                            public juce::ChangeListener
{
public:
    // Recording / Mixing / Aux share the live track-to-master path; only
    // UI changes between them. Mastering swaps signal flow to stereo
    // file -> MasteringChain -> output.
    enum class Stage { Recording, Mixing, Aux, Mastering };

    explicit AudioEngine (Session& sessionToBindTo);
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    Session&          getSession()        noexcept { return session; }
    const Session&    getSession() const   noexcept { return session; }
    Transport&        getTransport()      noexcept { return transport; }
    const Transport&  getTransport() const noexcept { return transport; }
    RecordManager&    getRecordManager()   noexcept { return recordManager; }
    PlaybackEngine&   getPlaybackEngine()  noexcept { return playbackEngine; }
    PluginManager&    getPluginManager()   noexcept { return pluginManager; }
    MasteringPlayer&  getMasteringPlayer() noexcept { return masteringPlayer; }
    MasteringChain&   getMasteringChain()  noexcept { return masteringChain; }
    MasterBus&        getMasterBus()       noexcept { return master; }
    Metronome&        getMetronome()       noexcept { return metronome; }

    Stage getStage() const noexcept { return stage.load (std::memory_order_relaxed); }
    void  setStage (Stage s) noexcept;

    // Re-enumerate MIDI inputs after hot-plug. Detach + rebuild + reattach
    // so audio thread doesn't race the mutation. Message-thread only.
    void refreshMidiInputs();

    // Lightweight: re-map the loaded session's saved per-track MIDI in/out
    // IDENTIFIERS to runtime INDICES against the EXISTING device banks. No
    // bank rebuild / callback detach (the physical devices are unchanged since
    // startup), so it doesn't reconfigure the audio device or churn MIDI
    // handles. Used on session load; MCU + sync re-resolve runs in
    // openConfiguredMidiOutputs. Message-thread only.
    void reresolveTrackMidiFromSession();

    const juce::Array<juce::MidiDeviceInfo>& getMidiInputDevices() const noexcept
    {
        return midiInputDevices;
    }
    const juce::Array<juce::MidiDeviceInfo>& getMidiOutputDevices() const noexcept
    {
        return midiOutputDevices;
    }

    // Synthetic "Virtual Keyboard (Dusk Studio)" collector appended to
    // midiInputDevices. nullptr until rebuildMidiInputBank has run.
    juce::MidiMessageCollector* getVirtualKeyboardCollector() noexcept;

    // Index of the virtual keyboard inside midiInputDevices, or -1 if
    // the bank hasn't been built yet. Used by UI flows that auto-route
    // the on-screen keyboard to a freshly loaded instrument track.
    int getVirtualKeyboardInputIndex() const noexcept { return virtualKeyboardCollectorIndex; }

    // Lazy-open + start delivery thread. Opening every available output
    // at startup blocks the main thread (snd_seq_connect_to is sync).
    // Message-thread only.
    bool ensureMidiOutputOpen (int index);

    // No implicit ensure — caller checks state first. Safe from audio
    // OR message thread (JUCE marshals to its own delivery thread).
    bool sendMidiToOutput (int index, const juce::MidiBuffer& events) noexcept;

    // Open the MIDI output every track is routed to. Called once after
    // SessionSerializer::load resolves identifiers. Message-thread only.
    void openConfiguredMidiOutputs();

    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    struct RegionClipboard
    {
        bool        hasContent = false;
        int         sourceTrack = -1;
        AudioRegion region;
    };
    RegionClipboard& getRegionClipboard() noexcept { return regionClipboard; }

    ChannelStrip& getChannelStrip (int idx) noexcept
    {
        jassert (idx >= 0 && idx < (int) strips.size());
        return strips[(size_t) idx];
    }
    const ChannelStrip& getChannelStrip (int idx) const noexcept
    {
        jassert (idx >= 0 && idx < (int) strips.size());
        return strips[(size_t) idx];
    }
    // Legacy alias kept so RegionEditActions / ConsoleView compile.
    ChannelStrip&       getStrip (int idx)       noexcept { return getChannelStrip (idx); }
    const ChannelStrip& getStrip (int idx) const noexcept { return getChannelStrip (idx); }

    BusStrip& getBusStrip (int idx) noexcept
    {
        jassert (idx >= 0 && idx < (int) busStrips.size());
        return busStrips[(size_t) idx];
    }
    const BusStrip& getBusStrip (int idx) const noexcept
    {
        jassert (idx >= 0 && idx < (int) busStrips.size());
        return busStrips[(size_t) idx];
    }
    AuxLaneStrip& getAuxLaneStrip (int idx) noexcept
    {
        jassert (idx >= 0 && idx < (int) auxLaneStrips.size());
        return auxLaneStrips[(size_t) idx];
    }
    const AuxLaneStrip& getAuxLaneStrip (int idx) const noexcept
    {
        jassert (idx >= 0 && idx < (int) auxLaneStrips.size());
        return auxLaneStrips[(size_t) idx];
    }

    void play();
    void stop();
    void record();

    // Marker jumps clamp to known points — no overshoot past zero or
    // past the last marker. Message-thread only.
    void jumpToPrevMarker();
    void jumpToNextMarker();
    void jumpToZero();
    void jumpToLastRecordPoint();

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    // Fires when the backend can no longer service I/O. JUCE doesn't
    // guarantee a follow-up audioDeviceStopped, so we do the same
    // flush-record-and-stop dance to keep recordings recoverable.
    void audioDeviceError (const juce::String& errorMessage) override;

    // Bound via addMidiInputDeviceCallback("", this). Fires from JUCE's
    // MIDI input thread (NOT the audio thread); routes to per-input
    // collectors that the audio thread drains lock-free.
    void handleIncomingMidiMessage (juce::MidiInput* source,
                                       const juce::MidiMessage& message) override;

    // Prepare without a real AudioIODevice. The self-test detaches the
    // engine and drives audioDeviceIOCallbackWithContext directly with
    // synthetic buffers.
    void prepareForSelfTest (double sampleRate, int blockSize);

    // Test-only. Next callback merges `events` into perInputMidi[inputIdx]
    // AFTER collector drain. Cleared after one block.
    void stageTestMidiInjection (int inputIdx, juce::MidiBuffer events);

    // Bookends around SessionSerializer save/load. publish copies each
    // PluginSlot's description + state into the Track fields; consume
    // restores in reverse.
    //
    // audioCallbackDetached: pass true only when the caller already
    // removed this engine from AudioDeviceManager. Skips the atomic-park
    // sleep that defends against audio-thread re-entry, dropping
    // message-thread block time from hundreds of ms to roughly the cost
    // of state I/O alone on heavy sessions.
    void publishPluginStateForSave (bool audioCallbackDetached = false);
    void consumePluginStateAfterLoad();

    struct PluginLoadFailure
    {
        juce::String location;
        juce::String pluginName;
    };
    const std::vector<PluginLoadFailure>& getLastPluginLoadFailures() const noexcept
    {
        return lastPluginLoadFailures;
    }

    // releaseResources on every loaded plugin. Diva's terminate()
    // attempts host callbacks; doing them on an already-inactive plugin
    // is safe, doing them while it believes it's rendering aborts with
    // __cxa_pure_virtual. Caller MUST have removed the audio callback.
    void releaseAllPluginResources();

    // Process-shutdown only. Drops ownership of every plugin instance
    // without destructing. See PluginSlot::leakInstanceForShutdown.
    void leakAllPluginInstancesForShutdown();

    void publishTransportStateForSave();
    void consumeTransportStateAfterLoad();

    double getCurrentSampleRate() const noexcept { return currentSampleRate.load (std::memory_order_relaxed); }
    int    getCurrentBlockSize() const noexcept  { return currentBlockSize.load  (std::memory_order_relaxed); }

    // Engine-side xrun: callback wall-clock exceeded the buffer's
    // audio time. Distinct from getBackendXRunCount.
    int    getXRunCount() const noexcept         { return xrunCount.load         (std::memory_order_relaxed); }

    // 0..1 fraction of buffer wall-clock consumed by the callback,
    // one-pole-LPF smoothed. xruns imminent above ~0.85.
    float  getCpuUsage() const noexcept          { return cpuUsage.load          (std::memory_order_relaxed); }

    // Backend xruns (e.g. ALSA snd_pcm_recover EPIPE). 0 if no device.
    int    getBackendXRunCount() const noexcept;

    // False = PipeWire opened the per-device ALSA name with 0 output
    // channels and the user gets silent output with no error.
    bool   hasUsableOutputs() const noexcept { return usableOutputs.load (std::memory_order_relaxed); }

    // Forces a transport-LED edge without waiting for the next 30 Hz
    // tick. nullptr during selftest static-init.
    class McuController* getMcuController() noexcept { return mcuController.get(); }

    // H5: UI sink invoked when the active audio device disappears
    // unexpectedly (hot-unplug, OS audio service restart). Sink runs
    // on the message thread — engine marshals via callAsync from the
    // ChangeListener path. UI typically surfaces a non-blocking
    // EmbeddedModal banner with a route to Audio Settings.
    //
    // Set at startup (MainComponent), nullable. Message-thread-only;
    // engine never invokes from the audio thread.
    using DeviceLostAlertSink = std::function<void (juce::String)>;
    void setDeviceLostAlertSink (DeviceLostAlertSink sink) noexcept
    {
        onDeviceLostAlert_ = std::move (sink);
    }

    // Fired when record() refuses to start (no track armed, or no audio
    // device open) so the UI can show an in-window alert instead of the
    // failure only reaching stderr. Set at startup (MainComponent), nullable.
    // Message-thread-only — record() is never called from the audio thread.
    using RecordBlockedSink = std::function<void (juce::String)>;
    void setRecordBlockedSink (RecordBlockedSink sink) noexcept
    {
        onRecordBlocked_ = std::move (sink);
    }

    // juce::ChangeListener — fires on the message thread when
    // AudioDeviceManager's device list / current device changes.
    // We use it to detect hot-unplug (current device became nullptr
    // while we had one). Message-thread-only.
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

private:
    Session& session;
    juce::AudioDeviceManager deviceManager;

    Transport       transport;
    // Heap-allocated so we can pass &session.tempoBpm / &currentSampleRate
    // from the ctor body, after those addresses are known.
    std::unique_ptr<DuskStudioPlayHead> playHead;
    RecordManager   recordManager   { session };

    std::vector<PluginLoadFailure> lastPluginLoadFailures;
    PlaybackEngine  playbackEngine  { session };
    PluginManager   pluginManager;
    juce::UndoManager undoManager;
    RegionClipboard   regionClipboard;

    MasteringPlayer  masteringPlayer;
    MasteringChain   masteringChain;
    Metronome        metronome;
    PitchDetector    pitchDetector;
    MidiSyncReceiver       midiSyncReceiver;
    MidiTimeCodeReceiver   midiTimeCodeReceiver;
    std::unique_ptr<class McuReceiver>   mcuReceiver;
    std::unique_ptr<class McuController> mcuController;

    MidiClockEmitter     midiClockEmitter;
    MidiTimeCodeEmitter  midiTimeCodeEmitter;
    juce::MidiBuffer midiClockOutScratch;
    int              lastSyncOutputIdx = -1;

    // Monotonic sample clock the sync receiver timestamps clock ticks
    // against. Reset when syncSourceInputIdx changes so old interval
    // history doesn't poison the new source's BPM. Can't use
    // transport.getPlayhead — it jumps on loop wrap and scrub.
    juce::int64 midiSyncSampleClock = 0;
    int         lastSyncSourceIdx   = -1;
    // Rolling-edge detector — act on the transition, not the steady
    // state, so a long Start signal doesn't restart every block.
    bool        lastExtRolling      = false;
    // MTC chase state.
    //  lastChaseEnabled: on false->true while master is rolling, force
    //                    a re-lock by clearing lastMtcRolling.
    //  mtcDriftWindowFrames: counts consecutive MTC FRAMES (not audio
    //                    blocks) over tolerance; lastSeenMtcFrames
    //                    gates the increment so it only ticks when
    //                    MTC actually advances.
    bool        lastMtcRolling      = false;
    bool        lastChaseEnabled    = false;
    int         mtcDriftWindowFrames = 0;
    juce::int64 lastSeenMtcFrames    = -1;
    // Recording is the workflow start state. Saved sessions overwrite
    // via session.uiStage on load.
    std::atomic<Stage> stage { Stage::Recording };

    std::array<ChannelStrip, Session::kNumTracks> strips;
    std::array<BusStrip,  Session::kNumBuses> busStrips;
    std::array<AuxLaneStrip, Session::kNumAuxLanes> auxLaneStrips;
    MasterBus master;

    std::vector<float> mixL, mixR;
    std::array<std::vector<float>, Session::kNumBuses> busL, busR;
    std::array<std::vector<float>, Session::kNumAuxLanes> auxLaneL, auxLaneR;
    std::vector<float> playbackScratch;
    std::vector<float> playbackScratchR;
    // Fed to strips with a generator-style insert when the track has no
    // audio source — lets the insert emit even without input or playback.
    std::vector<float> silentInputScratch;

    // One MidiMessageCollector per registered input. MIDI thread
    // addMessageToQueue's; audio thread drains per-block into
    // perInputMidi[i]. Both sides lock-free per JUCE contract.
    juce::Array<juce::MidiDeviceInfo> midiInputDevices;
    std::vector<std::unique_ptr<juce::MidiMessageCollector>> midiInputCollectors;
    std::vector<juce::MidiBuffer> perInputMidi;
    juce::MidiBuffer perTrackMidiScratch;

    // Recomputed every rebuildMidiInputBank so hot-plug doesn't invalidate.
    int virtualKeyboardCollectorIndex { -1 };

    // SPSC handoff. Producer must not touch testInjectMidi while
    // testInjectReady==true. Single relaxed load + branch per block in
    // production.
    juce::MidiBuffer  testInjectMidi;
    std::atomic<int>  testInjectInputIdx { -1 };
    std::atomic<bool> testInjectReady    { false };

    juce::Array<juce::MidiDeviceInfo> midiOutputDevices;
    std::vector<std::unique_ptr<juce::MidiOutput>> midiOutputs;

    // Previous block's midiInputIndex per track — detects mid-play
    // input swaps so we can fire All-Notes-Off + Sustain-Off on the
    // new input. Without this, held notes from the previous device
    // keep ringing (Note Off never arrives on the now-unrouted source).
    std::array<int, Session::kNumTracks> lastMidiInputIndex {};

    // Called from ctor BEFORE callbacks register, and from
    // refreshMidiInputs WHILE callbacks are detached. The detach-rebuild-
    // reattach fence is what makes vector mutation safe.
    void rebuildMidiInputBank();
    void rebuildMidiOutputBank();

    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };
    std::atomic<int>    xrunCount         { 0 };
    std::atomic<float>  cpuUsage          { 0.0f };

    // Valid for the duration of one callback only — strips consume
    // synchronously and never cache across blocks.
    const float* const* currentDeviceInputs   = nullptr;
    int                 numCurrentDeviceInputs  = 0;
    float* const*       currentDeviceOutputs  = nullptr;
    int                 numCurrentDeviceOutputs = 0;

    // 1 / juce::Time::getHighResolutionTicksPerSecond(). Avoids the
    // internal divide in highResolutionTicksToSeconds on the xrun
    // watchdog (twice per callback).
    double secondsPerTick { 0.0 };

    std::atomic<bool>   usableOutputs     { true };

    // H5 hot-unplug detector: set in audioDeviceAboutToStart, cleared
    // when changeListenerCallback observes a now-null current device.
    // Atomic so audioDeviceAboutToStart (audio device thread) and
    // changeListenerCallback (message thread) don't race on the flag
    // itself. Acquire / release across the threads.
    std::atomic<bool>   hadLiveDevice_    { false };

    DeviceLostAlertSink onDeviceLostAlert_;
    RecordBlockedSink   onRecordBlocked_;

    // First sample committed to disk. Under count-in the playhead is
    // rolled back before this; writes are skipped until it catches up.
    // INT64_MIN = no record active.
    std::atomic<juce::int64> activeRecordStart { std::numeric_limits<juce::int64>::min() };

    // Audio-thread-only. Together these let us detect two events that
    // require a per-block "All Notes Off" flush:
    //   • rolling -> stopped: held notes from playback or live input
    //     would otherwise sustain forever.
    //   • playhead discontinuity (loop wrap, scrub): notes whose Note
    //     Off is past the jump never fire — synth stuck.
    bool         wasRolling          = false;
    juce::int64  lastBlockEndSample  = 0;
};
} // namespace duskstudio
