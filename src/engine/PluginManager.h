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
    // Rescan CLAP search paths (slow — loads each bundle). Folded into the Scan button
    // via scanInstalledPlugins; result cached in memory for the session.
    void scanClapPlugins();

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
    juce::Array<juce::PluginDescription> clapDescriptions;   // native CLAP (scanned separately)
    bool                           oopEnabled { false };

    void loadCache();
    void saveCache() const;

    // Native CLAP descriptions persist in their own sidecar cache (knownPluginList is
    // JUCE-formats only). Loaded at construction, rewritten after each scanClapPlugins.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    juce::File getClapCacheFile() const;
    void loadClapCache();
    void saveClapCache() const;
#endif
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
