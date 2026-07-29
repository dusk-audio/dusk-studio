#pragma once

#include "PluginDescriptor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace duskstudio
{
// Owns the private JUCE format manager and legacy plugin list. Public metadata
// views use PluginDescriptor; JUCE conversion is confined to this host boundary.
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    int getPluginCount() const noexcept { return knownPluginList.getNumTypes(); }

    void setOopEnabled (bool enable) noexcept { oopEnabled = enable; }
    bool isOopEnabled() const noexcept       { return oopEnabled; }

    int getLastScanSandboxSkips() const noexcept
    { return lastScanSandboxSkips.load (std::memory_order_relaxed); }

    juce::String getHostExecutablePath() const;

    std::vector<PluginDescriptor> getInstrumentDescriptions() const;
    std::vector<PluginDescriptor> getEffectDescriptions() const;

    std::vector<PluginDescriptor> getClapEffectDescriptions() const;
    std::vector<PluginDescriptor> getClapInstrumentDescriptions() const;
    void scanClapPlugins();

    std::vector<PluginDescriptor> getLv2EffectDescriptions() const;
    std::vector<PluginDescriptor> getLv2InstrumentDescriptions() const;
    void scanLv2Plugins();

    std::vector<PluginDescriptor> getVst3NativeEffectDescriptions() const;
    std::vector<PluginDescriptor> getVst3NativeInstrumentDescriptions() const;
    void scanVst3NativePlugins();

    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::File& pluginFile,
                          double sampleRate, int blockSize,
                          juce::String& errorMessage);

    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const PluginDescriptor& descriptor,
                          double sampleRate, int blockSize,
                          juce::String& errorMessage);

    void createPluginInstanceAsync (
        const PluginDescriptor& descriptor, double sampleRate, int blockSize,
        std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> callback);

    // Conversion helpers used by PluginSlot at the private legacy-host edge.
    PluginDescriptor descriptorForInstance (juce::AudioPluginInstance&) const;
    bool descriptorFromLegacyXml (const juce::String&, PluginDescriptor&) const;
    juce::String descriptorToLegacyXml (const PluginDescriptor&) const;

   #if defined(DUSKSTUDIO_TESTS)
    static PluginDescriptor descriptorFromJuceForTest (
        const juce::PluginDescription&);
    static juce::PluginDescription descriptorToJuceForTest (
        const PluginDescriptor&);
    static std::vector<PluginDescriptor> importLegacyNativeCacheForTest (
        const juce::String& xml,
        const std::function<bool (const juce::File&)>& locationExists);
    static bool loadNativeCacheSourcesForTest (
        const std::optional<std::string>& jsonSource,
        const std::optional<juce::String>& legacyXmlSource,
        const std::function<bool (const juce::File&)>& locationExists,
        std::vector<PluginDescriptor>& into);
   #endif

    juce::File getCacheFile() const;
    juce::File getDeadMansPedalFile() const;

    int scanInstalledPlugins();
    int scanInstalledPlugins (std::function<bool (float, const juce::String&)> onProgress,
                              const std::atomic<bool>* abort = nullptr);

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;

    mutable juce::CriticalSection nativeDescriptionsLock;
    std::vector<PluginDescriptor> clapDescriptions;
    std::vector<PluginDescriptor> lv2Descriptions;
    std::vector<PluginDescriptor> vst3NativeDescriptions;
    bool oopEnabled { false };
    std::atomic<int> lastScanSandboxSkips { 0 };

    std::vector<PluginDescriptor> filterByInstrumentFlag (
        const std::vector<PluginDescriptor>& source, bool wantInstrument) const;

    void loadCache();
    void saveCache() const;

    juce::File nativeCacheFile (const char* fileName) const;
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
    bool scanNativeBundleSandboxed (const char* format, const juce::File& bundle,
                                    std::vector<PluginDescriptor>& into) const;
#endif
    void loadNativeCache (std::vector<PluginDescriptor>& into,
                          const char* jsonFileName, const char* legacyXmlFileName,
                          bool bundleIsDirectory);
    void saveNativeCache (const std::vector<PluginDescriptor>& from,
                          const char* jsonFileName) const;
};

inline juce::String PluginManager::getHostExecutablePath() const
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
   #if JUCE_WINDOWS
    const char* const childName = "dusk-studio-plugin-host.exe";
   #else
    const char* const childName = "dusk-studio-plugin-host";
   #endif
    return exe.getParentDirectory().getChildFile (childName).getFullPathName();
   #else
    return {};
   #endif
}
} // namespace duskstudio
