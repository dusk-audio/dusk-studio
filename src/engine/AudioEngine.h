#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <mutex>
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
#include "AudioWorkerPool.h"
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

    // initialWorkers seeds the parallel strip-DSP worker count applied at the
    // first prepare (so Auto takes effect at startup without a re-prepare). The
    // UI layer resolves it from per-machine AppConfig; headless callers default
    // to 0 (serial). DUSKSTUDIO_AUDIO_WORKERS overrides it at every prepare.
    explicit AudioEngine (Session& sessionToBindTo, int initialWorkers = 0);
    ~AudioEngine() override;

    // Message thread. Target worker count for the next prepare; clamped to the
    // host cap. Does NOT re-prepare — caller drives a device-callback detach/
    // reattach (as the Audio Settings panel does) to apply it live. Ignored
    // while DUSKSTUDIO_AUDIO_WORKERS pins the count.
    void setDesiredWorkers (int n) noexcept { desiredWorkers = juce::jmax (0, n); }

    // Largest worker count the engine can use: the strips fan out across at most
    // kMaxDspLanes - 1 worker lanes (the audio callback runs the last lane
    // itself). Single source of truth for AppConfig's settings/manual cap.
    static constexpr int getMaxWorkerCount() noexcept { return kMaxDspLanes - 1; }

    // Message thread. CALLER MUST have removed this engine as the audio callback
    // first (so no audio thread is inside the worker pool's runBlock) — the
    // Audio Settings panel detaches around the change. Stops+restarts the pool
    // to the current desired count. This is the ONLY live-reconfigure path;
    // routine device re-opens (buffer-size/rate changes) must NOT touch the pool
    // because their prepare runs while the callback is still attached.
    void applyDesiredWorkers();

    // Process gate. suspendProcessing (message thread) raises a flag the audio
    // callback checks at entry, then blocks until in-flight callbacks drain;
    // until resumeProcessing the callback emits silent buffers. Used to make
    // re-prepare provably exclusive of processing (Ardour's process-lock
    // pattern). prepareForSelfTest brackets itself with these automatically.
    void suspendProcessing();
    void resumeProcessing() noexcept;

    // Test-only. Immediately stop+start the pool to `n` workers. Safe only when
    // no audio callback is concurrently in runBlock() — the offline self-test
    // drives the callback synchronously, so the A/B harness can flip the count
    // between captures.
    void setWorkerCountForTest (int n);

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

    // Message thread: replace the session tempo map and republish the lock-free
    // snapshot the audio thread reads. Call setTempoPoints for edits; call
    // publishTempoMap after a session load (or any other mutation of
    // session.tempoMap) to sync the audio-thread copy.
    void setTempoPoints (std::vector<TempoPoint> pts);
    void publishTempoMap();

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

    // No implicit ensure — caller checks state first. Message thread
    // only (MCU feedback sink). The audio thread routes MIDI out through
    // queueMidiOutBlock instead — sendBlockOfMessages locks and
    // allocates, so it must never run in the callback.
    bool sendMidiToOutput (int index, const juce::MidiBuffer& events) noexcept;

    // Open the MIDI output every track is routed to. Called once after
    // SessionSerializer::load resolves identifiers. Message-thread only.
    // Caller MUST have the audio callback detached (startup pre-attach path).
    void openConfiguredMidiOutputs();

    // Same, but for live callers (session load) where the audio callback is
    // attached: detaches it around the bank mutation so the audio thread
    // never reads midiOutputs mid-open. Message-thread only.
    void openConfiguredMidiOutputsSafely();

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

    // Offline-render oversampling override. When > 0, the next prepare uses
    // this factor instead of session.oversamplingFactor, so an offline bounce
    // can render the saturating analog stages (console/comp/tube/tape) alias-
    // free at e.g. 4× while realtime monitoring stays at the user's lighter
    // factor. BounceEngine sets it around its render and clears it (0) before
    // the engine is re-prepared for live playback.
    void setRenderOversamplingOverride (int factor) noexcept
        { renderOversamplingOverride.store (factor, std::memory_order_relaxed); }

    // Cross-track Plugin Delay Compensation. Reads each track's reported insert
    // latency (plugin OR hardware, gated by mode; MIDI tracks count 0 because
    // their instrument latency is already absorbed by the MIDI scheduling
    // pre-shift), finds the deepest, and sets every strip's compensation =
    // deepest − own so all tracks line up. Pure atomic loads/stores — no alloc,
    // lock, or I/O — so it runs once per audio block (auto-tracks any latency
    // change: plugin load/unload, HW measure, auto-bypass) and also at prepare.
    // aggregatePdcLatencySamples exposes the deepest track latency for bounce
    // lead-in trimming.
    void recomputePdc() noexcept;
    int  getAggregatePdcLatencySamples() const noexcept
    {
        return aggregatePdcLatencySamples.load (std::memory_order_relaxed);
    }

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

    // Zero both xrun readouts (status-bar double-click). The backend
    // counter is device-owned and can't be cleared, so it's offset
    // against a baseline instead. Message thread.
    void   resetXRunCounts() noexcept;

    // Per-section callback timing (see PerfSections below). Capture is
    // normally enabled by DUSKSTUDIO_PERF=1 + a 2 s reporter timer; the
    // headless session-perf harness enables it directly and prints once
    // at the end of its run.
    void setPerfCaptureEnabled (bool b) noexcept { perf.enabled.store (b, std::memory_order_relaxed); }
    void printPerfTable();

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

    // One-shot startup device-open outcome. Empty = the saved device opened.
    // Non-empty when the saved device was busy and we fell back (or couldn't).
    // The UI reads it ONCE after construction — the alert sinks above are still
    // null while AudioEngine itself is being constructed, so the startup result
    // can't go through them.
    juce::String consumeStartupDeviceMessage()
    {
        auto m = startupDeviceMessage_;
        startupDeviceMessage_.clear();
        return m;
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

    // Lock-free tempo-map snapshot for the audio thread (MIDI scheduler +
    // metronome). publishTempoMap heap-allocates an immutable copy, keeps it
    // alive in the pool, and release-stores its pointer; the audio thread
    // acquire-loads and never frees. The pool is only ever appended to, and
    // only from the message thread, so the audio thread can read a published
    // copy without it being freed or reallocated under it. Retired copies stay
    // alive for the session (each is tiny — a few tempo points) and are freed
    // when the engine is destroyed. null pointer = treat as constant tempoBpm.
    std::vector<std::unique_ptr<TempoMap>> rtTempoMapPool;
    std::atomic<const TempoMap*>           rtTempoMap { nullptr };

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

    // Deepest per-track insert latency in the session (samples). Set by
    // recomputePdc; read by BounceEngine to trim the render's lead-in.
    std::atomic<int> aggregatePdcLatencySamples { 0 };

    // Render-time oversampling override (0 = use session factor). See
    // setRenderOversamplingOverride.
    std::atomic<int> renderOversamplingOverride { 0 };

    std::array<ChannelStrip, Session::kNumTracks> strips;
    std::array<BusStrip,  Session::kNumBuses> busStrips;
    std::array<AuxLaneStrip, Session::kNumAuxLanes> auxLaneStrips;
    MasterBus master;

    std::vector<float> mixL, mixR;
    std::array<std::vector<float>, Session::kNumBuses> busL, busR;
    std::array<std::vector<float>, Session::kNumAuxLanes> auxLaneL, auxLaneR;

    // Aux tail-aware skip: consecutive samples each plugin lane's wet
    // output has been silent. Past kAuxTailSilenceSeconds the lane
    // sleeps until its input returns. Audio thread only.
    static constexpr double kAuxTailSilenceSeconds = 2.0;
    std::array<juce::int64, Session::kNumAuxLanes> auxSilentRunSamples {};
    // Per-track disk-playback buffers. One per track (not a single shared
    // scratch) so the per-block work can run as two passes — a serial PREP
    // pass that resolves each track's source + MIDI, then a DSP pass that
    // processes the strips — without a later track's prep clobbering an
    // earlier track's not-yet-processed source. (Prerequisite for running the
    // DSP pass across cores.)
    std::array<std::vector<float>, Session::kNumTracks> playbackScratch;
    std::array<std::vector<float>, Session::kNumTracks> playbackScratchR;
    // Fed to strips with a generator-style insert when the track has no
    // audio source — lets the insert emit even without input or playback.
    std::vector<float> silentInputScratch;

    // Per-track DSP inputs resolved by the PREP pass and consumed by the DSP
    // pass. Pointers reference the persistent per-track buffers above (disk),
    // the device input block (live), or silentInputScratch — all valid for the
    // whole callback.
    struct TrackDspJob
    {
        const float* monoIn      = nullptr;
        const float* monoInR     = nullptr;
        const float* deviceInput = nullptr;   // raw input for the recorder
        bool         isMidi      = false;
        bool         passes      = false;
        bool         armed       = false;
        bool         stereoInput = false;
    };
    std::array<TrackDspJob, Session::kNumTracks> trackJobs;

    // ── Opt-in parallel strip DSP ────────────────────────────────────────
    // The DSP pass over the 24 strips can be fanned out across worker threads
    // when DUSKSTUDIO_AUDIO_WORKERS is set (default: serial, the proven path).
    // Each lane accumulates its strip subset into its OWN buffer set so the
    // workers never write a shared buffer; a serial reduce then sums the lane
    // sets into mixL/busL/auxLaneL. Metering + recording stay on a serial tail
    // pass (audio thread), so worker threads only ever run pure DSP.
    static constexpr int kMaxDspLanes = 16;
    struct AccumSet
    {
        std::vector<float> mixL, mixR;
        std::array<std::vector<float>, Session::kNumBuses>    busL, busR;
        std::array<std::vector<float>, Session::kNumAuxLanes> auxL, auxR;
    };
    std::array<AccumSet, kMaxDspLanes> laneAccum;
    AudioWorkerPool workerPool;
    // Target worker count (message-thread-owned). Reconciled into workerPool at
    // each prepare; DUSKSTUDIO_AUDIO_WORKERS overrides it there.
    int  desiredWorkers = 0;
    // The pool is started exactly once (first prepare) and thereafter only
    // reconfigured via applyDesiredWorkers with the callback detached. Device
    // re-opens (buffer/rate change) must not stop+start it — start/stop mutate
    // worker state that a live runBlock reads lock-free. (In-flight LANES are
    // separately handled: prepare/stopped quiesce the pool first.)
    bool workerPoolStarted = false;
    int  currentBlockSamples = 0;        // stashed for the worker lane job

    // Env DUSKSTUDIO_AUDIO_WORKERS (CI / power users) overrides desiredWorkers.
    int  resolveTargetWorkers() const noexcept;

    // Message thread, callback detached or offline. Stop+start the pool so it
    // runs exactly `target` workers (clamped to the host cap); no-op if already
    // there. NEVER call while the audio callback is attached.
    void reconcileWorkerPool (int target);

    void accumulateStrip (int t, float* mL, float* mR,
                          const std::array<float*, ChannelStrip::kNumBuses>& bL,
                          const std::array<float*, ChannelStrip::kNumBuses>& bR,
                          const std::array<float*, ChannelStripParams::kNumAuxSends>& aL,
                          const std::array<float*, ChannelStripParams::kNumAuxSends>& aR,
                          int numSamples) noexcept;
    void processStripLane (int lane) noexcept;            // one worker lane
    void reduceLaneAccum (int numSamples) noexcept;       // sum lanes → mix

    // One MidiMessageCollector per registered input. MIDI thread
    // addMessageToQueue's; audio thread drains per-block into
    // perInputMidi[i]. Both sides lock-free per JUCE contract.
    juce::Array<juce::MidiDeviceInfo> midiInputDevices;
    std::vector<std::unique_ptr<juce::MidiMessageCollector>> midiInputCollectors;
    std::vector<juce::MidiBuffer> perInputMidi;
    std::array<juce::MidiBuffer, Session::kNumTracks> perTrackMidiScratch;

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

    // MIDI-out handoff. juce::MidiOutput::sendBlockOfMessages is NOT
    // audio-thread safe: it takes the delivery thread's mutex (which
    // that thread holds across waits of up to ~20 ms) and inserts into
    // a heap-allocating multiset under it. The audio thread instead
    // writes whole per-port event blocks into this lock-free FIFO; the
    // pump thread drains it every millisecond and does the
    // sendBlockOfMessages call where blocking is harmless. Slot
    // MidiBuffers are pre-sized in prepareForSelfTest so the audio-
    // thread copy never allocates. Queue-full drops the block —
    // dropping clock bytes beats an xrun.
    static constexpr int kMidiOutQueueSlots = 64;
    static constexpr int kMidiOutSlotBytes  = 4096;
    struct QueuedMidiOut
    {
        int    port       = -1;
        double timeMs     = 0.0;
        double sampleRate = 48000.0;
        juce::MidiBuffer events;
    };
    juce::AbstractFifo midiOutFifo { kMidiOutQueueSlots };
    std::array<QueuedMidiOut, kMidiOutQueueSlots> midiOutQueue;

    // Serialises the pump thread's port access against message-thread
    // bank mutation (rebuildMidiOutputBank / ensureMidiOutputOpen). The
    // audio thread never takes it — it only touches the FIFO.
    std::mutex midiOutBankMutex;

    class MidiOutPump final : public juce::Thread
    {
    public:
        explicit MidiOutPump (AudioEngine& e)
            : juce::Thread ("Dusk Studio MIDI out"), engine (e) {}
        void run() override;
    private:
        AudioEngine& engine;
    };
    MidiOutPump midiOutPump { *this };

    void queueMidiOutBlock (int port, const juce::MidiBuffer& events,
                            double sampleRate) noexcept;
    void drainMidiOutQueue();

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

    // Write the finished master mix (mixL/mixR) to its configured device output
    // pair. Accumulates, so an aux lane routed to the same pair sums in rather
    // than being clobbered. Default (-1) maps to the first pair (outputs 1-2).
    void writeMasterMixToOutput (float* const* outputChannelData,
                                 int numOutputChannels, int numSamples) noexcept;

    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };

    // DUSKSTUDIO_PERF=1: coarse per-section wall-time attribution for the
    // callback. The audio thread adds tick deltas into relaxed atomics at
    // six section boundaries (one branch + one fetch_add each when
    // enabled, a single cached-bool branch when not); a 2 s message-thread
    // timer prints the table to stderr and zeroes the counters.
    struct PerfSections
    {
        enum Section { kPre = 0, kStrips, kMeterRecordTail,
                       kBuses, kAuxes, kMasterOut, kNumSections };
        std::array<std::atomic<juce::int64>, kNumSections> ticks {};
        std::atomic<juce::int64> totalTicks { 0 };
        std::atomic<juce::int64> blocks     { 0 };
        std::atomic<bool> enabled { false };
    };
    PerfSections perf;
    class PerfReporter;
    std::unique_ptr<PerfReporter> perfReporter;

    // Process gate state (see suspendProcessing). The callback increments
    // callbacksInFlight around its body; suspend raises the flag and waits for
    // the counter to hit zero, after which it owns every buffer the callback
    // touches until resume.
    std::atomic<bool>   processingSuspended { false };
    std::atomic<int>    callbacksInFlight   { 0 };
    // Silent-output diagnostics. The audio callback only bumps these atomics
    // (RT-safe); diagTimer drains them on the message thread and emits the
    // stderr line — stdio locks must never touch the audio thread. A silent-
    // output stall is otherwise opaque without a debugger.
    std::atomic<juce::int64> earlyOutBlocks  { 0 };   // GATED (processingSuspended) callbacks
    std::atomic<juce::int64> silentBlocks    { 0 };   // undersized-buffer SILENT callbacks
    // size<<32 | delivered, packed into ONE atomic so the timer reads a
    // consistent pair instead of a torn mix of two different SILENT events.
    std::atomic<juce::uint64> silentDims      { 0 };

    // Drains the silent-output diagnostics off the audio thread. A nested Timer
    // member keeps AudioEngine's (final) base list unchanged; it ticks at 1 Hz
    // and prints only when a counter has advanced since the last report.
    void drainCallbackDiagnostics();
    struct CallbackDiagnosticTimer : juce::Timer
    {
        explicit CallbackDiagnosticTimer (AudioEngine& o) : owner (o) {}
        void timerCallback() override { owner.drainCallbackDiagnostics(); }
        AudioEngine& owner;
    };
    CallbackDiagnosticTimer diagTimer { *this };
    juce::int64 lastReportedGated  = 0;   // diagTimer (message thread) only
    juce::int64 lastReportedSilent = 0;

    std::atomic<int>    xrunCount         { 0 };
    // Device xrun count at the last resetXRunCounts(); subtracted in
    // getBackendXRunCount. Cleared on device start (the device's own
    // counter restarts at 0 there).
    std::atomic<int>    backendXrunBaseline { 0 };
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

    // Set once in the constructor by the busy-device fallback; drained by
    // consumeStartupDeviceMessage() after construction. Message-thread only.
    juce::String        startupDeviceMessage_;

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
