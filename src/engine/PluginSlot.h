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

// One plugin instance per slot, audio-thread-safe processing.
// load/unload/setEnabled: message thread.
// process*: audio thread.
// Instance pointer is atomic; old instances are released on the message
// thread after swap because the destructor isn't RT-safe. Audio thread
// sees either nullptr (bypass) or a fully-prepared instance.
class PluginSlot
{
public:
    PluginSlot() = default;
    ~PluginSlot();

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
};
} // namespace duskstudio
