#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>

#if DUSKSTUDIO_HAS_OOP_PLUGINS
 #include "ipc/RemotePluginConnection.h"
 #include <memory>
#endif

namespace duskstudio
{
class PluginManager;

// A single plugin slot - holds at most one AudioPluginInstance and exposes
// audio-thread-safe processing methods. Designed for per-channel use:
// channel strips own a PluginSlot and call its `processMonoBlock` between
// phase-invert and EQ in their per-block processing.
//
// Threading rules (RT-critical):
//   • load() / unload() / setEnabled() run on the message thread only.
//   • processMonoBlock() / processStereoBlock() run on the audio thread.
//   • The instance pointer is wrapped in a std::atomic so swap-in / swap-out
//     are lock-free for the audio thread. Old instances are released on the
//     message thread after the swap (the instance destructor is NOT
//     RT-safe). The audio thread sees either nullptr (no plugin → bypass)
//     or a fully-prepared instance.
//
// MVP scope:
//   • No multi-bus support - mono in / mono out, with a stereo fallback when
//     a plugin can't do mono.
//   • No editor window management - that's a follow-up stage.
//   • No state save/restore via SessionSerializer yet - also follow-up.
class PluginSlot
{
public:
    PluginSlot() = default;
    ~PluginSlot();

    // Bind the manager that resolves plugin files. Must be called before
    // loadFromFile. Pointer rather than reference so the slot can be
    // default-constructed inside containers (e.g. as a ChannelStrip member,
    // which lives in std::array<ChannelStrip, 16> requiring default ctors).
    void setManager (PluginManager& mgr) noexcept { manager = &mgr; }

    // Cache the host's AudioPlayHead so every loaded plugin sees it via
    // setPlayHead. Called from AudioEngine::prepareForSelfTest after the
    // engine's playhead is constructed. Pass nullptr to clear. Safe to
    // call before / after a plugin is loaded - we apply on every load
    // path. Tempo-synced plugin features (arps, LFOs, delays) need this;
    // otherwise JUCE-hosted plugins fall back to a default 120 BPM.
    void setHostPlayHead (juce::AudioPlayHead* ph) noexcept
    {
        hostPlayHead = ph;
        if (auto* p = currentInstance.load (std::memory_order_acquire))
        {
            p->setPlayHead (ph);
            // Tempo-locked plugins (grid-quantised slicers, BPM-synced
            // delays) can derive their reported latency from the host
            // playhead's tempo. Re-cache after the binding so the PDC
            // aggregator picks up the post-playhead value instead of
            // the stale post-load number. OOP path doesn't surface
            // post-playhead latency over IPC yet — flagged in the
            // matching prepareToPlay comment.
            cachedLatencySamples.store (p->getLatencySamples(),
                                          std::memory_order_relaxed);
        }
    }

    // UI-side access to the bound manager - used by the ChannelStripComponent
    // plugin picker to walk KnownPluginList and trigger scans. Returns the
    // bound manager; binding must have happened first (asserts otherwise).
    PluginManager& getManagerForUi() const noexcept { jassert (manager != nullptr); return *manager; }

    // Message-thread API.
    void prepareToPlay (double sampleRate, int blockSize);
    void releaseResources();

    // Process-shutdown only: relinquish ownership of the loaded
    // AudioPluginInstance(s) without destroying them. The instances
    // remain in heap memory until the OS reclaims it on process exit.
    //
    // Why: some Linux plugins have buggy destructors that abort the
    // process on the way out (e.g. u-he Diva's ~AM_VST3_ViewInterface
    // calls a virtual that resolves to a pure virtual on the way up
    // the destruction chain). The plugin's IPluginBase::terminate()
    // hook may not run, but in exchange the process exits cleanly
    // (zero exit code, no SIGABRT, no coredump). Acceptable trade-off
    // because the OS reclaims the leaked memory immediately.
    //
    // Must NOT be called outside of process shutdown - leaks are
    // per-call and accumulate.
    void leakInstanceForShutdown();

    // Loads the plugin from a file. Returns true on success, false with
    // errorMessage populated on failure. Releases any previously-loaded
    // plugin first.
    bool loadFromFile (const juce::File& pluginFile, juce::String& errorMessage);

    // Same shape, but resolves via a previously-discovered PluginDescription
    // (e.g. one the user picked from the scanned KnownPluginList). Useful
    // when we want format-aware loading without having the user navigate to
    // the on-disk file path.
    bool loadFromDescription (const juce::PluginDescription& desc,
                                juce::String& errorMessage);

    // Unload - clears the slot. Audio thread sees nullptr next block.
    void unload();

    // True when an instance is currently loaded (and presumed processing).
    bool isLoaded() const noexcept;

    // Display name for the loaded plugin, or empty string if none.
    juce::String getLoadedName() const;

    // Bypass toggle - when true, processMonoBlock is a no-op even with a
    // plugin loaded. Useful for the time-budget-bypass mechanism described
    // in the spec for crash safety / CPU spikes.
    void setBypassed (bool shouldBypass) noexcept { bypassed.store (shouldBypass, std::memory_order_relaxed); }
    bool isBypassed() const noexcept             { return bypassed.load (std::memory_order_relaxed); }

    // Audio-thread API. Mono channel strip: feed the same buffer in and out;
    // the plugin processes it in place. If no plugin loaded or bypassed, the
    // buffer is unchanged.
    //
    // The buffer is treated as 1 channel of `numSamples`. For plugins that
    // require stereo (no mono bus support), we duplicate-and-process (L=R)
    // and average back to mono.
    //
    // `midiMessages` is forwarded to the plugin's processBlock. For audio-
    // effect inserts the caller passes an empty buffer; for instrument
    // plugins it carries the per-track-filtered MIDI events for the block.
    //
    // Stage-4 time-budget protection: each block is timed; if the plugin's
    // wall-clock exceeds `kTimeBudgetFraction` of the buffer's audio time
    // (e.g. 60 % of 21.3 ms at 1024/48k = 12.8 ms), the bypass flag is
    // automatically engaged. Engineering-grade safety: stops a misbehaving
    // plugin from freezing the audio thread even if it doesn't outright
    // crash. User can manually clear the auto-bypass via the plugin slot
    // UI (right-click → "Re-enable plugin"); we don't auto-recover because
    // the plugin would just freeze the thread again.
    void processMonoBlock (float* monoData, int numSamples,
                           juce::MidiBuffer& midiMessages) noexcept;

    // Stereo variant for aux/master buses. L and R are processed in place.
    // Mono-only plugins: average L+R into a temporary mono buffer, run the
    // plugin, write the result to both channels (so a mono reverb still
    // makes sense on a stereo send return).
    // Same time-budget watchdog as processMonoBlock.
    void processStereoBlock (float* L, float* R, int numSamples,
                             juce::MidiBuffer& midiMessages) noexcept;

    // True if the slot was AUTO-bypassed by the time-budget watchdog (as
    // distinct from manual setBypassed). UI shows this state so the user
    // knows why their plugin stopped affecting audio.
    bool wasAutoBypassed() const noexcept { return autoBypassed.load (std::memory_order_relaxed); }
    void clearAutoBypass() noexcept;

    // True when the OOP child process has exited (crashed or killed).
    // Distinct from wasAutoBypassed (which can also fire on CPU
    // overrun for an in-process plugin). UI uses this to show
    // "Plugin crashed - reload to recover" copy. Always false on
    // platforms without OOP support.
    bool wasCrashed() const noexcept
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return remoteCrashed.load (std::memory_order_relaxed);
       #else
        return false;
       #endif
    }

    // True when this slot's plugin currently runs out-of-process. The
    // UI uses this to branch its plugin-editor flow: in-process slots
    // get the existing AudioProcessorEditor path; OOP slots use
    // showRemoteEditor to fetch a native window ID for XEmbed embedding.
    bool isRemote() const noexcept
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return currentRemote.load (std::memory_order_acquire) != nullptr;
       #else
        return false;
       #endif
    }

    // Editor RPC pass-throughs (OOP only — no-op + return false on
    // other platforms / in-process slots). The UI calls these to bridge
    // the editor across the process boundary; on success, windowIdOut
    // is a host-OS native window handle (X11 Window on Linux, packed
    // as uint64_t).
    bool showRemoteEditor (std::uint64_t& windowIdOut, int& widthOut, int& heightOut);
    bool hideRemoteEditor();
    bool resizeRemoteEditor (int width, int height);

    // Plugin instance access for Stage 3 editor window. Returns nullptr if
    // no plugin loaded. Caller MUST be on the message thread; the audio
    // thread may swap the instance out between calls.
    juce::AudioPluginInstance* getInstance() const noexcept
    {
        return currentInstance.load (std::memory_order_acquire);
    }

    // True when the loaded plugin self-reports as an instrument
    // (synth / sampler / drum machine). False for effect plugins and
    // when no plugin is loaded. Message-thread only - fillInPluginDescription
    // can take internal locks inside the plugin, so it's not safe to call
    // from the audio callback.
    bool isLoadedPluginInstrument() const;

    // Reported latency of the loaded plugin in samples. 0 when no plugin
    // is loaded or the plugin reports no latency. Used by AudioEngine's
    // MIDI scheduler to push instrument events forward in time so the
    // plugin's delayed audio output lands on the correct timeline
    // sample.
    //
    // Reads a cached atomic - the underlying juce::AudioPluginInstance::
    // getLatencySamples() call is NOT documented as RT-safe (a plugin is
    // technically allowed to recompute latency inside processBlock and
    // some take internal locks), so we cache it on the message thread at
    // load / prepareToPlay time. The cache stays consistent because the
    // load path (loadFromFile / loadFromDescription / restoreFromSavedState
    // / unload) refreshes it before the new instance becomes
    // currentInstance, and prepareToPlay refreshes it when the engine
    // re-preps the slot after a sample-rate change.
    int getLatencySamples() const noexcept
    {
        return cachedLatencySamples.load (std::memory_order_relaxed);
    }

    // Index of the parameter the user most recently moved via the
    // plugin's own UI. -1 = no touch since the slot loaded. Driven by
    // a juce::AudioProcessorParameter::Listener wired on load + torn
    // down on unload. Read by the channel strip's right-click MIDI
    // Learn handler so the user can bind "the knob I just touched"
    // without picking from a parameter list.
    int getLastTouchedParamIndex() const noexcept
    {
        return lastTouchedParamIndex.load (std::memory_order_relaxed);
    }

    // Audio-thread-safe setter for the normalised value (0..1) of a
    // parameter by index. No-op when no plugin is loaded or the
    // index is out of range. Uses setValue (no notify) to avoid the
    // host-notify path's listener calls from the audio thread; the
    // plugin re-reads the new value on its next processBlock.
    void setParamNormalised (int paramIndex, float value01) noexcept;

    // Session save/restore. The XML form encodes a juce::PluginDescription
    // (uid + format + path + display name); the state blob is whatever
    // opaque bytes the plugin wants persisted (its getStateInformation
    // output). Both are empty strings when no plugin is loaded.
    //
    // NON-CONST because both internally atomic-park currentInstance to
    // nullptr while reading from the plugin: that prevents the audio
    // thread from re-entering processBlock on the same instance during
    // state I/O. JUCE's contract is that processBlock and getStateInfo
    // must not overlap. Several plugins (notably U-he Diva) crash hard
    // when the contract is violated - on Linux/Wayland the crash can
    // cascade into a Mutter compositor fault, but the root cause is the
    // same data race on every platform.
    //
    // parkSleepMs controls how long the message thread waits between
    // null-store and getStateInfo to give the audio thread time to
    // observe the parked pointer. Default = 25 ms (covers a 1024-sample
    // block at 44.1 kHz). Pass 0 when the caller has ALREADY detached
    // the audio callback (e.g. shutdown path) — there's no audio thread
    // to wait for, and skipping the sleep avoids the message-thread
    // starvation that has been observed to time-out compositor
    // responsiveness checks on save-then-quit flows with several heavy
    // plugins loaded.
    juce::String getDescriptionXmlForSave (int parkSleepMs = 25);
    juce::String getStateBase64ForSave   (int parkSleepMs = 25);

    // Re-create the plugin from the saved description and apply the state
    // blob. Returns true on success. Caller is responsible for ensuring
    // prepareToPlay has been called first; if the saved description doesn't
    // resolve to an installed plugin the slot is left empty.
    bool restoreFromSavedState (const juce::String& descriptionXml,
                                  const juce::String& stateBase64,
                                  juce::String& errorMessage);

    // True when restoreFromSavedState was called with a non-empty
    // description but the slot couldn't be re-instantiated (plugin
    // missing / moved / format unsupported). The original description
    // XML + state blob are preserved on the slot so a subsequent
    // session save round-trips them — the user does NOT lose their
    // saved plugin state just because the plugin wasn't available at
    // load time. Explicit loadFromFile / loadFromDescription / unload
    // clear the offline state.
    bool         isOffline() const noexcept;
    juce::String getOfflineName() const;

private:
    PluginManager* manager = nullptr;

    // Owning pointer - only the message thread mutates this. The audio
    // thread reads via the `currentInstance` atomic below.
    std::unique_ptr<juce::AudioPluginInstance> ownedInstance;

    // Two-deep keep-alive for deposed plugin instances. At swap time the
    // freshly-deposed instance moves into slot [0]; whatever was in [0]
    // rotates to [1]; the instance in [1] is destroyed. Two slots
    // because a single slot races itself: two rapid Replace clicks
    // within one audio block destroy the instance the audio thread is
    // still holding a pointer to (loaded via acquire before the
    // currentInstance.store(nullptr)). With two slots the deposed
    // instance survives two full swaps before destruction, so any
    // pointer the audio thread captured can complete its block.
    std::array<std::unique_ptr<juce::AudioPluginInstance>, 2> previousInstances;

    // Atomic view of `ownedInstance.get()` so the audio thread can
    // swap-load without a lock. Set after prepareToPlay completes.
    std::atomic<juce::AudioPluginInstance*> currentInstance { nullptr };
    std::atomic<bool> bypassed { false };
    std::atomic<bool> autoBypassed { false };  // tripped by the time-budget watchdog

    // Audio-thread helper: engage auto-bypass + zero cached latency in
    // one place. Without zeroing the latency cache, the AudioEngine's
    // MIDI scheduler keeps shifting note timing forward by the
    // plugin's reported pre-bypass latency, so events drift on every
    // overrun-trip. Restored on the next successful load (autoBypassed
    // cleared, cachedLatencySamples re-queried).
    void engageAutoBypass() noexcept
    {
        autoBypassed       .store (true, std::memory_order_relaxed);
        cachedLatencySamples.store (0,    std::memory_order_relaxed);
    }

    double preparedSampleRate = 0.0;
    int    preparedBlockSize  = 0;
    // Cached at prepareToPlay so the watchdog multiplies by a constant
    // instead of calling juce::Time::highResolutionTicksToSeconds (which
    // does an internal divide per call). Mirrors AudioEngine's
    // secondsPerTick.
    double secondsPerTick = 0.0;

    // Non-owning pointer to the host's AudioPlayHead; applied to every
    // loaded plugin via setHostPlayHead -> instance->setPlayHead. The
    // engine outlives every PluginSlot so this pointer stays valid for
    // the slot's lifetime.
    juce::AudioPlayHead* hostPlayHead = nullptr;

    // Cached plugin latency. Refreshed on the message thread whenever
    // the instance changes (load / unload / prepareToPlay). The audio
    // thread reads this atom from getLatencySamples() instead of
    // calling juce::AudioPluginInstance::getLatencySamples() directly,
    // which isn't documented as RT-safe.
    std::atomic<int> cachedLatencySamples { 0 };

    // MIDI Learn "last touched" target. Updated by a parameter
    // listener attached on load; cleared on unload. The channel
    // strip's right-click handler reads this so the user can bind
    // "the knob I just moved" without picking from a parameter list.
    std::atomic<int> lastTouchedParamIndex { -1 };
    // Per-parameter listener that just updates lastTouchedParamIndex.
    // Owned by PluginSlot so the lifetime matches the loaded plugin
    // (added on load, removed on unload + destructor). Single instance
    // is attached to every parameter; JUCE's Listener interface is
    // value-only so no per-param storage is needed. Defined inline so
    // unique_ptr's deleter can instantiate against a complete type.
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

    // Watchdog state. Audio-thread-only; no other thread reads these.
    //   • blocksSinceLoad: skips the budget check while a freshly-loaded
    //     plugin warms up (cold caches, lazy-init, oversampler ramps).
    //     Without this, plugins like reverbs and look-ahead limiters trip
    //     the watchdog on block 1 every time and never get processed.
    //   • consecutiveOverruns: a single late block (scheduling jitter,
    //     other-thread preemption) shouldn't kill the plugin. Require N
    //     in a row before bypassing.
    // Reset to 0 by prepareToPlay and by every successful load.
    int blocksSinceLoad     = 0;
    int consecutiveOverruns = 0;

    // Per-block scratch buffer for stereo-fallback plugins. Sized at
    // prepareToPlay so the audio thread doesn't allocate. 2-channel.
    juce::AudioBuffer<float> stereoScratch;

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Out-of-process plugin state. When `remote` is non-null we route
    // every audio + state RPC through it instead of `ownedInstance`.
    // The slot stays in one mode for its lifetime per load: a single
    // load either lands in-process or out-of-process; switching modes
    // requires unload + reload. `currentInstance` stays null in OOP
    // mode (the in-process atomic-park machinery is unused).
    //
    // RT-safety: `remote.get()` is read from the audio thread. We mirror
    // the in-process atomic-pointer pattern with `currentRemote` so the
    // load can swap-publish without locks.
    std::unique_ptr<duskstudio::ipc::RemotePluginConnection> ownedRemote;
    std::atomic<duskstudio::ipc::RemotePluginConnection*>    currentRemote { nullptr };

    // Two-deep keep-alive for deposed IPC connections. Same rationale as
    // `previousInstances` (in-process side): one slot races itself
    // when the user fires two Replace clicks within an audio block;
    // two slots guarantee the SHM mapping + child process stay valid
    // for any pointer the audio thread captured before the
    // currentRemote.store(nullptr).
    std::array<std::unique_ptr<duskstudio::ipc::RemotePluginConnection>, 2> previousRemotes;

    // Cached at load time (the LoadPluginReply tells us all three).
    // Audio thread reads them from the same atomics so no message-thread
    // queries cross the process boundary on the RT path.
    std::atomic<int>  remoteNumIn       { 0 };
    std::atomic<int>  remoteNumOut      { 0 };
    std::atomic<bool> remoteIsInstrument { false };

    // Set when the reaper-poll timer sees the child process has exited.
    // Distinct from autoBypassed (which can also fire on CPU overrun).
    // Read by the UI to drive a "Plugin crashed" cue.
    std::atomic<bool> remoteCrashed { false };

    // Persisted plugin identity. Used by getDescriptionXmlForSave (which
    // can't fillInPluginDescription on a remote instance) and to drive
    // re-prepare-on-block-size-change. Both are message-thread state.
    juce::String savedDescriptionXml;

    // Polls the connected child process every kReaperPeriodMs ms via
    // waitpid(WNOHANG). When the child has exited, sets autoBypassed +
    // remoteCrashed and stops polling (the slot needs an explicit
    // reload before there's anything to watch again). Started on a
    // successful OOP load; stopped on unload, releaseResources, or
    // when the child has been reaped.
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

    // Stashed when restoreFromSavedState fails: the slot is empty but
    // we still know what *should* have loaded. getDescriptionXmlForSave
    // / getStateBase64ForSave return these when the slot is otherwise
    // empty, so a save while offline doesn't wipe the user's data.
    // Cleared on successful load / unload. Message-thread only. Used
    // by the in-process restore path so lives OUTSIDE the OOP gate —
    // DUSKSTUDIO_HAS_OOP_PLUGINS isn't defined on macOS (no *_Mac IPC
    // impls yet) and the offline fallback is cross-platform.
    juce::String offlineDescriptionXml;
    juce::String offlineStateBase64;
};
} // namespace duskstudio
