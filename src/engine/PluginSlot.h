#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <atomic>

#if DUSKSTUDIO_HAS_OOP_PLUGINS
 #include "ipc/RemotePluginConnection.h"
 #include <memory>
#endif

namespace duskstudio
{
class PluginManager;

// One plugin instance per slot, audio-thread-safe processing.
// load/unload/setEnabled: message thread.
// process*: audio thread.
// Instance pointer is atomic; old instances are released on the message
// thread after swap because the destructor isn't RT-safe. Audio thread
// sees either nullptr (bypass) or a fully-prepared instance.
//
// PluginSlot inherits juce::Timer so the audio-thread param-write path
// (setParamNormalised → SPSC FIFO push) can be drained on the message
// thread without ever blocking, allocating, or invoking JUCE parameter
// setters from the render context. See timerCallback() + paramFifo.
class PluginSlot : private juce::Timer
{
public:
    PluginSlot();
    ~PluginSlot() override;

    // Pointer so the slot can sit inside default-constructed containers.
    // Must be set before any load.
    void setManager (PluginManager& mgr) noexcept { manager = &mgr; }

    // Apply the host playhead to the loaded plugin. Tempo-locked plugins
    // may report different latency after the playhead binds, so we
    // re-cache latency here.
    void setHostPlayHead (juce::AudioPlayHead* ph) noexcept
    {
        hostPlayHead = ph;
        if (auto* p = currentInstance.load (std::memory_order_acquire))
        {
            p->setPlayHead (ph);
            cachedLatencySamples.store (p->getLatencySamples(),
                                          std::memory_order_relaxed);
        }
    }

    PluginManager& getManagerForUi() const noexcept { jassert (manager != nullptr); return *manager; }

    void prepareToPlay (double sampleRate, int blockSize);
    void releaseResources();

    // Process-shutdown only. Drops ownership without destructing — some
    // Linux plugins abort the process from their destructor (e.g. u-he
    // Diva). The OS reclaims the leaked memory at exit. NEVER call
    // outside shutdown.
    void leakInstanceForShutdown();

    bool loadFromFile (const juce::File& pluginFile, juce::String& errorMessage);
    bool loadFromDescription (const juce::PluginDescription& desc,
                                juce::String& errorMessage);
    void unload();

    bool isLoaded() const noexcept;
    juce::String getLoadedName() const;

    void setBypassed (bool shouldBypass) noexcept { bypassed.store (shouldBypass, std::memory_order_relaxed); }
    bool isBypassed() const noexcept             { return bypassed.load (std::memory_order_relaxed); }

    // In-place process. Empty MIDI buffer for FX inserts; per-track MIDI
    // for instrument plugins. Mono-only plugins on stereo input are
    // averaged to mono and broadcast back to both channels.
    //
    // Time-budget watchdog: if wall-clock exceeds kTimeBudgetFraction of
    // the block's audio time for kOverrunStreak blocks in a row, the
    // plugin is auto-bypassed and the user has to re-enable manually.
    void processMonoBlock (float* monoData, int numSamples,
                           juce::MidiBuffer& midiMessages) noexcept;
    void processStereoBlock (float* L, float* R, int numSamples,
                             juce::MidiBuffer& midiMessages) noexcept;

    bool wasAutoBypassed() const noexcept { return autoBypassed.load (std::memory_order_relaxed); }
    void clearAutoBypass() noexcept;

    // OOP child process exited (crashed or killed). Always false without
    // OOP support.
    bool wasCrashed() const noexcept
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return remoteCrashed.load (std::memory_order_relaxed);
       #else
        return false;
       #endif
    }

    bool isRemote() const noexcept
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return currentRemote.load (std::memory_order_acquire) != nullptr;
       #else
        return false;
       #endif
    }

    // OOP editor RPC. No-op for in-process / non-OOP platforms.
    // windowIdOut: native window handle (X11 Window packed as uint64_t).
    bool showRemoteEditor (std::uint64_t& windowIdOut, int& widthOut, int& heightOut);
    bool hideRemoteEditor();
    bool resizeRemoteEditor (int width, int height);

    // Message-thread only — audio thread may swap the instance.
    juce::AudioPluginInstance* getInstance() const noexcept
    {
        return currentInstance.load (std::memory_order_acquire);
    }

    // Plugin's self-report. False when no plugin loaded.
    bool isLoadedPluginInstrument() const;

    // Cached at load. AudioPluginInstance::getLatencySamples isn't
    // documented as RT-safe (plugins may take locks), so we cache on
    // the message thread and the audio thread reads the atom.
    int getLatencySamples() const noexcept
    {
        return cachedLatencySamples.load (std::memory_order_relaxed);
    }

    // -1 = no parameter touched since load. Driven by a parameter
    // listener; read by MIDI Learn so the user can bind "the knob I
    // just touched" without picking from a list.
    int getLastTouchedParamIndex() const noexcept
    {
        return lastTouchedParamIndex.load (std::memory_order_relaxed);
    }

    // Audio-thread entry. Lock-free push into the SPSC paramFifo; the
    // actual JUCE parameter setter runs on the message thread inside
    // timerCallback(). FIFO-full = silent drop (extremely rare; the
    // 256-deep ring covers a full second of 30 Hz drains under steady-
    // state knob-twiddle traffic).
    void setParamNormalised (int paramIndex, float value01) noexcept;

    // Session save/restore. Both NON-CONST: they atomically park
    // currentInstance to null while reading state, because JUCE's
    // contract is that processBlock and getStateInfo must not overlap
    // (Diva crashes on this).
    //
    // parkSleepMs: how long to wait between null-store and state read
    // so the audio thread observes the parked pointer. Default 25 ms
    // covers a 1024-sample block at 44.1 kHz. Pass 0 only when the
    // audio callback is already detached (shutdown).
    juce::String getDescriptionXmlForSave (int parkSleepMs = 25);
    juce::String getStateBase64ForSave   (int parkSleepMs = 25);

    bool restoreFromSavedState (const juce::String& descriptionXml,
                                  const juce::String& stateBase64,
                                  juce::String& errorMessage);

    // True when restore couldn't re-instantiate (plugin missing /
    // moved / unsupported). Saved description + state are preserved so
    // a subsequent save round-trips them — user doesn't lose state
    // because the plugin wasn't available at load time.
    bool         isOffline() const noexcept;
    juce::String getOfflineName() const;

    // Screenshot-harness only: force the empty slot into the offline display
    // state with a synthetic saved description (so the manual's offline-slot
    // figure can be captured without a real missing-plugin session). Pass an
    // empty string to clear it again. Never called in normal operation.
    void setOfflineForCapture (const juce::String& displayName);

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Mac dual-load shell-instance API. Loads a parent-process copy of
    // the plugin solely to host its editor in the main app window while
    // the OOP child handles DSP. processBlock is NEVER called on the
    // shell instance; prepareToPlay is invoked so editor layout
    // matches the engine's current SR/BS, that's it.
    //
    // Architectural note: PluginSlot returns a raw juce::AudioProcessor
    // Editor* rather than calling duskstudio::platform::createInProcess
    // EditorHost itself because CLAUDE.md forbids src/engine/ including
    // src/ui/. The factory call lives in ChannelStripComponent (which
    // is allowed to include both the engine + UI layer). The
    // notifyShellEditorWrapper call closes the loop so PluginSlot can
    // refuse a second concurrent editor + defer releaseShellInstance
    // until the wrapper destructs.
    //
    // All five methods: message thread only. AudioProcessor lifecycle
    // is not RT-safe.
    bool ensureShellInstanceForEditor (juce::String& err);
    juce::AudioProcessorEditor* createShellEditor();
    void notifyShellEditorWrapper (juce::Component* wrapper) noexcept;
    void releaseShellInstance();

    // 3c-3b first-open state sync. Fetches the child's current
    // AudioProcessor state via the existing GetState IPC and applies
    // it to the shell instance via setStateInformation BEFORE the
    // editor wrapper is shown. Guarantees the parent-process editor
    // reflects what the OOP DSP is actually running. Skips silently
    // (returns true) if the child has no state to share (empty blob).
    //
    // applyingFromMirror is held for the duration of setState
    // Information so the shell parameter listener doesn't echo every
    // freshly-applied param back to the child as a SetParam.
    bool syncShellStateFromChild (juce::String& err);
   #endif

private:
    PluginManager* manager = nullptr;

    std::unique_ptr<juce::AudioPluginInstance> ownedInstance;

    // Two-deep keep-alive: a single slot races itself when two rapid
    // Replace clicks fire within one audio block. With two slots the
    // deposed instance survives two full swaps before destruction, so
    // any pointer the audio thread captured before currentInstance was
    // nulled can complete its block.
    std::array<std::unique_ptr<juce::AudioPluginInstance>, 2> previousInstances;

    std::atomic<juce::AudioPluginInstance*> currentInstance { nullptr };
    std::atomic<bool> bypassed { false };
    std::atomic<bool> autoBypassed { false };

    // Without zeroing latency too, the MIDI scheduler keeps shifting
    // note timing forward by the plugin's pre-bypass latency.
    void engageAutoBypass() noexcept
    {
        autoBypassed       .store (true, std::memory_order_relaxed);
        cachedLatencySamples.store (0,    std::memory_order_relaxed);
    }

    double preparedSampleRate = 0.0;
    int    preparedBlockSize  = 0;
    double secondsPerTick = 0.0;

    juce::AudioPlayHead* hostPlayHead = nullptr;

    std::atomic<int> cachedLatencySamples { 0 };
    std::atomic<int> lastTouchedParamIndex { -1 };

    class LastTouchedListener final : public juce::AudioProcessorParameter::Listener
    {
    public:
        explicit LastTouchedListener (std::atomic<int>& atomRef) noexcept
            : indexAtom (atomRef) {}
        void parameterValueChanged (int parameterIndex, float) override
        {
            indexAtom.store (parameterIndex, std::memory_order_relaxed);
        }
        void parameterGestureChanged (int, bool) override {}
    private:
        std::atomic<int>& indexAtom;
    };
    std::unique_ptr<LastTouchedListener> lastTouchedListener;

    // Watchdog state (audio-thread only).
    // blocksSinceLoad skips the budget check while cold caches / lazy
    // init / oversampler ramps warm up — without it, look-ahead
    // limiters and reverbs trip on block 1 every time.
    // consecutiveOverruns requires N in a row before tripping so single
    // late blocks from scheduling jitter don't kill the plugin.
    int blocksSinceLoad     = 0;
    int consecutiveOverruns = 0;

    juce::AudioBuffer<float> stereoScratch;

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // OOP plugin routing. A slot stays in one mode for its lifetime
    // per load: switching modes requires unload + reload.
    std::unique_ptr<duskstudio::ipc::RemotePluginConnection> ownedRemote;
    std::atomic<duskstudio::ipc::RemotePluginConnection*>    currentRemote { nullptr };
    std::array<std::unique_ptr<duskstudio::ipc::RemotePluginConnection>, 2> previousRemotes;

    std::atomic<int>  remoteNumIn       { 0 };
    std::atomic<int>  remoteNumOut      { 0 };
    std::atomic<bool> remoteIsInstrument { false };
    std::atomic<bool> remoteCrashed { false };

    // Cached for getDescriptionXmlForSave (which can't
    // fillInPluginDescription on a remote instance).
    juce::String savedDescriptionXml;

    static constexpr int kReaperPeriodMs = 1000;
    class ReaperTimer final : public juce::Timer
    {
    public:
        explicit ReaperTimer (PluginSlot& s) : slot (s) {}
        void timerCallback() override { slot.pollRemoteReaper(); }
    private:
        PluginSlot& slot;
    };
    ReaperTimer reaperTimer { *this };
    void pollRemoteReaper();
   #endif

    // Stashed when restoreFromSavedState fails so a subsequent save
    // round-trips the original blob. Outside the OOP gate because
    // macOS has no OOP impl yet but offline restore is cross-platform.
    juce::String offlineDescriptionXml;
    juce::String offlineStateBase64;

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Mac dual-load shell-instance state. Loaded lazily on first editor
    // open; lives independent of ownedRemote (which holds the OOP child
    // connection). outstandingShellWrapper auto-nulls when the wrapper
    // Component destructs, so PluginSlot can detect the editor-closed
    // state without an explicit notification channel.
    std::unique_ptr<juce::AudioPluginInstance>    shellInstanceForEditor;
    juce::Component::SafePointer<juce::Component> outstandingShellWrapper;

    // Loop breaker: set while a remote-originated change is being
    // applied to the shell instance. The shell parameter listener
    // checks this on entry and skips its outbound setRemoteParam echo
    // when set. Atomic because the listener can fire on whichever
    // thread JUCE pumps it; the mirror-apply path is message-thread,
    // so a release store on entry pairs with the listener's acquire
    // load.
    std::atomic<bool> applyingFromMirror { false };

    class ShellParamListener final : public juce::AudioProcessorParameter::Listener
    {
    public:
        explicit ShellParamListener (PluginSlot& s) noexcept : slot (s) {}
        void parameterValueChanged (int paramIndex, float newValue) override;
        void parameterGestureChanged (int, bool) override {}
    private:
        PluginSlot& slot;
    };
    std::unique_ptr<ShellParamListener> shellParamListener;

    // Called by the ParamChangedSink the parent registered on
    // ownedRemote — runs on the message thread (callAsync marshalled).
    // Sets applyingFromMirror across the setValueNotifyingHost call so
    // the shell listener doesn't bounce the change back.
    void applyShellParamFromChild (int paramIndex, float value01) noexcept;
   #endif

    // SPSC param-write queue. Producer = audio thread (setParamNormalised);
    // Consumer = message thread (timerCallback). Pre-sized at construction
    // so no audio-thread allocation. Capacity must be a power of two for
    // juce::AbstractFifo's bitwise wrap, AND comfortably above the burst
    // size a user can generate by dragging a knob inside a single 30 Hz
    // drain interval (33 ms × 100 Hz controller stream ≈ 3 events; 256
    // gives ~85× headroom for chord-strike-plus-drag scenarios).
    //
    // ParamWrite is a trivial POD so the in-place store on the audio
    // thread is a single 12-byte write + an atomic finishedWrite — no
    // ctor, no dtor, no allocation. loadEpoch is stamped from
    // currentLoadEpoch at push time so the drain can discard entries
    // queued against a now-replaced plugin instance.
    struct ParamWrite
    {
        int           paramIndex = -1;
        float         value      = 0.0f;
        std::uint32_t loadEpoch  = 0;
    };
    static constexpr int kParamFifoCapacity = 256;
    juce::AbstractFifo                       paramFifo { kParamFifoCapacity };
    std::array<ParamWrite, kParamFifoCapacity> paramQueue {};

    // Bumped on every load / unload / restore. Audio thread reads it
    // when stamping a queued ParamWrite; message-thread drain discards
    // any entry whose loadEpoch != currentLoadEpoch (stale, target
    // plugin no longer matches the slot's current instance). Acquire
    // ordering on the audio-thread read pairs with the release fetch_add
    // performed by the message-thread mutators.
    std::atomic<std::uint32_t> currentLoadEpoch { 0 };

    // Drains paramFifo on the message thread. Called by juce::Timer at
    // 30 Hz; cheap when empty (one atomic load + branch).
    void timerCallback() override;

    // Message-thread-only. Applies one queued ParamWrite to the live
    // plugin instance. JUCE_ASSERT_MESSAGE_THREAD enforces the contract
    // so any future caller that tries to invoke this from the audio
    // thread trips in debug + selftest builds.
    void applyParamWriteOnMessageThread (const ParamWrite& pw) noexcept;
};
} // namespace duskstudio
