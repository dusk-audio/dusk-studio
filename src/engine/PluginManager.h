#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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

    // Cache file under userApplicationDataDirectory. Auto-saved on every
    // successful add; loaded best-effort at construction.
    juce::File getCacheFile() const;

    // Scans default install locations across every supported format.
    // Synchronous, 10-30 s first run. UI should show a modal.
    int scanInstalledPlugins();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList          knownPluginList;
    bool                           oopEnabled { false };

    void loadCache();
    void saveCache() const;
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
