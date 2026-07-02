#include "PluginManager.h"
#include "JuceCompat.h"

#if DUSKSTUDIO_HAS_MULTISAMPLE
  #include "multisample/DuskMultisamplePluginFormat.h"
#endif

#include "ipc/PluginScanProtocol.h"
#include "PluginBackingCheck.h"
#if DUSKSTUDIO_HAS_NATIVE_CLAP
  #include "clap/ClapScanner.h"   // Linux-only native CLAP discovery
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
  #include "lv2/Lv2Scanner.h"     // Linux-only native LV2 discovery
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
  #include "vst3/Vst3Scanner.h"   // Linux-only native VST3 discovery
#endif

#include <map>

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
                proc.waitForProcessToFinish (200);  // reap: kill() SIGKILLs but never waitpid()s
                aborted = true;
                break;
            }

            if (juce::Time::getMillisecondCounter() - startMs >= (juce::uint32) kScanTimeoutMs)
            {
                proc.kill();
                proc.waitForProcessToFinish (200);  // reap the SIGKILLed child, no zombie
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
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    loadClapCache();   // restore native CLAP descriptions so the picker has them at launch
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    loadLv2Cache();
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    loadVst3NativeCache();
#endif
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

    // Snapshot the blacklist size BEFORE applying the dead-man's-pedal so a
    // recovery that only re-quarantines a prior crash (added == 0, no new
    // scanner blacklisting) still counts as a change and persists - otherwise
    // the next launch re-probes and re-crashes on the same file.
    const int blacklistBefore = knownPluginList.getBlacklistedFiles().size();

    // Quarantine anything a previous run was probing when it died: a file left
    // in the dead-man's-pedal means the app itself crashed mid-scan on it.
    if (deadMansPedalFile != juce::File())
        juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (
            knownPluginList, deadMansPedalFile);

    int added = 0;
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
    // call as "scan finished". Also honour a scanner-side abort (the abort
    // atomic): if it flipped on the last file, scanNextFile can fall out of
    // the loop before the in-loop abort check runs, so re-test it here.
    const bool abortFlagSet = (abort != nullptr && abort->load (std::memory_order_relaxed));
    const bool aborting     = aborted || abortFlagSet;
    if (! aborting && onProgress != nullptr)
        onProgress (1.0f, {});

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Release the child-launching scanner — load-time instantiation must stay
    // in-process and doesn't go through the custom scanner anyway.
    if (sandboxScan)
        knownPluginList.setCustomScanner (nullptr);
   #endif

    // Prune dead entries so the picker never offers a plugin that can't load.
    // Two instantiation-free checks cover every format:
    //   1. Path-backed formats (VST3 / AU bundles / CLAP / soundfonts): the
    //      bundle or file is gone or hollowed out — see pluginBackingLooksDead.
    //   2. URI-backed formats (LV2, AU component IDs): the identifier is no
    //      longer discoverable in the format's live search paths because the
    //      bundle was uninstalled. searchPathsForPlugins re-reads the LV2 world /
    //      AU registry but does NOT instantiate, so a crashy plugin can't take
    //      down the scan, and one call yields the whole live set per format.
    // Skipped on abort — the list is mid-scan and incomplete.
    int pruned = 0;
    if (! aborting)
    {
        // Live identifier set per format, gathered once and only for formats
        // that actually have URI-style (non-path) entries needing validation.
        std::map<juce::String, juce::StringArray> liveIdsByFormat;
        for (const auto& desc : knownPluginList.getTypes())
            if (! juce::File::isAbsolutePath (desc.fileOrIdentifier))
                liveIdsByFormat.emplace (desc.pluginFormatName, juce::StringArray{});

        for (auto& entry : liveIdsByFormat)
            for (auto* format : formatManager.getFormats())
            {
                if (format == nullptr) continue;
                if (format->getName() == entry.first)
                {
                    entry.second = format->searchPathsForPlugins (
                        format->getDefaultLocationsToSearch(), /*recursive*/ true,
                        /*allowAsync*/ false);
                    break;
                }
            }

        // getTypes() returns a COPY of the internal array (JUCE), so removeType()
        // mutating the live list inside this loop can't invalidate the iteration.
        for (const auto& desc : knownPluginList.getTypes())
        {
            bool dead = pluginBackingLooksDead (desc.fileOrIdentifier);

            if (! dead && ! juce::File::isAbsolutePath (desc.fileOrIdentifier))
            {
                const auto it = liveIdsByFormat.find (desc.pluginFormatName);
                // Only prune when we have a NON-EMPTY live set for the format:
                // an empty set means discovery failed or the format has no search
                // locations, and nuking every entry on a transient miss would be
                // far worse than leaving a stale one.
                if (it != liveIdsByFormat.end() && ! it->second.isEmpty()
                     && ! it->second.contains (desc.fileOrIdentifier))
                    dead = true;
            }

            if (dead)
            {
                knownPluginList.removeType (desc);
                ++pruned;
            }
        }
    }

    const bool blacklistGrew = knownPluginList.getBlacklistedFiles().size() != blacklistBefore;
    if (added > 0 || pruned > 0 || blacklistGrew) saveCache();

    if (! aborting)
    {
        scanClapPlugins();        // CLAP isn't a juce format — scan it alongside the JUCE pass
        scanLv2Plugins();         // native-LV2 rows are separate from JUCE's LV2 format
        scanVst3NativePlugins();  // native-VST3 rows are separate from JUCE's VST3 format
    }
    return added;
}

void PluginManager::scanClapPlugins()
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    // Discover OUTSIDE the lock (dlopens every bundle — slow), swap in under it.
    // The cache write also stays outside so a picker open on the message thread
    // can't stall behind this thread's file I/O.
    const auto scanned = clap::ClapScanner::scan();
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        clapDescriptions.clearQuick();
        for (const auto& s : scanned)
        {
            juce::PluginDescription d;
            d.name             = juce::String (juce::CharPointer_UTF8 (s.desc.name.c_str()));
            d.manufacturerName = juce::String (juce::CharPointer_UTF8 (s.desc.vendor.c_str()));
            d.version          = juce::String (juce::CharPointer_UTF8 (s.desc.version.c_str()));
            d.pluginFormatName = "CLAP";
            d.fileOrIdentifier = s.bundlePath;
            d.isInstrument     = s.desc.isInstrument();
            clapDescriptions.add (d);
        }
    }
    saveClapCache();   // persist so the picker has CLAP at next launch without a re-scan
#endif
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP
juce::File PluginManager::getClapCacheFile() const
{
    const auto base = getCacheFile();
    return base == juce::File() ? juce::File() : base.getSiblingFile ("clap-cache.xml");
}

void PluginManager::loadClapCache()
{
    const auto file = getClapCacheFile();
    if (file == juce::File() || ! file.existsAsFile())
        return;

    if (auto xml = juce::parseXML (file))
    {
        clapDescriptions.clearQuick();
        for (auto* child : xml->getChildIterator())
        {
            juce::PluginDescription d;
            // Drop entries whose .clap bundle is gone since the cache was written, so
            // the picker never offers a removed plugin until the next rescan rebuilds it.
            if (d.loadFromXml (*child) && juce::File (d.fileOrIdentifier).exists())
                clapDescriptions.add (d);
        }
    }
}

void PluginManager::saveClapCache() const
{
    const auto file = getClapCacheFile();
    if (file == juce::File())
        return;

    // Snapshot under the lock; serialize + write with it released so the picker
    // can't stall behind the file I/O.
    juce::Array<juce::PluginDescription> snapshot;
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        snapshot = clapDescriptions;
    }
    juce::XmlElement root ("CLAP_PLUGINS");
    for (const auto& d : snapshot)
        root.addChildElement (d.createXml().release());
    root.writeTo (file);
}
#endif

juce::Array<juce::PluginDescription> PluginManager::getClapEffectDescriptions() const
{
    const juce::ScopedLock sl (nativeDescriptionsLock);
    juce::Array<juce::PluginDescription> effects;
    for (const auto& d : clapDescriptions)
        if (! d.isInstrument)
            effects.add (d);
    return effects;
}

void PluginManager::scanLv2Plugins()
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
    const auto scanned = lv2::Lv2Scanner::scan();   // manifest parse outside the lock
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        lv2Descriptions.clearQuick();
        for (const auto& s : scanned)
        {
            // Only audio effects for now — the native LV2 host is an insert host;
            // instruments and MIDI utilities stay with the JUCE LV2 format.
            if (s.desc.audioInputs <= 0 || s.desc.audioOutputs <= 0)
                continue;
            juce::PluginDescription d;
            d.name             = juce::String (juce::CharPointer_UTF8 (s.desc.name.c_str()));
            d.pluginFormatName = "LV2-Native";
            d.fileOrIdentifier = s.bundlePath;
            d.isInstrument     = false;
            lv2Descriptions.add (d);
        }
    }
    saveLv2Cache();   // persist so the picker has LV2 at next launch without a re-scan
#endif
}

#if DUSKSTUDIO_HAS_NATIVE_LV2
juce::File PluginManager::getLv2CacheFile() const
{
    const auto base = getCacheFile();
    return base == juce::File() ? juce::File() : base.getSiblingFile ("lv2-native-cache.xml");
}

void PluginManager::loadLv2Cache()
{
    const auto file = getLv2CacheFile();
    if (file == juce::File() || ! file.existsAsFile())
        return;

    if (auto xml = juce::parseXML (file))
    {
        lv2Descriptions.clearQuick();
        for (auto* child : xml->getChildIterator())
        {
            juce::PluginDescription d;
            // Drop entries whose bundle directory is gone since the cache was written.
            if (d.loadFromXml (*child) && juce::File (d.fileOrIdentifier).isDirectory())
                lv2Descriptions.add (d);
        }
    }
}

void PluginManager::saveLv2Cache() const
{
    const auto file = getLv2CacheFile();
    if (file == juce::File())
        return;

    juce::Array<juce::PluginDescription> snapshot;
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        snapshot = lv2Descriptions;
    }
    juce::XmlElement root ("LV2_NATIVE_PLUGINS");
    for (const auto& d : snapshot)
        root.addChildElement (d.createXml().release());
    root.writeTo (file);
}
#endif

juce::Array<juce::PluginDescription> PluginManager::getLv2EffectDescriptions() const
{
    const juce::ScopedLock sl (nativeDescriptionsLock);
    return lv2Descriptions;   // scan already filtered to audio effects
}

void PluginManager::scanVst3NativePlugins()
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
    const auto scanned = vst3::Vst3Scanner::scan();   // dlopens every module — outside the lock
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        vst3NativeDescriptions.clearQuick();
        for (const auto& s : scanned)
        {
            // Only audio effects for now — the native VST3 host is an insert host;
            // instruments stay with the JUCE VST3 format.
            if (s.desc.isInstrument)
                continue;
            juce::PluginDescription d;
            d.name             = juce::String (juce::CharPointer_UTF8 (s.desc.name.c_str()));
            d.manufacturerName = juce::String (juce::CharPointer_UTF8 (s.desc.vendor.c_str()));
            d.version          = juce::String (juce::CharPointer_UTF8 (s.desc.version.c_str()));
            d.pluginFormatName = "VST3-Native";
            d.fileOrIdentifier = s.bundlePath;
            d.isInstrument     = false;
            vst3NativeDescriptions.add (d);
        }
    }
    saveVst3NativeCache();   // persist so the picker has VST3 at next launch without a re-scan
#endif
}

#if DUSKSTUDIO_HAS_NATIVE_VST3
juce::File PluginManager::getVst3NativeCacheFile() const
{
    const auto base = getCacheFile();
    return base == juce::File() ? juce::File() : base.getSiblingFile ("vst3-native-cache.xml");
}

void PluginManager::loadVst3NativeCache()
{
    const auto file = getVst3NativeCacheFile();
    if (file == juce::File() || ! file.existsAsFile())
        return;

    if (auto xml = juce::parseXML (file))
    {
        vst3NativeDescriptions.clearQuick();
        for (auto* child : xml->getChildIterator())
        {
            juce::PluginDescription d;
            // Drop entries whose bundle is gone since the cache was written.
            if (d.loadFromXml (*child) && juce::File (d.fileOrIdentifier).exists())
                vst3NativeDescriptions.add (d);
        }
    }
}

void PluginManager::saveVst3NativeCache() const
{
    const auto file = getVst3NativeCacheFile();
    if (file == juce::File())
        return;

    juce::Array<juce::PluginDescription> snapshot;
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        snapshot = vst3NativeDescriptions;
    }
    juce::XmlElement root ("VST3_NATIVE_PLUGINS");
    for (const auto& d : snapshot)
        root.addChildElement (d.createXml().release());
    root.writeTo (file);
}
#endif

juce::Array<juce::PluginDescription> PluginManager::getVst3NativeEffectDescriptions() const
{
    const juce::ScopedLock sl (nativeDescriptionsLock);
    return vst3NativeDescriptions;   // scan already filtered to audio effects
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

namespace
{
// The caller will call prepareToPlay before processing - but we set the bus
// layout here so the caller knows what they got. Default mono in / mono out for
// channel-strip use; stereo fallback for plugins that don't support mono
// (channel strips mix L+R from a stereo plugin's output).
void applyDefaultBusLayout (juce::AudioPluginInstance& instance)
{
    if (! instance.setBusesLayout ({ { juce::AudioChannelSet::mono() },
                                      { juce::AudioChannelSet::mono() } }))
        instance.setBusesLayout ({ { juce::AudioChannelSet::stereo() },
                                    { juce::AudioChannelSet::stereo() } });
}
} // namespace

std::unique_ptr<juce::AudioPluginInstance>
PluginManager::createPluginInstance (const juce::PluginDescription& desc,
                                      double sampleRate, int blockSize,
                                      juce::String& errorMessage)
{
    auto instance = formatManager.createPluginInstance (desc, sampleRate, blockSize, errorMessage);
    if (instance == nullptr)
        return nullptr;

    applyDefaultBusLayout (*instance);
    return instance;
}

void PluginManager::createPluginInstanceAsync (
    const juce::PluginDescription& desc, double sampleRate, int blockSize,
    std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> callback)
{
    // Off-thread creation for slow-to-instantiate formats (notably the native
    // multisample / soundfont player, which decodes samples). JUCE runs the
    // format's createInstance on a background thread when the format reports
    // requiresUnblockedMessageThreadDuringCreation() == false, then fires this
    // callback ON THE MESSAGE THREAD with the fully-built instance — so the
    // caller's swap-in logic stays single-threaded.
    formatManager.createPluginInstanceAsync (desc, sampleRate, blockSize,
        [cb = std::move (callback)]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err)
    {
        if (instance != nullptr)
            applyDefaultBusLayout (*instance);
        cb (std::move (instance), err);
    });
}
} // namespace duskstudio
