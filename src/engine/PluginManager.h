#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>

namespace duskstudio
{
// Owns AudioPluginFormatManager + shared KnownPluginList. Channel strips
// hold a PluginSlot that calls back here to instantiate plugins.
// Message-thread only — audio thread never touches this class; it reads
// the slot's atomic instance pointer instead.
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }
    juce::KnownPluginList&          getKnownPluginList() noexcept { return knownPluginList; }

    // OOP toggle: when on, PluginSlot routes new loads through the
    // dusk-studio-plugin-host child via RemotePluginConnection.
    // Read at load time only; affects next load, not current ones.
    void setOopEnabled (bool enable) noexcept { oopEnabled = enable; }
    bool isOopEnabled() const noexcept       { return oopEnabled; }

    // Path to the OOP child binary, alongside the running app. Empty when
    // OOP isn't compiled in for this platform.
    juce::String getHostExecutablePath() const;

    // Filtered views over the known list, used by the per-channel picker
    // so MIDI tracks see instruments only and audio tracks see effects only.
    juce::Array<juce::PluginDescription> getInstrumentDescriptions() const;
    juce::Array<juce::PluginDescription> getEffectDescriptions() const;

    // Native-CLAP plugins — scanned separately (CLAP is NOT a juce::AudioPluginFormat).
    // Synthesised PluginDescriptions: pluginFormatName "CLAP", fileOrIdentifier = the
    // .clap bundle path. The unified picker merges these in for surfaces that have a
    // native CLAP host (aux lanes); routing keys on pluginFormatName == "CLAP".
    juce::Array<juce::PluginDescription> getClapEffectDescriptions() const;
    juce::Array<juce::PluginDescription> getClapInstrumentDescriptions() const;
    // Rescan CLAP search paths (slow — loads each bundle). Folded into the Scan button
    // via scanInstalledPlugins; result cached in memory for the session.
    void scanClapPlugins();

    // Native-LV2 plugins, scanned separately from JUCE's LV2 format (which stays as
    // the fallback host). pluginFormatName "LV2-Native", fileOrIdentifier = the .lv2
    // bundle directory. Effects and instruments; discovery is manifest-only via lilv.
    juce::Array<juce::PluginDescription> getLv2EffectDescriptions() const;
    juce::Array<juce::PluginDescription> getLv2InstrumentDescriptions() const;
    void scanLv2Plugins();

    // Native-VST3 plugins, scanned separately from JUCE's VST3 format (which stays
    // as the fallback host). pluginFormatName "VST3-Native", fileOrIdentifier = the
    // .vst3 bundle path. Effects and instruments; each module is dlopen'd (like CLAP).
    juce::Array<juce::PluginDescription> getVst3NativeEffectDescriptions() const;
    juce::Array<juce::PluginDescription> getVst3NativeInstrumentDescriptions() const;
    void scanVst3NativePlugins();

    // Synchronous instantiation. May take 100s of ms. Returns nullptr on
    // failure and sets errorMessage. Caller runs prepareToPlay before
    // processing audio. Success adds the description to knownPluginList.
    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::File& pluginFile,
                           double sampleRate, int blockSize,
                           juce::String& errorMessage);

    // Resolve by description (used at session restore when the path may
    // have moved but the uid + format still match).
    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::PluginDescription& desc,
                           double sampleRate, int blockSize,
                           juce::String& errorMessage);

    // Off-thread variant: creates the instance on a background thread (for
    // formats that allow it — e.g. the multisample player's slow sample
    // decode) and invokes `callback` ON THE MESSAGE THREAD with the finished
    // instance (or nullptr + error). Keeps the UI responsive during load.
    void createPluginInstanceAsync (
        const juce::PluginDescription& desc, double sampleRate, int blockSize,
        std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> callback);

    // Cache file under userApplicationDataDirectory. Auto-saved on every
    // successful add; loaded best-effort at construction.
    juce::File getCacheFile() const;

    // Dead-man's-pedal alongside the cache. PluginDirectoryScanner records
    // the file it is about to probe here and clears it on success, so a scan
    // that crashes the whole app quarantines the culprit on the next run.
    juce::File getDeadMansPedalFile() const;

    // Scans default install locations across every supported format.
    // Synchronous, 10-30 s first run — run it on a background thread (see
    // PluginScanModal) and surface progress, otherwise the app looks frozen.
    int scanInstalledPlugins();

    // Progress-reporting variant. onProgress(fraction 0..1, currentPluginName)
    // is invoked on the CALLING thread once per scanned file; return false from
    // it to stop early. `abort`, if non-null, is polled mid-file (between the
    // child-process reads) so a cancel / app-shutdown doesn't wait out a slow
    // plugin's full 30 s timeout. Safe to call from a background thread:
    // KnownPluginList serialises its own mutations and nothing else touches it
    // during a scan.
    int scanInstalledPlugins (std::function<bool (float, const juce::String&)> onProgress,
                              const std::atomic<bool>* abort = nullptr);

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList          knownPluginList;
    // Native-format descriptions. scanInstalledPlugins runs on a background
    // thread (PluginScanModal) and repopulates these while the message thread
    // may be reading them for the picker — every access goes through this lock.
    mutable juce::CriticalSection nativeDescriptionsLock;
    juce::Array<juce::PluginDescription> clapDescriptions;       // native CLAP (scanned separately)
    juce::Array<juce::PluginDescription> lv2Descriptions;        // native LV2 (scanned separately)
    juce::Array<juce::PluginDescription> vst3NativeDescriptions; // native VST3 (scanned separately)
    bool                           oopEnabled { false };

    juce::Array<juce::PluginDescription> filterByInstrumentFlag (
        const juce::Array<juce::PluginDescription>& source, bool wantInstrument) const;

    void loadCache();
    void saveCache() const;

    // Native-format descriptions persist in their own sidecar caches next to the
    // JUCE cache (knownPluginList is JUCE-formats only). Loaded at construction,
    // rewritten after each scan. `bundleIsDirectory` selects the staleness check
    // (LV2 bundles are directories; CLAP/VST3 accept files or bundle dirs).
    juce::File nativeCacheFile (const char* fileName) const;
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
    // Enumerate one native bundle via the sandbox child (--scan-native). False =
    // child couldn't spawn (caller falls back in-process); crash/timeout inside
    // the child skips the bundle and still returns true.
    bool scanNativeBundleSandboxed (const char* format, const juce::File& bundle,
                                    juce::Array<juce::PluginDescription>& into) const;
#endif
    void loadNativeCache (juce::Array<juce::PluginDescription>& into,
                          const char* fileName, bool bundleIsDirectory);
    void saveNativeCache (const juce::Array<juce::PluginDescription>& from,
                          const char* fileName, const char* rootTag) const;
};

inline juce::String PluginManager::getHostExecutablePath() const
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    return exe.getParentDirectory().getChildFile ("dusk-studio-plugin-host").getFullPathName();
   #else
    return {};
   #endif
}
} // namespace duskstudio
