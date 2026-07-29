#include "PluginManager.h"
#include "JuceCompat.h"

#include "ipc/PluginScanProtocol.h"
#include "NativePluginCache.h"
#include "PluginBackingCheck.h"
#include "hosting/NativePluginId.h"
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
 #include "NativeScanRows.h"
#endif
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
namespace
{
PluginDescriptor fromJuceDescription (const juce::PluginDescription& source)
{
    PluginDescriptor descriptor;
    descriptor.name = source.name.toStdString();
    descriptor.descriptiveName = source.descriptiveName.toStdString();
    descriptor.manufacturer = source.manufacturerName.toStdString();
    descriptor.category = source.category.toStdString();
    descriptor.version = source.version.toStdString();
    descriptor.formatName = source.pluginFormatName.toStdString();
    const auto nativeIdentifier = hosting::splitNativeIdentifier (
        source.fileOrIdentifier);
    if (nativeIdentifier.pluginId.isNotEmpty())
    {
        descriptor.backend = PluginBackend::Native;
        descriptor.location = nativeIdentifier.bundlePath.toStdString();
        descriptor.pluginId = nativeIdentifier.pluginId.toStdString();
    }
    else
    {
        descriptor.backend = PluginBackend::JuceLegacy;
        descriptor.location = source.fileOrIdentifier.toStdString();
    }
    descriptor.uniqueId = source.uniqueId;
    descriptor.deprecatedUid = source.deprecatedUid;
    descriptor.numInputChannels = source.numInputChannels;
    descriptor.numOutputChannels = source.numOutputChannels;
    descriptor.lastFileModificationMs = source.lastFileModTime.toMilliseconds();
    descriptor.lastInfoUpdateMs = source.lastInfoUpdateTime.toMilliseconds();
    descriptor.isInstrument = source.isInstrument;
    descriptor.hasSharedContainer = source.hasSharedContainer;
    descriptor.hasAraExtension = source.hasARAExtension;
    return descriptor;
}

juce::PluginDescription toJuceDescription (const PluginDescriptor& source)
{
    juce::PluginDescription descriptor;
    descriptor.name = source.name;
    descriptor.descriptiveName = source.descriptiveName;
    descriptor.manufacturerName = source.manufacturer;
    descriptor.category = source.category;
    descriptor.version = source.version;
    descriptor.pluginFormatName = source.formatName;
    descriptor.fileOrIdentifier = source.backend == PluginBackend::Native
        ? hosting::joinNativeIdentifier (source.location, source.pluginId)
        : juce::String (source.location);
    descriptor.uniqueId = source.uniqueId;
    descriptor.deprecatedUid = source.deprecatedUid;
    descriptor.numInputChannels = source.numInputChannels;
    descriptor.numOutputChannels = source.numOutputChannels;
    descriptor.lastFileModTime = juce::Time (source.lastFileModificationMs);
    descriptor.lastInfoUpdateTime = juce::Time (source.lastInfoUpdateMs);
    descriptor.isInstrument = source.isInstrument;
    descriptor.hasSharedContainer = source.hasSharedContainer;
    descriptor.hasARAExtension = source.hasAraExtension;
    return descriptor;
}

std::optional<std::vector<PluginDescriptor>> importLegacyNativeCache (
    const juce::String& xmlSource,
    const std::function<bool (const juce::File&)>& locationExists)
{
    std::vector<PluginDescriptor> imported;
    const auto xml = juce::parseXML (xmlSource);
    if (xml == nullptr || ! xml->hasTagName ("KNOWNPLUGINS"))
        return std::nullopt;

    for (auto* child : xml->getChildIterator())
    {
        juce::PluginDescription legacy;
        if (! legacy.loadFromXml (*child))
            continue;
        auto descriptor = fromJuceDescription (legacy);
        descriptor.backend = PluginBackend::Native;
        if (descriptor.formatName == "LV2-Native") descriptor.formatName = "LV2";
        if (descriptor.formatName == "VST3-Native") descriptor.formatName = "VST3";
        const auto split = hosting::splitNativeIdentifier (legacy.fileOrIdentifier);
        descriptor.location = split.bundlePath.toStdString();
        descriptor.pluginId = split.pluginId.toStdString();
        if (! locationExists || locationExists (juce::File (descriptor.location)))
            imported.push_back (std::move (descriptor));
    }
    return imported;
}

bool loadNativeCacheSources (
    const std::optional<std::string>& jsonSource,
    const std::optional<juce::String>& legacyXmlSource,
    const std::function<bool (const juce::File&)>& locationExists,
    std::vector<PluginDescriptor>& into)
{
    std::vector<PluginDescriptor> fresh;
    if (jsonSource.has_value()
        && nativecache::parse (*jsonSource,
            [&locationExists] (std::string_view location)
            {
                const juce::File bundle (juce::String::fromUTF8 (
                    location.data(), static_cast<int> (location.size())));
                return ! locationExists || locationExists (bundle);
            },
            fresh))
    {
        into = std::move (fresh);
        return true;
    }

    if (legacyXmlSource.has_value())
    {
        auto imported = importLegacyNativeCache (*legacyXmlSource, locationExists);
        if (imported.has_value())
        {
            into = std::move (*imported);
            return true;
        }
    }

    return false;
}
} // namespace

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
                               const std::atomic<bool>* abortFlag, int* skipCounter)
        : hostExecutable (std::move (hostExe)), knownList (list), abort (abortFlag),
          sandboxSkips (skipCounter),
          hostPresent (juce::File (hostExecutable).existsAsFile()) {}

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        // In-house formats are our own code and can't crash the host, so scan
        // them in-process. Third-party binary formats are sandboxed - and for
        // those we NEVER fall back to in-process: an unauthorized or crashy
        // plugin scanned in-process takes down the whole app (issue #45).
        if (! scanproto::formatRequiresSandbox (format.getName().toStdString()))
        {
            format.findAllTypesForFile (result, fileOrIdentifier);
            return true;
        }

        if (! hostPresent)
        {
            // No sandbox binary to isolate the scan with. Skip rather than probe
            // in-process. Returning true with an empty result marks the plugin
            // UNSCANNED (JUCE blacklists only on a false return) - it did nothing
            // wrong, and a later scan with the host present re-probes it.
            noteSandboxUnavailable (fileOrIdentifier);
            return true;
        }

        juce::ChildProcess proc;
        const juce::StringArray args { hostExecutable, "--scan",
                                       format.getName(), fileOrIdentifier };

        if (! proc.start (args, juce::ChildProcess::wantStdOut))
        {
            // Host present but wouldn't spawn (AV block, transient OS limit).
            // Same rule as above: skip, don't scan in-process, don't blacklist.
            noteSandboxUnavailable (fileOrIdentifier);
            return true;
        }

        juce::MemoryOutputStream captured;
        char buf[8192];
        // Wrap-safe elapsed check: unsigned subtraction is correct across a
        // single getMillisecondCounter() wrap (every ~49 days of uptime), so
        // compare the interval rather than an absolute deadline.
        const std::uint32_t startMs = juce::Time::getMillisecondCounter();
        bool aborted  = false;
        bool timedOut = false;

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

            if (juce::Time::getMillisecondCounter() - startMs >= (std::uint32_t) kScanTimeoutMs)
            {
                proc.kill();                         // TerminateProcess on Windows - unblocks a modal dialog
                proc.waitForProcessToFinish (200);   // reap the SIGKILLed child, no zombie
                timedOut = true;
                break;
            }
            juce::Thread::sleep (5);
        }

        // Abort tears the whole scan down; leave the in-flight file UNSCANNED
        // (return true) rather than blacklisting a plugin the user cancelled on.
        if (aborted) return true;

        const auto payload = scanproto::extractPayload (captured.toString().toStdString());

        if (timedOut || payload.empty())
        {
            // A hang (timed out) or a crash (child died with no clean payload):
            // quarantine so the next scan skips it instead of re-hanging /
            // re-crashing. A hung scan is most often a plugin blocking on a
            // license / authorization dialog it cannot show during discovery.
            knownList.addToBlacklist (fileOrIdentifier);
            std::fprintf (stderr, timedOut
                ? "[Dusk Studio/scan] quarantined \"%s\": timed out (possible license dialog)\n"
                : "[Dusk Studio/scan] quarantined \"%s\": scan crashed (no payload)\n",
                fileOrIdentifier.toRawUTF8());
            std::fflush (stderr);
            return false;
        }

        const auto parsed = scanproto::parsePayload (payload);
        if (! parsed.has_value())
        {
            knownList.addToBlacklist (fileOrIdentifier);
            std::fprintf (stderr,
                          "[Dusk Studio/scan] quarantined \"%s\": malformed scan payload\n",
                          fileOrIdentifier.toRawUTF8());
            std::fflush (stderr);
            return false;
        }

        // A valid payload means the child completed the scan without crashing,
        // so this is successful even when the file yields zero descriptions.
        for (const auto& descriptor : *parsed)
            result.add (new juce::PluginDescription (toJuceDescription (descriptor)));
        return true;
    }

private:
    void noteSandboxUnavailable (const juce::String& fileOrIdentifier)
    {
        if (sandboxSkips != nullptr) ++(*sandboxSkips);
        std::fprintf (stderr,
                      "[Dusk Studio/scan] sandbox unavailable - left unscanned: %s\n",
                      fileOrIdentifier.toRawUTF8());
        std::fflush (stderr);
    }

    juce::String                 hostExecutable;
    juce::KnownPluginList&        knownList;
    const std::atomic<bool>*      abort;         // polled mid-file; null = never aborts
    int*                          sandboxSkips;   // ++ per third-party file left unscanned; may be null
    const bool                    hostPresent;    // sandbox child binary exists at construction
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

    loadCache();
    // Restore native-format descriptions so the picker has them at launch.
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    loadNativeCache (clapDescriptions, "clap-cache.json", "clap-cache.xml", false);
#endif
#if DUSKSTUDIO_HAS_NATIVE_LV2
    loadNativeCache (lv2Descriptions, "lv2-native-cache.json", "lv2-native-cache.xml", true);
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
    loadNativeCache (vst3NativeDescriptions, "vst3-native-cache.json",
                     "vst3-native-cache.xml", false);
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
    // Sandbox third-party plugin discovery in the child process. A plugin that
    // crashes/hangs its scan kills only the child; we quarantine it and carry on.
    // The custom scanner is installed UNCONDITIONALLY: if the child binary is
    // missing or won't spawn, third-party formats are left UNSCANNED (and counted
    // here) rather than scanned in-process, so an unauthorized/crashy plugin can
    // never take down the app. First-party formats always scan in-process.
    int sandboxSkips = 0;
    const juce::File hostExe (getHostExecutablePath());
    knownPluginList.setCustomScanner (
        std::make_unique<OutOfProcessPluginScanner> (hostExe.getFullPathName(),
                                                     knownPluginList, abort, &sandboxSkips));
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
    // Release the child-launching scanner - load-time instantiation must stay
    // in-process and doesn't go through the custom scanner anyway.
    knownPluginList.setCustomScanner (nullptr);
    lastScanSandboxSkips.store (sandboxSkips, std::memory_order_relaxed);
   #endif

    // Prune dead entries so the picker never offers a plugin that can't load.
    // Two instantiation-free checks cover every format:
    //   1. Path-backed formats (VST3 / AU bundles / CLAP / soundfonts): the
    //      bundle or file is gone or hollowed out - see pluginBackingLooksDead.
    //   2. URI-backed formats (LV2, AU component IDs): the identifier is no
    //      longer discoverable in the format's live search paths because the
    //      bundle was uninstalled. searchPathsForPlugins re-reads the LV2 world /
    //      AU registry but does NOT instantiate, so a crashy plugin can't take
    //      down the scan, and one call yields the whole live set per format.
    // Skipped on abort - the list is mid-scan and incomplete.
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
            bool dead = pluginBackingLooksDead (desc.fileOrIdentifier.toStdString());

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
        scanClapPlugins();        // CLAP isn't a juce format - scan it alongside the JUCE pass
        scanLv2Plugins();         // native-LV2 rows are separate from JUCE's LV2 format
        scanVst3NativePlugins();  // native-VST3 rows are separate from JUCE's VST3 format
    }
    return added;
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_VST3
// One native bundle -> picker rows through the sandbox child (loading a bundle
// executes its code; a broken .so must kill the child, not the app). False =
// couldn't spawn (caller falls back in-process); a spawned child that crashes
// or times out yields no payload and the bundle is skipped - re-probed next
// scan, but never fatal.
bool PluginManager::scanNativeBundleSandboxed (const char* format, const juce::File& bundle,
                                               std::vector<PluginDescriptor>& into) const
{
    const juce::File hostExe (getHostExecutablePath());
    if (hostExe == juce::File() || ! hostExe.existsAsFile())
        return false;

    juce::ChildProcess proc;
    const juce::StringArray args { hostExe.getFullPathName(), "--scan-native",
                                   format, bundle.getFullPathName() };
    if (! proc.start (args, juce::ChildProcess::wantStdOut))
        return false;

    juce::MemoryOutputStream captured;
    char buf[8192];
    const std::uint32_t startMs = juce::Time::getMillisecondCounter();
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
        if (juce::Time::getMillisecondCounter() - startMs >= (std::uint32_t) kScanTimeoutMs)
        {
            proc.kill();
            proc.waitForProcessToFinish (200);   // reap the SIGKILLed child, no zombie
            break;
        }
        juce::Thread::sleep (5);
    }

    const auto payload = scanproto::extractPayload (captured.toString().toStdString());
    if (payload.empty())
    {
        // Crash / hang / no sentinels - treat as handled (skip the bundle) so the
        // caller doesn't re-execute the crashing code in-process.
        std::fprintf (stderr, "[Dusk Studio/scan] native %s bundle skipped (child failed): %s\n",
                      format, bundle.getFullPathName().toRawUTF8());
        return true;
    }
    auto found = scanproto::parsePayload (payload);
    if (! found.has_value())
    {
        std::fprintf (stderr,
                      "[Dusk Studio/scan] native %s bundle skipped (malformed child payload): %s\n",
                      format, bundle.getFullPathName().toRawUTF8());
        return true;
    }
    into.insert (into.end(), std::make_move_iterator (found->begin()),
                 std::make_move_iterator (found->end()));
    return true;
}
#endif

void PluginManager::scanClapPlugins()
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
    // Discover OUTSIDE the lock (executes every bundle's factory - slow), swap
    // in under it. The cache write also stays outside so a picker open on the
    // message thread can't stall behind this thread's file I/O.
    std::vector<PluginDescriptor> fresh;
    for (const auto& path : clap::ClapScanner::findClapFiles (clap::ClapScanner::defaultSearchPaths()))
    {
        const juce::File file (juce::String::fromUTF8 (path.u8string().c_str()));
        if (! scanNativeBundleSandboxed ("clap", file, fresh))
            nativescan::appendClapRows (path, fresh);   // no sandbox available - in-process
    }
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        clapDescriptions.swap (fresh);
    }
    saveNativeCache (clapDescriptions, "clap-cache.json");
#endif
}

juce::File PluginManager::nativeCacheFile (const char* fileName) const
{
    const auto base = getCacheFile();
    return base == juce::File() ? juce::File() : base.getSiblingFile (fileName);
}

void PluginManager::loadNativeCache (std::vector<PluginDescriptor>& into,
                                     const char* jsonFileName, const char* legacyXmlFileName,
                                     bool bundleIsDirectory)
{
    std::vector<PluginDescriptor> fresh;
    const auto locationExists = [bundleIsDirectory] (const juce::File& bundle)
    {
        return bundleIsDirectory ? bundle.isDirectory() : bundle.exists();
    };
    bool loaded = false;
    const auto jsonFile = nativeCacheFile (jsonFileName);
    if (jsonFile != juce::File() && jsonFile.existsAsFile())
    {
        loaded = loadNativeCacheSources (
            jsonFile.loadFileAsString().toStdString(), std::nullopt,
            locationExists, fresh);
    }

    if (! loaded)
    {
        const auto legacyFile = nativeCacheFile (legacyXmlFileName);
        if (legacyFile != juce::File() && legacyFile.existsAsFile())
            loaded = loadNativeCacheSources (
                std::nullopt, legacyFile.loadFileAsString(),
                locationExists, fresh);
    }

    if (! loaded)
        return;

    const juce::ScopedLock sl (nativeDescriptionsLock);
    into.swap (fresh);
}

void PluginManager::saveNativeCache (const std::vector<PluginDescriptor>& from,
                                     const char* jsonFileName) const
{
    const auto file = nativeCacheFile (jsonFileName);
    if (file == juce::File())
        return;

    // Snapshot under the lock; serialize + write with it released so the picker
    // can't stall behind the file I/O.
    std::vector<PluginDescriptor> snapshot;
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        snapshot = from;
    }
    file.replaceWithText (nativecache::serialize (snapshot));
}

std::vector<PluginDescriptor> PluginManager::filterByInstrumentFlag (
    const std::vector<PluginDescriptor>& source, bool wantInstrument) const
{
    const juce::ScopedLock sl (nativeDescriptionsLock);
    std::vector<PluginDescriptor> out;
    for (const auto& d : source)
        if (d.isInstrument == wantInstrument)
            out.push_back (d);
    return out;
}

std::vector<PluginDescriptor> PluginManager::getClapEffectDescriptions() const
{
    return filterByInstrumentFlag (clapDescriptions, false);
}

std::vector<PluginDescriptor> PluginManager::getClapInstrumentDescriptions() const
{
    return filterByInstrumentFlag (clapDescriptions, true);
}

void PluginManager::scanLv2Plugins()
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
    const auto scanned = lv2::Lv2Scanner::scan();   // manifest parse outside the lock
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        lv2Descriptions.clear();
        for (const auto& s : scanned)
        {
            // Audio effects (audio in + out) and instruments (atom/MIDI in,
            // audio out, no audio in - classified by Lv2Bundle::describePlugin).
            // MIDI-only utilities stay with the JUCE LV2 format.
            const bool effect = s.desc.audioInputs > 0 && s.desc.audioOutputs > 0;
            if (! effect && ! s.desc.isInstrument)
                continue;
            PluginDescriptor descriptor;
            descriptor.name = s.desc.name;
            descriptor.formatName = "LV2";
            descriptor.backend = PluginBackend::Native;
            descriptor.location = s.bundlePath;
            descriptor.pluginId = s.desc.uri;
            descriptor.isInstrument = s.desc.isInstrument;
            lv2Descriptions.push_back (std::move (descriptor));
        }
    }
    saveNativeCache (lv2Descriptions, "lv2-native-cache.json");
#endif
}

std::vector<PluginDescriptor> PluginManager::getLv2EffectDescriptions() const
{
    return filterByInstrumentFlag (lv2Descriptions, false);
}

std::vector<PluginDescriptor> PluginManager::getLv2InstrumentDescriptions() const
{
    return filterByInstrumentFlag (lv2Descriptions, true);
}

void PluginManager::scanVst3NativePlugins()
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
    std::vector<PluginDescriptor> fresh;
    for (const auto& path : vst3::Vst3Scanner::findVst3Bundles (vst3::Vst3Scanner::defaultSearchPaths()))
    {
        const juce::File file (juce::String::fromUTF8 (path.u8string().c_str()));
        if (! scanNativeBundleSandboxed ("vst3", file, fresh))
            nativescan::appendVst3Rows (path, fresh);   // no sandbox available - in-process
    }
    {
        const juce::ScopedLock sl (nativeDescriptionsLock);
        vst3NativeDescriptions.swap (fresh);
    }
    saveNativeCache (vst3NativeDescriptions, "vst3-native-cache.json");
#endif
}

std::vector<PluginDescriptor> PluginManager::getVst3NativeEffectDescriptions() const
{
    return filterByInstrumentFlag (vst3NativeDescriptions, false);
}

std::vector<PluginDescriptor> PluginManager::getVst3NativeInstrumentDescriptions() const
{
    return filterByInstrumentFlag (vst3NativeDescriptions, true);
}

std::vector<PluginDescriptor> PluginManager::getInstrumentDescriptions() const
{
    std::vector<PluginDescriptor> instruments;
    for (const auto& desc : knownPluginList.getTypes())
        if (desc.isInstrument)
            instruments.push_back (fromJuceDescription (desc));
    return instruments;
}

std::vector<PluginDescriptor> PluginManager::getEffectDescriptions() const
{
    std::vector<PluginDescriptor> effects;
    for (const auto& desc : knownPluginList.getTypes())
        if (! desc.isInstrument)
            effects.push_back (fromJuceDescription (desc));
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

    return createPluginInstance (fromJuceDescription (*typesFound.getFirst()),
                                 sampleRate, blockSize, errorMessage);
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
PluginManager::createPluginInstance (const PluginDescriptor& desc,
                                      double sampleRate, int blockSize,
                                      juce::String& errorMessage)
{
    auto instance = formatManager.createPluginInstance (
        toJuceDescription (desc), sampleRate, blockSize, errorMessage);
    if (instance == nullptr)
        return nullptr;

    applyDefaultBusLayout (*instance);
    return instance;
}

void PluginManager::createPluginInstanceAsync (
    const PluginDescriptor& desc, double sampleRate, int blockSize,
    std::function<void (std::unique_ptr<juce::AudioPluginInstance>, juce::String)> callback)
{
    // Off-thread creation for slow-to-instantiate formats. JUCE runs the
    // format's createInstance on a background thread when the format reports
    // requiresUnblockedMessageThreadDuringCreation() == false, then fires this
    // callback ON THE MESSAGE THREAD with the fully-built instance - so the
    // caller's swap-in logic stays single-threaded.
    formatManager.createPluginInstanceAsync (toJuceDescription (desc), sampleRate, blockSize,
        [cb = std::move (callback)]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err)
    {
        if (instance != nullptr)
            applyDefaultBusLayout (*instance);
        cb (std::move (instance), err);
    });
}

PluginDescriptor PluginManager::descriptorForInstance (juce::AudioPluginInstance& instance) const
{
    juce::PluginDescription descriptor;
    instance.fillInPluginDescription (descriptor);
    return fromJuceDescription (descriptor);
}

bool PluginManager::descriptorFromLegacyXml (const juce::String& source,
                                             PluginDescriptor& out) const
{
    const auto xml = juce::XmlDocument::parse (source);
    if (xml == nullptr)
        return false;
    juce::PluginDescription descriptor;
    if (! descriptor.loadFromXml (*xml))
        return false;
    out = fromJuceDescription (descriptor);
    return true;
}

juce::String PluginManager::descriptorToLegacyXml (const PluginDescriptor& source) const
{
    const auto xml = toJuceDescription (source).createXml();
    return xml != nullptr
        ? xml->toString (juce::XmlElement::TextFormat().singleLine())
        : juce::String();
}

#if defined(DUSKSTUDIO_TESTS)
PluginDescriptor PluginManager::descriptorFromJuceForTest (
    const juce::PluginDescription& source)
{
    return fromJuceDescription (source);
}

juce::PluginDescription PluginManager::descriptorToJuceForTest (
    const PluginDescriptor& source)
{
    return toJuceDescription (source);
}

std::vector<PluginDescriptor> PluginManager::importLegacyNativeCacheForTest (
    const juce::String& xml,
    const std::function<bool (const juce::File&)>& locationExists)
{
    return importLegacyNativeCache (xml, locationExists).value_or (
        std::vector<PluginDescriptor> {});
}

bool PluginManager::loadNativeCacheSourcesForTest (
    const std::optional<std::string>& jsonSource,
    const std::optional<juce::String>& legacyXmlSource,
    const std::function<bool (const juce::File&)>& locationExists,
    std::vector<PluginDescriptor>& into)
{
    return loadNativeCacheSources (
        jsonSource, legacyXmlSource, locationExists, into);
}
#endif
} // namespace duskstudio
