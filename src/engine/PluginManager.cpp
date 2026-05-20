#include "PluginManager.h"
#include "JuceCompat.h"

namespace duskstudio
{
PluginManager::PluginManager()
{
    // Registers the platform-default formats: VST3 + LV2 + AU on Linux/macOS.
    // VST2 is gone from upstream JUCE so don't expect it. Format presence
    // depends on which JUCE modules were compiled in - VST3 is in
    // juce_audio_processors which we already link. The compat shim covers
    // the upstream-vs-wayland-fork API split.
    juce_compat::addDefaultFormats (formatManager);

    loadCache();
}

PluginManager::~PluginManager() = default;

juce::File PluginManager::getCacheFile() const
{
    auto cfgDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Focal");
    if (! cfgDir.isDirectory() && cfgDir.createDirectory().failed())
        return {};   // fall back to empty File - load/saveCache become no-ops
    return cfgDir.getChildFile ("plugin-cache.xml");
}

void PluginManager::loadCache()
{
    const auto cache = getCacheFile();
    if (! cache.existsAsFile()) return;

    if (auto xml = juce::XmlDocument::parse (cache))
        knownPluginList.recreateFromXml (*xml);
}

void PluginManager::saveCache() const
{
    if (auto xml = knownPluginList.createXml())
        xml->writeTo (getCacheFile());
}

int PluginManager::scanInstalledPlugins()
{
    int added = 0;
    juce::PropertiesFile::Options unusedDeadEntries;
    juce::ignoreUnused (unusedDeadEntries);

    for (auto* format : formatManager.getFormats())
    {
        if (format == nullptr) continue;

        // Default search paths per format - JUCE pulls these from the OS
        // standard locations (e.g. /usr/lib/vst3, ~/.vst3, /usr/lib/lv2).
        const auto searchPaths = format->getDefaultLocationsToSearch();
        if (searchPaths.getNumPaths() == 0) continue;

        juce::File deadMansPedalFile;  // empty = no crash-recovery shielding
        juce::PluginDirectoryScanner scanner (knownPluginList, *format,
                                                searchPaths, /*recursive*/ true,
                                                deadMansPedalFile,
                                                /*allowAsync*/ false);

        juce::String pluginBeingScanned;
        // Loop scanNextFile until it returns false. JUCE adds discovered
        // descriptions to knownPluginList directly; we just count.
        const int prevCount = knownPluginList.getNumTypes();
        while (scanner.scanNextFile (/*dontRescanIfAlreadyInList*/ true,
                                       pluginBeingScanned))
            ;
        added += knownPluginList.getNumTypes() - prevCount;
    }

    if (added > 0) saveCache();
    return added;
}

juce::Array<juce::PluginDescription> PluginManager::getInstrumentDescriptions() const
{
    juce::Array<juce::PluginDescription> instruments;
    for (const auto& desc : knownPluginList.getTypes())
        if (desc.isInstrument)
            instruments.add (desc);
    return instruments;
}

juce::Array<juce::PluginDescription> PluginManager::getEffectDescriptions() const
{
    juce::Array<juce::PluginDescription> effects;
    for (const auto& desc : knownPluginList.getTypes())
        if (! desc.isInstrument)
            effects.add (desc);
    return effects;
}

std::unique_ptr<juce::AudioPluginInstance>
PluginManager::createPluginInstance (const juce::File& pluginFile,
                                      double sampleRate, int blockSize,
                                      juce::String& errorMessage)
{
    // Iterate available formats and ask each to scan the file for plugin
    // descriptions. The first format that recognises the file wins. For a
    // VST3 bundle on Linux, this is the VST3 format. Multiple descriptions
    // can come from a single bundle (a "shell" plugin) - for MVP we just
    // take the first one.
    juce::OwnedArray<juce::PluginDescription> typesFound;
    for (auto* format : formatManager.getFormats())
    {
        if (format == nullptr) continue;
        if (! format->fileMightContainThisPluginType (pluginFile.getFullPathName()))
            continue;

        format->findAllTypesForFile (typesFound, pluginFile.getFullPathName());
        if (typesFound.size() > 0)
            break;
    }

    if (typesFound.isEmpty())
    {
        errorMessage = "No plugin descriptions found in " + pluginFile.getFullPathName();
        return nullptr;
    }

    // Cache discovered descriptions - even ones we won't instantiate now
    // (multi-shell plugins) will be useful at session restore time.
    for (auto* desc : typesFound)
        if (desc != nullptr)
            knownPluginList.addType (*desc);
    saveCache();

    return createPluginInstance (*typesFound.getFirst(), sampleRate, blockSize, errorMessage);
}

std::unique_ptr<juce::AudioPluginInstance>
PluginManager::createPluginInstance (const juce::PluginDescription& desc,
                                      double sampleRate, int blockSize,
                                      juce::String& errorMessage)
{
    auto instance = formatManager.createPluginInstance (desc, sampleRate, blockSize, errorMessage);
    if (instance == nullptr)
        return nullptr;

    // The caller will call prepareToPlay before processing - but we set the
    // bus layout here so the caller knows what they got. Default mono in /
    // mono out for channel-strip use; stereo can be re-set by callers that
    // want it.
    if (! instance->setBusesLayout ({ { juce::AudioChannelSet::mono() },
                                       { juce::AudioChannelSet::mono() } }))
    {
        // Plugin doesn't support mono - fall back to stereo. Callers that
        // need mono will have to deal (most channel strips will mix
        // L+R from a stereo plugin's output).
        instance->setBusesLayout ({ { juce::AudioChannelSet::stereo() },
                                     { juce::AudioChannelSet::stereo() } });
    }

    return instance;
}
} // namespace duskstudio
