#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginDescriptor.h"

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

    // Plugins quarantined by an earlier scan (a crash, a hang, or a payload the
    // child couldn't produce). Every later scan skips them outright, so without
    // this count a machine that quarantined its library just reports "0 added".
    int getQuarantinedCount() const
    { return knownPluginList.getBlacklistedFiles().size(); }

    juce::String getHostExecutablePath() const;

    // The mode the sandbox child is launched in. Only the tests ever move it,
    // and only so they can drive the real child in one of its stub modes.
    std::string getHostModeArg() const
    {
       #if defined(DUSKSTUDIO_TESTS)
        if (! hostModeArgOverride.empty()) return hostModeArgOverride;
       #endif
        return "--ipc-host";
    }

   #if defined(DUSKSTUDIO_TESTS)
    // The child is resolved beside the running executable, which is the app in
    // production and the Catch2 binary under ctest - where no child sits, so the
    // sandboxed load path is unreachable without this.
    void setHostExecutableForTest (std::string path, std::string modeArg)
    {
        hostExecutableOverride = std::move (path);
        hostModeArgOverride    = std::move (modeArg);
    }
   #endif

    std::vector<PluginDescriptor> getInstrumentDescriptions() const;
    std::vector<PluginDescriptor> getEffectDescriptions() const;

    std::vector<PluginDescriptor> getClapEffectDescriptions() const;
    std::vector<PluginDescriptor> getClapInstrumentDescriptions() const;
    void scanClapPlugins (const std::atomic<bool>* abort = nullptr);

    std::vector<PluginDescriptor> getLv2EffectDescriptions() const;
    std::vector<PluginDescriptor> getLv2InstrumentDescriptions() const;
    void scanLv2Plugins (const std::atomic<bool>* abort = nullptr);

    std::vector<PluginDescriptor> getVst3NativeEffectDescriptions() const;
    std::vector<PluginDescriptor> getVst3NativeInstrumentDescriptions() const;
    void scanVst3NativePlugins (const std::atomic<bool>* abort = nullptr);

    std::vector<PluginDescriptor> getAuEffectDescriptions() const;
    std::vector<PluginDescriptor> getAuInstrumentDescriptions() const;
    void scanAuPlugins (const std::atomic<bool>* abort = nullptr);

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
    std::vector<PluginDescriptor> auDescriptions;
    bool oopEnabled { false };
    std::atomic<int> lastScanSandboxSkips { 0 };

   #if defined(DUSKSTUDIO_TESTS)
    std::string hostExecutableOverride;
    std::string hostModeArgOverride;
   #endif

    std::vector<PluginDescriptor> filterByInstrumentFlag (
        const std::vector<PluginDescriptor>& source, bool wantInstrument) const;

    void loadCache();
    void saveCache() const;

    juce::File nativeCacheFile (const char* fileName) const;
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
    // What one sandboxed native-bundle scan settled.
    enum class NativeScanOutcome
    {
        Handled,     // the child settled it; do NOT re-run the bundle in-process
        NoSandbox,   // no usable child was started; the caller falls back in-process
        Cancelled    // user stopped the scan; abandon the phase without caching it
    };

    NativeScanOutcome scanNativeBundleSandboxed (const char* format, const juce::File& bundle,
                                                 std::vector<PluginDescriptor>& into,
                                                 const std::atomic<bool>* abort) const;
#endif
    void loadNativeCache (std::vector<PluginDescriptor>& into,
                          const char* jsonFileName, const char* legacyXmlFileName,
                          bool bundleIsDirectory);
    void loadAuCache();
    void saveNativeCache (const std::vector<PluginDescriptor>& from,
                          const char* jsonFileName) const;
};

inline juce::String PluginManager::getHostExecutablePath() const
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
   #if defined(DUSKSTUDIO_TESTS)
    if (! hostExecutableOverride.empty()) return hostExecutableOverride;
   #endif
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
