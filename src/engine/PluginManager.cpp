#include "PluginManager.h"
#include "JuceCompat.h"

#if DUSKSTUDIO_HAS_MULTISAMPLE
  #include "multisample/DuskMultisamplePluginFormat.h"
#endif

#include "ipc/PluginScanProtocol.h"

namespace duskstudio
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS
namespace
{
// A plugin can take a few seconds to instantiate on a cold cache; give a
// generous ceiling and treat anything past it as a hang.
constexpr int kScanTimeoutMs = 30000;

// Routes third-party-binary plugin discovery through the dusk-studio-plugin-host
// child so a plugin that segfaults or hangs in findAllTypesForFile takes down
// only the child, not the app. Installed on knownPluginList for the duration
// of a scan; PluginDirectoryScanner -> KnownPluginList::scanAndAddFile calls
// findPluginTypesFor here for each candidate file.
class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    OutOfProcessPluginScanner (juce::String hostExe, juce::KnownPluginList& list,
                               const std::atomic<bool>* abortFlag)
        : hostExecutable (std::move (hostExe)), knownList (list), abort (abortFlag) {}

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        // Native (in-house) formats are our own code and can't crash the host,
        // so scan them in-process. Only third-party binary formats get sandboxed.
        if (! isSandboxedFormat (format))
        {
            format.findAllTypesForFile (result, fileOrIdentifier);
            return true;
        }

        juce::ChildProcess proc;
        const juce::StringArray args { hostExecutable, "--scan",
                                       format.getName(), fileOrIdentifier };

        if (! proc.start (args, juce::ChildProcess::wantStdOut))
        {
            // Couldn't even spawn the sandbox — fall back to in-process rather
            // than silently dropping a plugin the user has installed.
            format.findAllTypesForFile (result, fileOrIdentifier);
            return true;
        }

        juce::MemoryOutputStream captured;
        char buf[8192];
        // Wrap-safe elapsed check: unsigned subtraction is correct across a
        // single getMillisecondCounter() wrap (every ~49 days of uptime), so
        // compare the interval rather than an absolute deadline.
        const juce::uint32 startMs = juce::Time::getMillisecondCounter();
        bool aborted = false;

        for (;;)
        {
            const int n = proc.readProcessOutput (buf, (int) sizeof buf);
            if (n > 0) { captured.write (buf, (size_t) n); continue; }

            if (! proc.isRunning())
            {
                int extra;
                while ((extra = proc.readProcessOutput (buf, (int) sizeof buf)) > 0)
                    captured.write (buf, (size_t) extra);
                break;
            }

            // Cancel / app-shutdown: kill the child immediately rather than
            // waiting out its timeout. Not a crash, so don't blacklist.
            if (abort != nullptr && abort->load (std::memory_order_relaxed))
            {
                proc.kill();
                aborted = true;
                break;
            }

            if (juce::Time::getMillisecondCounter() - startMs >= (juce::uint32) kScanTimeoutMs)
            {
                proc.kill();
                break;
            }
            juce::Thread::sleep (5);
        }

        if (aborted) return false;

        const juce::String payload = scanproto::extractPayload (captured.toString());

        if (payload.isEmpty())
        {
            // No clean payload => the child crashed or hung. Quarantine the
            // file so the next scan skips it instead of re-crashing.
            knownList.addToBlacklist (fileOrIdentifier);
            return false;
        }

        // A clean payload means the child completed the scan without crashing,
        // so this is a successful scan even when the file legitimately yields
        // zero descriptions. Returning false here would re-probe / quarantine a
        // perfectly-good file. Only the empty-payload (crash/hang) case fails.
        scanproto::parsePayload (payload, result);
        return true;
    }

private:
    static bool isSandboxedFormat (const juce::AudioPluginFormat& f)
    {
        const auto n = f.getName();
        return n == "VST3" || n == "LV2" || n == "AudioUnit" || n == "VST";
    }

    juce::String                 hostExecutable;
    juce::KnownPluginList&        knownList;
    const std::atomic<bool>*      abort;   // polled mid-file; null = never aborts
};
} // namespace
#endif // DUSKSTUDIO_HAS_OOP_PLUGINS

PluginManager::PluginManager()
{
    // Registers the platform-default formats: VST3 + LV2 + AU on Linux/macOS.
    // VST2 is gone from upstream JUCE so don't expect it. Format presence
    // depends on which JUCE modules were compiled in - VST3 is in
    // juce_audio_processors which we already link. The compat shim covers
    // the upstream-vs-wayland-fork API split.
    juce_compat::addDefaultFormats (formatManager);

   #if DUSKSTUDIO_HAS_MULTISAMPLE
    // Native multisample format: makes .sfz / .sf2 files first-class
    // citizens alongside VST3 / LV2 / AU. The format claims those
    // extensions in fileMightContainThisPluginType so PluginSlot's
    // loadFromFile path lands here first for soundfont files.
    formatManager.addFormat (new DuskMultisamplePluginFormat());
   #endif

    loadCache();
}

PluginManager::~PluginManager() = default;

juce::File PluginManager::getCacheFile() const
{
    auto cfgDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Dusk Studio");
    if (! cfgDir.isDirectory() && cfgDir.createDirectory().failed())
        return {};   // fall back to empty File - load/saveCache become no-ops
    return cfgDir.getChildFile ("plugin-cache.xml");
}

juce::File PluginManager::getDeadMansPedalFile() const
{
    const auto cache = getCacheFile();
    if (cache == juce::File()) return {};
    return cache.getSiblingFile ("plugin-scan-deadmanspedal.txt");
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
    return scanInstalledPlugins (nullptr, nullptr);
}

int PluginManager::scanInstalledPlugins (
    std::function<bool (float, const juce::String&)> onProgress,
    const std::atomic<bool>* abort)
{
    const auto deadMansPedalFile = getDeadMansPedalFile();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Sandbox third-party plugin discovery in the child process when the host
    // binary is present. A plugin that crashes its scan kills only the child;
    // we blacklist it and continue. Falls through to in-process scanning if
    // the binary is missing (e.g. a stripped build).
    const juce::File hostExe (getHostExecutablePath());
    const bool sandboxScan = hostExe.existsAsFile();
    if (sandboxScan)
        knownPluginList.setCustomScanner (
            std::make_unique<OutOfProcessPluginScanner> (hostExe.getFullPathName(),
                                                         knownPluginList, abort));
   #else
    juce::ignoreUnused (abort);
   #endif

    // Quarantine anything a previous run was probing when it died: a file left
    // in the dead-man's-pedal means the app itself crashed mid-scan on it.
    if (deadMansPedalFile != juce::File())
        juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (
            knownPluginList, deadMansPedalFile);

    int added = 0;
    // Snapshot the blacklist size so a scan that only quarantines a crashing
    // plugin (added == 0) still persists - otherwise the next launch re-probes
    // and re-crashes on the same file.
    const int blacklistBefore = knownPluginList.getBlacklistedFiles().size();
    const auto& formats = formatManager.getFormats();
    const int   numFormats = formats.size();
    bool aborted = false;

    for (int fi = 0; fi < numFormats && ! aborted; ++fi)
    {
        auto* format = formats[fi];
        if (format == nullptr) continue;

        // Default search paths per format - JUCE pulls these from the OS
        // standard locations (e.g. /usr/lib/vst3, ~/.vst3, /usr/lib/lv2).
        const auto searchPaths = format->getDefaultLocationsToSearch();
        if (searchPaths.getNumPaths() == 0) continue;

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
        {
            if (onProgress != nullptr)
            {
                // Overall fraction: completed formats + this format's own
                // 0..1 progress, divided by the format count.
                const float frac = numFormats > 0
                    ? ((float) fi + scanner.getProgress()) / (float) numFormats
                    : 0.0f;
                if (! onProgress (frac, pluginBeingScanned)) { aborted = true; break; }
            }
            if (abort != nullptr && abort->load (std::memory_order_relaxed))
            {
                aborted = true;
                break;
            }
        }
        added += knownPluginList.getNumTypes() - prevCount;
        if (aborted) break;   // stop advancing to the next format on abort
    }

    // Don't report 100%/complete if the user aborted - onProgress already
    // returned false to request the stop, and the caller treats this final
    // call as "scan finished".
    if (! aborted && onProgress != nullptr)
        onProgress (1.0f, {});

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Release the child-launching scanner — load-time instantiation must stay
    // in-process and doesn't go through the custom scanner anyway.
    if (sandboxScan)
        knownPluginList.setCustomScanner (nullptr);
   #endif

    const bool blacklistGrew = knownPluginList.getBlacklistedFiles().size() != blacklistBefore;
    if (added > 0 || blacklistGrew) saveCache();
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
