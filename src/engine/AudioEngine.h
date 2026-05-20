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
// Phase 2 engine: input -> channel strip (live or playback source) -> aux/master.
// Adds Transport state, recording (RecordManager), and playback (PlaybackEngine).
class AudioEngine final : public juce::AudioIODeviceCallback,
                            public juce::MidiInputCallback,
                            public juce::ChangeBroadcaster
{
public:
    // Workflow stages - drives which signal flow the audio callback runs
    // AND which view the UI shows. Recording / Mixing / Aux share the live
    // track-to-master path (input + disk playback into channel strips →
    // bus / aux lanes / master); only the visible UI changes between them.
    // Mastering swaps the signal flow entirely for stereo file →
    // MasteringChain → output.
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

    // Re-enumerate MIDI input devices (USB MIDI hot-plug, USB unplug,
    // virtual port appear/disappear). Detaches the audio + MIDI
    // callbacks, rebuilds the per-input collector list, then re-attaches
    // - so the audio thread is briefly silent during the swap rather than
    // racing the mutation. Sends a ChangeBroadcaster notification on
    // completion so listeners (per-strip MIDI dropdowns) can rebuild
    // their device lists. Message-thread only.
    void refreshMidiInputs();

    // Snapshot of the current MIDI input devices. Used by UI components
    // (channel-strip MIDI dropdowns) to populate themselves. Returns the
    // same list the engine handed to the audio device manager - indices
    // align with Track::midiInputIndex semantics. Message-thread only;
    // mutated only inside refreshMidiInputs() / rebuildMidiInputBank().
    const juce::Array<juce::MidiDeviceInfo>& getMidiInputDevices() const noexcept
    {
        return midiInputDevices;
    }

    // Mirror of getMidiInputDevices for the output side. UI components
    // (channel-strip MIDI-output dropdown, when a track is in Midi mode)
    // populate themselves from this list. Indices align with
    // Track::midiOutputIndex semantics. Message-thread only.
    const juce::Array<juce::MidiDeviceInfo>& getMidiOutputDevices() const noexcept
    {
        return midiOutputDevices;
    }

    // Synthetic "Virtual Keyboard (Focal)" entry appended to midiInputDevices
    // by rebuildMidiInputBank. The VirtualKeyboardComponent pushes typed-note
    // events into this collector via addMessageToQueue (lock-free); the audio
    // thread drains it through the same per-input → per-track filter as real
    // hardware. Returns nullptr if the bank hasn't been built yet.
    juce::MidiMessageCollector* getVirtualKeyboardCollector() noexcept;

    // Lazy-open a MIDI output port and start its background-delivery
    // thread. Called by the message thread when a track is routed to an
    // output (channel-strip dropdown onChange, post-session-load). Cheap
    // no-op when the index is already open. Returns false on out-of-range
    // index or if openDevice fails (a warning is printed in that case).
    // Message-thread only - opens an OS handle and spawns a thread.
    //
    // Lazy because opening every available output at startup blocks the
    // main thread (each ALSA snd_seq_connect_to is synchronous) and
    // spawns N threads regardless of whether any track uses them. Most
    // sessions touch zero or one output port.
    bool ensureMidiOutputOpen (int index);

    // Walk every track and open the MIDI output port each one is routed
    // to. Called once after SessionSerializer::load has resolved the
    // saved identifiers. Skips tracks with midiOutputIndex == -1.
    // Message-thread only.
    void openConfiguredMidiOutputs();

    // Single per-app UndoManager. Owns the stack of UndoableActions for
    // region edits and any other undoable mutation we add later. Touched
    // only on the message thread. It's a ChangeBroadcaster - TapeStrip
    // registers to repaint when the stack moves.
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    // App-lifetime region clipboard. Empty `region.file` means nothing
    // to paste. Only one slot - multi-region or cross-track multi-select
    // are post-MVP.
    struct RegionClipboard
    {
        bool        hasContent = false;
        int         sourceTrack = -1;
        AudioRegion region;
    };
    RegionClipboard& getRegionClipboard() noexcept { return regionClipboard; }

    // Per-strip access - UI components reach into the engine to call
    // PluginSlot::loadFromFile etc. on the message thread. The audio path
    // still owns the strips; this is just a typed accessor. Bounds
    // checking via jassert keeps debug builds loud about out-of-range
    // indices; release builds rely on the caller respecting Session::
    // kNumTracks / kNumBuses / kNumAuxLanes.
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
    // Legacy alias kept so existing call sites (RegionEditActions,
    // ConsoleView) compile without churn. Forwards to getChannelStrip.
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

    // Convenience for the UI; runs on the message thread. Coordinates the
    // RecordManager / PlaybackEngine state changes around Transport.
    void play();
    void stop();
    void record();

    // Transport-cluster jumps. Message-thread only. The marker variants
    // walk session.markers (already kept sorted by timelineSamples) and
    // pick the nearest neighbour relative to the current playhead. With
    // no markers, jumpToPrevMarker hits zero and jumpToNextMarker is a
    // no-op (matches Tascam behaviour - no overshoot beyond known points).
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
    // Fires from the audio thread when the backend can no longer service
    // I/O (USB / Bluetooth unplug; ALSA -ENODEV; macOS interrupt). JUCE
    // doesn't guarantee a follow-up audioDeviceStopped after this, so we
    // do the same flush-record-and-stop-transport dance to keep
    // recordings recoverable and prevent transport from "rolling" with
    // a dead audio backend.
    void audioDeviceError (const juce::String& errorMessage) override;

    // juce::MidiInputCallback. Bound to AudioDeviceManager via
    // addMidiInputDeviceCallback("", this) — empty deviceIdentifier means
    // "all enabled MIDI inputs". The callback fires from JUCE's MIDI input
    // thread (NOT the audio thread); we route to a per-input collector so
    // the audio thread can drain a per-block MidiBuffer without locks.
    void handleIncomingMidiMessage (juce::MidiInput* source,
                                       const juce::MidiMessage& message) override;

    // Self-test entry: prepare engine state (strip/aux/master DSP, mix
    // buffers) for a given sample rate and block size WITHOUT a real
    // AudioIODevice. The pipeline self-test detaches the engine from
    // deviceManager and calls audioDeviceIOCallbackWithContext directly with
    // synthetic input/output buffers. After the test, prepareForSelfTest can
    // be called again with the live device's params (or audioDeviceAboutToStart
    // will be invoked when the engine is re-attached).
    void prepareForSelfTest (double sampleRate, int blockSize);

    // Test-only hook: stage a MidiBuffer that the next
    // `audioDeviceIOCallbackWithContext` call will merge into
    // `perInputMidi[inputIdx]` AFTER the collector drain, so the
    // injected events flow through the same per-track filter and
    // strip path that real MIDI takes. Cleared after one block. Used
    // by the headless instrument-pipeline test in DuskStudioApp.cpp; does
    // nothing at runtime under normal device operation.
    void stageTestMidiInjection (int inputIdx, juce::MidiBuffer events);

    // Plugin-slot persistence - message-thread bookends around
    // SessionSerializer::save / ::load. publish copies each PluginSlot's
    // current description + state into its Track's pluginDescriptionXml /
    // pluginStateBase64 fields so the serializer can pick them up.
    // consume reads those Track fields back and asks each PluginSlot to
    // re-create + restore.
    //
    // audioCallbackDetached: pass true ONLY when the caller has already
    // removed this engine from the AudioDeviceManager's callback list.
    // The atomic-park sleep that defends against audio-thread re-entry
    // becomes a no-op (there's no audio thread to wait for), which on
    // a session with several heavy plugins drops total message-thread
    // block time from hundreds of milliseconds to roughly the cost of
    // the plugin-state I/O alone. Default false matches the autosave
    // path where the audio thread is still live.
    void publishPluginStateForSave (bool audioCallbackDetached = false);
    void consumePluginStateAfterLoad();

    // Failures captured during the most recent consumePluginStateAfterLoad
    // call. Each entry has a human-readable location ("Track 3",
    // "Aux 1 / slot 1") and the plugin name parsed from the saved
    // description XML. Empty when every slot restored cleanly. Caller
    // (MainComponent's load path) surfaces this as a single summary
    // AlertWindow rather than one popup per failure.
    struct PluginLoadFailure
    {
        juce::String location;
        juce::String pluginName;
    };
    const std::vector<PluginLoadFailure>& getLastPluginLoadFailures() const noexcept
    {
        return lastPluginLoadFailures;
    }

    // Suspend every loaded plugin (track + aux + master tape) by calling
    // releaseResources on each instance. Used by the shutdown path to
    // flip every plugin's IComponent::setActive(false) BEFORE the engine
    // starts destructing. Diva's terminate() (called inside
    // ~VST3PluginInstance) attempts callbacks back into the host's
    // VST3 context; doing them on an already-inactive plugin is safe,
    // but doing them while the plugin still believes it's rendering
    // has been observed to abort with __cxa_pure_virtual. Idempotent;
    // calling on an already-released plugin is a no-op. Caller MUST
    // have already removed the audio callback - this is not safe to
    // call while processBlock could fire.
    void releaseAllPluginResources();

    // Process-shutdown only: relinquish ownership of every loaded
    // AudioPluginInstance without destroying any of them. Walks every
    // ChannelStrip + every aux-lane plugin slot and calls
    // PluginSlot::leakInstanceForShutdown(). Use case: bypass plugin-
    // side destructor crashes on Linux (e.g. u-he Diva's
    // ~AM_VST3_ViewInterface) by skipping plugin destruction entirely
    // - the OS reclaims the leaked memory at process exit. Must be
    // called AFTER releaseAllPluginResources() so the plugins have
    // stopped processing, and only ever during the final shutdown
    // sequence (per-call leaks accumulate otherwise).
    void leakAllPluginInstancesForShutdown();

    // Transport (loop + punch) persistence - same publish/consume bookend.
    // The atomics live on Transport for audio-thread access; the serializer
    // sees only Session, so we mirror them onto Session.savedLoop* /
    // savedPunch* before save and apply them back after load.
    void publishTransportStateForSave();
    void consumeTransportStateAfterLoad();

    double getCurrentSampleRate() const noexcept { return currentSampleRate.load (std::memory_order_relaxed); }
    int    getCurrentBlockSize() const noexcept  { return currentBlockSize.load  (std::memory_order_relaxed); }

    // Engine-side xrun count: callbacks whose wall-clock time exceeded the
    // buffer's audio time. Detected at the end of each callback in
    // audioDeviceIOCallbackWithContext.
    int    getXRunCount() const noexcept         { return xrunCount.load         (std::memory_order_relaxed); }

    // Smoothed CPU usage as a 0..1 fraction of the buffer's wall-clock
    // budget consumed by audioDeviceIOCallbackWithContext. 1.0 means the
    // callback consumed the entire block period; xruns are imminent above
    // ~0.85. Updated every callback with a one-pole LPF so the UI doesn't
    // jitter on per-block spikes.
    float  getCpuUsage() const noexcept          { return cpuUsage.load          (std::memory_order_relaxed); }

    // Backend-side xrun count (e.g. ALSA snd_pcm_recover EPIPE events). Read
    // from AudioIODevice::getXRunCount on the message thread; returns 0 if no
    // device is currently open. Independent from the engine-side counter
    // above - backend xruns happen when the kernel/driver couldn't hand us a
    // buffer in time, even if our callback would have been fast.
    int    getBackendXRunCount() const noexcept;

    // True when the currently-open audio device has at least one active
    // output channel. False is the failure mode where a per-device ALSA name
    // (e.g. "UMC1820, USB Audio; Front output / input") is selected while
    // PipeWire is running - JUCE/PipeWire opens it with 0 output channels,
    // the engine writes its master mix to a buffer with no destinations, and
    // the user gets silent output with no error. Detected in
    // audioDeviceAboutToStart and surfaced by SystemStatusBar.
    bool   hasUsableOutputs() const noexcept { return usableOutputs.load (std::memory_order_relaxed); }

private:
    Session& session;
    juce::AudioDeviceManager deviceManager;

    Transport       transport;
    // Playhead initialized in the constructor body once tempoBpm /
    // currentSampleRate addresses are known - field declarations
    // run before constructor body so we can't pass &session.tempoBpm
    // here without manual ordering. Pointer to keep the field a
    // simple owned object initialized later.
    std::unique_ptr<DuskStudioPlayHead> playHead;
    RecordManager   recordManager   { session };

    // Populated by consumePluginStateAfterLoad. Message-thread only.
    std::vector<PluginLoadFailure> lastPluginLoadFailures;
    PlaybackEngine  playbackEngine  { session };
    PluginManager   pluginManager;  // shared across all per-channel PluginSlots
    juce::UndoManager undoManager;  // region edits + any future undoable mutation
    RegionClipboard   regionClipboard;

    MasteringPlayer  masteringPlayer;
    MasteringChain   masteringChain;
    Metronome        metronome;
    PitchDetector    pitchDetector;
    // External MIDI Clock sync. The engine drains the per-block buffer
    // for session.syncSourceInputIdx (set by the message thread via the
    // Audio Settings panel) and feeds clock / Start / Stop bytes here.
    // The receiver derives a smoothed BPM + a rolling hint that the UI
    // reads via session.externalBpm / externalSyncRolling atoms. v1 is
    // tempo-only - the rolling hint is exposed but doesn't drive the
    // engine's Transport state machine yet.
    MidiSyncReceiver       midiSyncReceiver;
    MidiTimeCodeReceiver   midiTimeCodeReceiver;
    MidiClockEmitter     midiClockEmitter;
    MidiTimeCodeEmitter  midiTimeCodeEmitter;
    // Scratch buffer the emitter writes F8/FA/FC bytes into before the
    // engine hands it off to MidiOutput::sendBlockOfMessages. Allocated
    // once via clear()/reserve-on-first-use semantics of juce::MidiBuffer.
    juce::MidiBuffer midiClockOutScratch;
    int              lastSyncOutputIdx = -1;

    // Monotonic sample clock the sync receiver timestamps clock ticks
    // against. Increments by numSamples each block; reset when the
    // session's syncSourceInputIdx changes (so old interval history
    // from the previous source doesn't poison the new source's BPM
    // computation). transport.getPlayhead() can jump (Cmd-jump, loop
    // wrap) - not safe to use for inter-tick interval math.
    juce::int64 midiSyncSampleClock = 0;
    int         lastSyncSourceIdx   = -1;
    // Edge detector for external transport chase. We act on the
    // rolling-flag transition (false -> true = Start/Continue,
    // true -> false = Stop), not the steady state, so a long Start
    // signal doesn't keep restarting the transport every block.
    bool        lastExtRolling      = false;
    // MTC chase state.
    //  lastMtcRolling   : rolling-edge detector. Updated EVERY block
    //                     regardless of chase-enabled state so the edge
    //                     detector stays current across toggle on/off.
    //  lastChaseEnabled : tracks the toggle. On false→true transition
    //                     while master is rolling, we force a re-lock
    //                     (set lastMtcRolling=false so next block fires
    //                     the rising-edge initial-lock path).
    //  mtcDriftWindowFrames : count of consecutive MTC FRAMES (not
    //                     audio blocks) where |transport - mtc| > tol.
    //                     lastSeenMtcFrames gates the increment so the
    //                     counter only ticks when MTC actually advances.
    bool        lastMtcRolling      = false;
    bool        lastChaseEnabled    = false;
    int         mtcDriftWindowFrames = 0;
    juce::int64 lastSeenMtcFrames    = -1;
    std::atomic<Stage> stage { Stage::Mixing };

    std::array<ChannelStrip, Session::kNumTracks> strips;
    std::array<BusStrip,  Session::kNumBuses> busStrips;
    std::array<AuxLaneStrip, Session::kNumAuxLanes> auxLaneStrips;
    MasterBus master;

    std::vector<float> mixL, mixR;
    std::array<std::vector<float>, Session::kNumBuses> busL, busR;
    std::array<std::vector<float>, Session::kNumAuxLanes> auxLaneL, auxLaneR;
    std::vector<float> playbackScratch;   // per-track playback L (or mono)
    std::vector<float> playbackScratchR;  // per-track playback R (stereo regions only)
    // Read-only silent buffer fed to strips with a generator-style
    // insert (e.g. signal generator) when the track has no audio
    // source. Lets the insert plugin emit its output even when there's
    // no live input or disk playback to drive the channel.
    std::vector<float> silentInputScratch;

    // MIDI input plumbing. One MidiMessageCollector per registered MIDI
    // input device (parallel to the device list). The MIDI input thread
    // calls handleIncomingMidiMessage which addMessageToQueue's into the
    // matching collector; the audio thread drains each collector per
    // block via removeNextBlockOfMessages into perInputMidi[i] — both
    // sides are lock-free per JUCE's MidiMessageCollector contract.
    // Identifiers are cached so a hot-plug re-enumeration can keep
    // existing collectors aligned to their devices.
    juce::Array<juce::MidiDeviceInfo> midiInputDevices;
    std::vector<std::unique_ptr<juce::MidiMessageCollector>> midiInputCollectors;
    std::vector<juce::MidiBuffer> perInputMidi;
    juce::MidiBuffer perTrackMidiScratch;  // reused per-block per-track filter target

    // Index into midiInputCollectors of the synthetic Virtual Keyboard
    // collector, recomputed every rebuildMidiInputBank so hot-plug doesn't
    // invalidate it. -1 until the first rebuild completes.
    int virtualKeyboardCollectorIndex { -1 };

    // Backing store for stageTestMidiInjection. SPSC handoff: the message
    // thread writes the buffer + index, then publishes via testInjectReady
    // (release); the audio callback reads testInjectReady (acquire), consumes
    // the buffer into perInputMidi[testInjectInputIdx], and clears the flag.
    // Producer must not touch testInjectMidi while testInjectReady==true.
    // Empty in production (no test hook calls staged); the relaxed flag load
    // makes the production cost a single load + branch per block.
    juce::MidiBuffer  testInjectMidi;
    std::atomic<int>  testInjectInputIdx { -1 };
    std::atomic<bool> testInjectReady    { false };

    // External MIDI output bank (parallel to the input bank). For tracks
    // that route to a hardware synth instead of (or in addition to) a
    // loaded instrument plugin. Built/torn-down inside refreshMidiInputs's
    // detach-rebuild-reattach fence so audio-thread reads are safe under
    // the same protocol as the input side. JUCE's MidiOutput owns its own
    // background thread for actual delivery, so audio-thread sends from
    // sendBlockOfMessages don't block on the OS port.
    juce::Array<juce::MidiDeviceInfo> midiOutputDevices;
    std::vector<std::unique_ptr<juce::MidiOutput>> midiOutputs;

    // Per-track snapshot of the previous block's midiInputIndex so we can
    // detect when the user swaps a track's MIDI input mid-play and emit
    // an All-Notes-Off + Sustain-Off flush on the new input's first block.
    // Without this, held notes from the previous device would keep
    // ringing on the synth (the synth never sees a Note Off because the
    // events stop arriving from the now-unrouted source).
    std::array<int, Session::kNumTracks> lastMidiInputIndex {};

    // Initial wiring of MIDI input collectors. Called once from the
    // constructor BEFORE the audio + MIDI callbacks are registered, and
    // again from refreshMidiInputs() WHILE those callbacks are detached.
    // The detach-rebuild-reattach fence in refreshMidiInputs() is what
    // makes vector mutation safe; this function itself is unguarded and
    // assumes the caller has handled the callback-detachment.
    void rebuildMidiInputBank();

    // Mirror of rebuildMidiInputBank for the output side. Snapshots the
    // current MIDI output device list and resets midiOutputs to a parallel
    // vector of nullptrs - actual port-open is lazy via
    // ensureMidiOutputOpen() so startup never blocks on snd_seq_connect_to
    // and the per-port delivery threads aren't spawned for ports nobody
    // uses. Called from the same detached window as the input rebuild so
    // the audio thread doesn't observe a half-built outputs vector.
    void rebuildMidiOutputBank();

    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };
    std::atomic<int>    xrunCount         { 0 };
    std::atomic<float>  cpuUsage          { 0.0f };

    // Per-callback device-I/O pointer cache. Populated at the top of
    // audioDeviceIOCallbackWithContext and forwarded to ChannelStrip /
    // AuxLaneStrip so the hardware-insert side of each slot (Phase 3)
    // can read returns from + write sends to specific device channels.
    // Pointers are valid for the duration of one callback only;
    // strips must consume them synchronously and never cache across blocks.
    const float* const* currentDeviceInputs   = nullptr;
    int                 numCurrentDeviceInputs  = 0;
    float* const*       currentDeviceOutputs  = nullptr;
    int                 numCurrentDeviceOutputs = 0;

    // Cached 1.0 / juce::Time::getHighResolutionTicksPerSecond(). The
    // tick->seconds conversion runs twice per audio callback (xrun watchdog
    // entry + exit). highResolutionTicksToSeconds() does an internal divide
    // every call; multiplying against this cached reciprocal avoids both
    // divides on the audio thread. Set in prepareForSelfTest /
    // audioDeviceAboutToStart, never written from the audio thread.
    double secondsPerTick { 0.0 };

    // True until proven false in audioDeviceAboutToStart (default true so
    // SystemStatusBar shows nothing alarming pre-open).
    std::atomic<bool>   usableOutputs     { true };

    // Where the active record session's WAV starts on the timeline (= the
    // first sample committed to disk). The audio callback uses this to
    // gate writes - under count-in the playhead is rolled back BEFORE this
    // sample so the metronome can tick a pre-roll, and writes are skipped
    // until the playhead catches up. INT64_MIN sentinel = no record active.
    std::atomic<juce::int64> activeRecordStart { std::numeric_limits<juce::int64>::min() };

    // Audio-thread-only state for hanging-note protection. wasRolling holds
    // the previous callback's transport-rolling state; lastBlockEndSample is
    // the previous block's exclusive end. Together they let us detect two
    // events that must trigger a per-block "All Notes Off" flush:
    //   • Transport rolling -> stopped: any held note from playback or
    //     live input would otherwise sustain forever.
    //   • Playhead discontinuity (loop wrap, scrub): notes whose Note Off
    //     event is after the jump never fire, so the synth gets stuck.
    bool         wasRolling          = false;
    juce::int64  lastBlockEndSample  = 0;
};
} // namespace duskstudio
