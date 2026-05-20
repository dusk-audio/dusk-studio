#include "PluginSlot.h"
#include "AtomicPark.h"
#include "PluginManager.h"
#include <cstdio>
#include <cstring>

namespace duskstudio
{
namespace
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS
// OOP timeout per processBlock round-trip. Generous because the child
// has to memcpy through SHM, run the plugin, memcpy back. A 100 ms cap
// is well under the audio-thread starvation point (~250 ms = visible
// glitch on most kernels) and well over a misbehaving plugin's typical
// stall.
constexpr long long kOopProcessTimeoutNs = 100'000'000LL;

// Watchdog budget for OOP plugins. The in-process kBudgetFraction = 0.6
// is too tight when the IPC round-trip already eats a few hundred
// microseconds at small buffer sizes. 0.85 leaves headroom for the
// plugin itself; OOP's hard timeout (kOopProcessTimeoutNs above) is
// the second line of defence.
constexpr double kOopBudgetFraction = 0.85;
#endif


// JUCE-hosted plugins often expose more than one bus (main + sidechain on
// effects, main + aux output on some synths). Our processBlock contract
// assumes a 2-channel buffer that maps directly to L/R, so any plugin that
// reports total channels > 2 ends up with its main-output channels outside
// our buffer and we hear silence. Reduce every non-main bus to disabled
// (0 channels) so getTotalNumInputChannels / getTotalNumOutputChannels
// align with what the host actually feeds the plugin.
//
// Best-effort: if setBusesLayout rejects the layout (some plugins refuse
// to disable certain buses), we leave the original layout untouched and
// move on - the existing channel-count branches in processStereoBlock will
// at least try the right thing for the most common cases.
void disableAuxiliaryBuses (juce::AudioPluginInstance& instance)
{
    auto layout = instance.getBusesLayout();

    auto disableTail = [] (juce::Array<juce::AudioChannelSet>& buses)
    {
        for (int i = 1; i < buses.size(); ++i)
            buses.set (i, juce::AudioChannelSet::disabled());
    };
    disableTail (layout.inputBuses);
    disableTail (layout.outputBuses);

    instance.setBusesLayout (layout);
}

// JUCE's Linux VST3 wrapper stores `fileOrIdentifier` as the inner .so
// path inside the bundle (e.g.
// "/.../Plugin.vst3/Contents/x86_64-linux/Plugin.so") in the descriptions
// it produces from fillInPluginDescription / findAllTypesForFile. But the
// AudioPluginFormatManager's findFormatForDescription gates on
// `format->fileMightContainThisPluginType`, which for VST3 demands the
// path end in `.vst3`. So a session that round-trips Diva's description
// through getDescriptionXmlForSave -> JSON -> loadFromXml fails to load
// with "No compatible plug-in format exists for this plug-in", AND a
// freshly-picked-from-cache description has the same problem because the
// scanned descriptions in KnownPluginList carry the same inner-.so path.
//
// Walk parents until we find a `.vst3` ancestor and rewrite the path.
// No-op when the path is already a `.vst3` (macOS / non-Linux / future
// JUCE that fixes this).
void normalizeVst3FileOrIdentifier (juce::PluginDescription& desc)
{
    if (desc.pluginFormatName != "VST3") return;
    if (juce::File (desc.fileOrIdentifier).hasFileExtension (".vst3")) return;

    for (auto walk = juce::File (desc.fileOrIdentifier).getParentDirectory();
         walk.exists() && walk.getFullPathName().isNotEmpty();
         walk = walk.getParentDirectory())
    {
        if (walk.hasFileExtension (".vst3"))
        {
            desc.fileOrIdentifier = walk.getFullPathName();
            return;
        }
        if (walk.getParentDirectory() == walk) break;  // hit fs root
    }
}

// One-shot diagnostic so the user (and we) can see exactly what JUCE is
// reporting after load + bus-layout pass. Helps debug "plugin loaded but
// silent" cases like an instrument that ends up looking like an effect
// because of an auto-enabled sidechain bus.
void logLoadedPlugin (const juce::AudioPluginInstance& instance)
{
    juce::PluginDescription desc;
    instance.fillInPluginDescription (desc);
    std::fprintf (stderr,
                  "[Dusk Studio/PluginSlot] Loaded \"%s\" (instrument=%d) — "
                  "totalIn=%d totalOut=%d busesIn=%d busesOut=%d latency=%d\n",
                  desc.name.toRawUTF8(),
                  (int) desc.isInstrument,
                  instance.getTotalNumInputChannels(),
                  instance.getTotalNumOutputChannels(),
                  instance.getBusCount (true),
                  instance.getBusCount (false),
                  instance.getLatencySamples());
}
} // namespace

PluginSlot::~PluginSlot()
{
    // Audio thread should already be detached by the time this runs (the
    // owning ChannelStrip destructs after its AudioEngine has released the
    // device callback). Belt-and-suspenders: clear the atomic first so
    // nothing reads from the instance during destruction.
    currentInstance.store (nullptr, std::memory_order_release);
    // Detach the parameter listener before the instance destructs so
    // JUCE's listener list (held inside each param) doesn't dangle on
    // the released LastTouchedListener. Same detach-first ordering as
    // unload() - closes the window where a plugin-UI-thread callback
    // could race the destructor.
    if (ownedInstance != nullptr && lastTouchedListener != nullptr)
        for (auto* p : ownedInstance->getParameters())
            if (p != nullptr) p->removeListener (lastTouchedListener.get());
    lastTouchedListener.reset();
    if (ownedInstance != nullptr)
        ownedInstance->releaseResources();
    if (previousInstance != nullptr)
        previousInstance->releaseResources();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    // ~RemotePluginConnection sends SIGTERM/SIGKILL to the child and
    // unmaps SHM. Safe at process shutdown - audio thread is detached.
    ownedRemote.reset();
    previousRemote.reset();
   #endif
}

void PluginSlot::leakInstanceForShutdown()
{
    currentInstance.store (nullptr, std::memory_order_release);
    if (ownedInstance != nullptr)
        (void) ownedInstance.release();
    if (previousInstance != nullptr)
        (void) previousInstance.release();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // OOP plugins live in a separate process — the in-process leak hack
    // is irrelevant. Cleanly disconnecting kills the child, which
    // releases its plugin in its own address space (so any plugin-side
    // dtor crash is confined to the child and ignored).
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    ownedRemote.reset();
    previousRemote.reset();
   #endif
}

#if DUSKSTUDIO_HAS_OOP_PLUGINS
void PluginSlot::pollRemoteReaper()
{
    auto* r = ownedRemote.get();
    if (r == nullptr) { reaperTimer.stopTimer(); return; }

    if (r->pollReaper())
    {
        // Child has exited. Park the audio path immediately (defense in
        // depth — the audio thread will set autoBypassed itself on the
        // next processBlockSync, but proactively bypassing closes the
        // window where transport-stopped slots silently hold a dead
        // connection).
        autoBypassed .store (true, std::memory_order_relaxed);
        remoteCrashed.store (true, std::memory_order_relaxed);
        currentRemote.store (nullptr, std::memory_order_release);
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP child process exited; slot "
                      "auto-bypassed. Reload the plugin to recover.\n");
        // Stop polling — child has been reaped, nothing more to watch.
        reaperTimer.stopTimer();
    }
}
#endif

void PluginSlot::clearAutoBypass() noexcept
{
    autoBypassed.store (false, std::memory_order_relaxed);
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Clearing crashed state too: if the user explicitly asks to
    // re-enable a crashed slot, drop the dead connection so the next
    // load (or processBlock attempt) doesn't see a stale carcass. The
    // saved description XML stays so a subsequent restoreFromSavedState
    // can re-spawn cleanly. Same deferred-destruction shape as the
    // swap paths: hand off into previousRemote so any audio block
    // that's still holding a freshly-null'd currentRemote pointer
    // can complete safely.
    if (remoteCrashed.load (std::memory_order_relaxed))
    {
        previousRemote.reset();
        previousRemote = std::move (ownedRemote);
        remoteCrashed.store (false, std::memory_order_relaxed);
    }
   #endif
}

bool PluginSlot::showRemoteEditor (std::uint64_t& windowIdOut,
                                     int& widthOut, int& heightOut)
{
    windowIdOut = 0; widthOut = 0; heightOut = 0;
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    auto* r = ownedRemote.get();
    if (r == nullptr) return false;
    std::string err;
    if (! r->showEditor (windowIdOut, widthOut, heightOut, err))
    {
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP showEditor failed: %s\n",
                      err.c_str());
        return false;
    }
    return true;
   #else
    return false;
   #endif
}

bool PluginSlot::hideRemoteEditor()
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    auto* r = ownedRemote.get();
    if (r == nullptr) return false;
    std::string err;
    if (! r->hideEditor (err))
    {
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP hideEditor failed: %s\n",
                      err.c_str());
        return false;
    }
    return true;
   #else
    return false;
   #endif
}

bool PluginSlot::resizeRemoteEditor (int width, int height)
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    auto* r = ownedRemote.get();
    if (r == nullptr) return false;
    std::string err;
    if (! r->resizeEditor (width, height, err))
    {
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP resizeEditor failed: %s\n",
                      err.c_str());
        return false;
    }
    return true;
   #else
    juce::ignoreUnused (width, height);
    return false;
   #endif
}

void PluginSlot::prepareToPlay (double sampleRate, int blockSize)
{
    preparedSampleRate = sampleRate;
    preparedBlockSize  = juce::jmax (1, blockSize);
    secondsPerTick     = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();

    // Pre-size the stereo scratch so the audio thread never allocates when
    // a stereo-only plugin is in the slot.
    stereoScratch.setSize (2, preparedBlockSize, false, false, true);

    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;

    if (ownedInstance != nullptr)
    {
        ownedInstance->setPlayConfigDetails (
            ownedInstance->getTotalNumInputChannels(),
            ownedInstance->getTotalNumOutputChannels(),
            sampleRate, preparedBlockSize);
        ownedInstance->prepareToPlay (sampleRate, preparedBlockSize);
        cachedLatencySamples.store (ownedInstance->getLatencySamples(),
                                      std::memory_order_relaxed);
        return;
    }

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (ownedRemote != nullptr)
    {
        // Re-prepare the child. Caller (AudioEngine) typically drives this
        // when sample rate or block size changes; bail to bypass if the
        // new block size exceeds what the IPC SHM was sized for.
        if (preparedBlockSize > duskstudio::ipc::kMaxBlock)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/PluginSlot] OOP path can't host blockSize=%d "
                          "(SHM max=%d); slot will be silent until reload.\n",
                          preparedBlockSize, duskstudio::ipc::kMaxBlock);
            currentRemote.store (nullptr, std::memory_order_release);
            cachedLatencySamples.store (0, std::memory_order_relaxed);
            return;
        }
        std::string err;
        if (! ownedRemote->prepareToPlay (sampleRate, preparedBlockSize, err))
        {
            std::fprintf (stderr,
                          "[Dusk Studio/PluginSlot] OOP prepareToPlay failed: %s\n",
                          err.c_str());
            currentRemote.store (nullptr, std::memory_order_release);
            cachedLatencySamples.store (0, std::memory_order_relaxed);
            return;
        }
        // Latency stays whatever loadPlugin reported; OOP doesn't currently
        // re-query it on prepareToPlay (the child reapplies the existing
        // setLatencySamples value). Leave cachedLatencySamples as set by
        // load.
        return;
    }
   #endif

    cachedLatencySamples.store (0, std::memory_order_relaxed);
}

void PluginSlot::releaseResources()
{
    currentInstance.store (nullptr, std::memory_order_release);
    if (ownedInstance != nullptr)
        ownedInstance->releaseResources();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    if (ownedRemote != nullptr)
    {
        std::string err;
        // release() asks the child to drop its plugin instance but keeps
        // the SHM + child process alive, so a subsequent load doesn't
        // pay the fork+exec cost again.
        (void) ownedRemote->release (err);
    }
   #endif
}

bool PluginSlot::isLoaded() const noexcept
{
    if (currentInstance.load (std::memory_order_acquire) != nullptr) return true;
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (currentRemote.load (std::memory_order_acquire) != nullptr)   return true;
   #endif
    return false;
}

juce::String PluginSlot::getLoadedName() const
{
    if (auto* p = currentInstance.load (std::memory_order_acquire))
        return p->getName();
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (currentRemote.load (std::memory_order_acquire) != nullptr)
    {
        // Parse the cached description XML for the plugin's display
        // name. Cheap (a few hundred bytes of XML) and avoids an IPC
        // round-trip just for a label.
        if (savedDescriptionXml.isNotEmpty())
            if (auto xml = juce::XmlDocument::parse (savedDescriptionXml))
            {
                juce::PluginDescription desc;
                if (desc.loadFromXml (*xml))
                    return desc.name;
            }
    }
   #endif
    return {};
}

bool PluginSlot::isOffline() const noexcept
{
    if (isLoaded()) return false;
    return offlineDescriptionXml.isNotEmpty();
}

juce::String PluginSlot::getOfflineName() const
{
    if (offlineDescriptionXml.isEmpty()) return {};
    if (auto xml = juce::XmlDocument::parse (offlineDescriptionXml))
    {
        juce::PluginDescription desc;
        if (desc.loadFromXml (*xml))
            return desc.name;
    }
    return {};
}

bool PluginSlot::loadFromFile (const juce::File& pluginFile, juce::String& errorMessage)
{
    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound - call setManager() first";
        return false;
    }

    // Park the audio thread first. Detach the current instance, then move
    // ownership of the prior plugin into previousInstance so its
    // destructor is deferred until the NEXT swap (see previousInstance
    // doc comment). This is the difference between a clean Replace and a
    // crash inside the audio callback under live device operation.
    currentInstance.store (nullptr, std::memory_order_release);
    if (previousInstance != nullptr)
        previousInstance->releaseResources();
    previousInstance = std::move (ownedInstance);
    ownedInstance.reset();

    auto fresh = manager->createPluginInstance (pluginFile,
                                                  preparedSampleRate,
                                                  preparedBlockSize,
                                                  errorMessage);
    if (fresh == nullptr)
        return false;

    // Strip auxiliary buses (sidechain inputs, secondary outputs) BEFORE
    // setPlayConfigDetails so the channel counts we report match the
    // actual buffer width we'll pass at processBlock time. Without this
    // an instrument with a sidechain bus auto-enabled would look like
    // a 2-in / 2-out effect to processStereoBlock and the plugin's main
    // output would land in channels we never read.
    disableAuxiliaryBuses (*fresh);
    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate,
                                  preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);
    logLoadedPlugin (*fresh);

    ownedInstance = std::move (fresh);
    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;
    autoBypassed.store (false, std::memory_order_relaxed);
    if (hostPlayHead != nullptr)
        ownedInstance->setPlayHead (hostPlayHead);
    cachedLatencySamples.store (ownedInstance->getLatencySamples(),
                                  std::memory_order_relaxed);
    // MIDI Learn last-touched: install a parameter listener on every
    // exposed parameter so the user's plugin-UI moves stamp
    // lastTouchedParamIndex. Cheap on load (small enum) and lock-free
    // at runtime.
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);
    lastTouchedListener = std::make_unique<LastTouchedListener> (lastTouchedParamIndex);
    for (auto* p : ownedInstance->getParameters())
        if (p != nullptr) p->addListener (lastTouchedListener.get());
    currentInstance.store (ownedInstance.get(), std::memory_order_release);
    offlineDescriptionXml.clear();
    offlineStateBase64.clear();
    return true;
}

bool PluginSlot::loadFromDescription (const juce::PluginDescription& desc,
                                        juce::String& errorMessage)
{
    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound - call setManager() first";
        return false;
    }

    // Normalize the path - cached KnownPluginList descriptions on Linux
    // carry the inner-.so path which findFormatForDescription rejects.
    juce::PluginDescription fixedDesc = desc;
    normalizeVst3FileOrIdentifier (fixedDesc);

    // Same swap-load shape as loadFromFile; just resolves via the
    // description path inside PluginManager. The previous owner moves
    // into `previousInstance` so its destructor is deferred until the
    // NEXT swap (or this PluginSlot's destruction). That guarantees at
    // least one full audio block elapses between the audio thread last
    // possibly seeing the old pointer and the destructor firing.
    currentInstance.store (nullptr, std::memory_order_release);
    if (previousInstance != nullptr)
        previousInstance->releaseResources();
    previousInstance = std::move (ownedInstance);
    ownedInstance.reset();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Tear down any prior OOP slot before deciding which path the new
    // load takes. Mode is per-load: we may end up in-process this time
    // even if the previous load was OOP (and vice versa).
    //
    // Deferred-destruction: move the just-deposed ownedRemote into
    // previousRemote rather than destroying it in place. The audio
    // thread may have just loaded currentRemote pre-store-nullptr and
    // be inside processBlockSync right now; tearing the SHM mapping
    // and child process down underneath that call is the classic
    // use-after-free shape. previousRemote holds the connection alive
    // for one more swap cycle; the previousRemote.reset() below
    // destroys whatever was deposed on the PRIOR swap, by which
    // point any audio block that started during that swap has long
    // since completed.
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    previousRemote.reset();
    previousRemote = std::move (ownedRemote);
    remoteCrashed.store (false, std::memory_order_relaxed);
    savedDescriptionXml.clear();

    const bool tryOop = manager->isOopEnabled()
                         && preparedBlockSize > 0
                         && preparedBlockSize <= duskstudio::ipc::kMaxBlock;

    if (tryOop)
    {
        const auto hostPath = manager->getHostExecutablePath();
        if (hostPath.isEmpty() || ! juce::File (hostPath).existsAsFile())
        {
            std::fprintf (stderr,
                          "[Dusk Studio/PluginSlot] OOP requested but host binary not found "
                          "at \"%s\"; falling back to in-process.\n",
                          hostPath.toRawUTF8());
        }
        else
        {
            auto remote = std::make_unique<duskstudio::ipc::RemotePluginConnection>();
            std::string err;
            if (! remote->connect (hostPath.toStdString(), "--ipc-host", err))
            {
                std::fprintf (stderr,
                              "[Dusk Studio/PluginSlot] OOP connect failed (%s); "
                              "falling back to in-process.\n",
                              err.c_str());
            }
            else
            {
                auto descXml = fixedDesc.createXml();
                const auto descXmlStr = descXml != nullptr
                    ? descXml->toString (juce::XmlElement::TextFormat().singleLine())
                    : juce::String();
                int  numIn = 0, numOut = 0, latency = 0;
                if (! remote->loadPlugin (descXmlStr.toStdString(),
                                            preparedSampleRate, preparedBlockSize,
                                            numIn, numOut, latency, err))
                {
                    std::fprintf (stderr,
                                  "[Dusk Studio/PluginSlot] OOP loadPlugin failed (%s); "
                                  "falling back to in-process.\n",
                                  err.c_str());
                }
                else
                {
                    remoteNumIn .store (numIn,  std::memory_order_relaxed);
                    remoteNumOut.store (numOut, std::memory_order_relaxed);
                    remoteIsInstrument.store (numIn == 0,
                                                std::memory_order_relaxed);
                    cachedLatencySamples.store (latency, std::memory_order_relaxed);
                    blocksSinceLoad     = 0;
                    consecutiveOverruns = 0;
                    autoBypassed .store (false, std::memory_order_relaxed);
                    remoteCrashed.store (false, std::memory_order_relaxed);
                    savedDescriptionXml = descXmlStr;
                    ownedRemote = std::move (remote);
                    currentRemote.store (ownedRemote.get(),
                                            std::memory_order_release);
                    reaperTimer.startTimer (kReaperPeriodMs);
                    offlineDescriptionXml.clear();
                    offlineStateBase64.clear();
                    return true;
                }
            }
        }
        // Fall through into the in-process load below — we still want
        // the user's load to succeed.
    }
   #endif

    auto fresh = manager->createPluginInstance (fixedDesc, preparedSampleRate,
                                                  preparedBlockSize, errorMessage);
    if (fresh == nullptr)
    {
        // No new plugin to install. Slot is now empty (currentInstance is
        // nullptr, ownedInstance reset). previousInstance still holds the
        // pre-swap plugin and will be released on the next swap.
        return false;
    }

    // Strip auxiliary buses (sidechain inputs, secondary outputs) BEFORE
    // setPlayConfigDetails so the channel counts we report match the
    // actual buffer width we'll pass at processBlock time. Without this
    // an instrument with a sidechain bus auto-enabled would look like
    // a 2-in / 2-out effect to processStereoBlock and the plugin's main
    // output would land in channels we never read.
    disableAuxiliaryBuses (*fresh);
    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate,
                                  preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);
    logLoadedPlugin (*fresh);

    ownedInstance = std::move (fresh);
    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;
    autoBypassed.store (false, std::memory_order_relaxed);
    if (hostPlayHead != nullptr)
        ownedInstance->setPlayHead (hostPlayHead);
    cachedLatencySamples.store (ownedInstance->getLatencySamples(),
                                  std::memory_order_relaxed);
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);
    lastTouchedListener = std::make_unique<LastTouchedListener> (lastTouchedParamIndex);
    for (auto* p : ownedInstance->getParameters())
        if (p != nullptr) p->addListener (lastTouchedListener.get());
    currentInstance.store (ownedInstance.get(), std::memory_order_release);
    offlineDescriptionXml.clear();
    offlineStateBase64.clear();
    return true;
}

void PluginSlot::unload()
{
    // Same deferred-destruction pattern as the load* functions: move
    // the current owner into previousInstance so its destructor only
    // fires on the NEXT swap (or this PluginSlot's destruction). Direct
    // destruction here races the audio thread.
    currentInstance.store (nullptr, std::memory_order_release);
    cachedLatencySamples.store (0, std::memory_order_relaxed);
    // Detach parameter listeners BEFORE clearing lastTouchedParamIndex
    // and BEFORE the instance moves into previousInstance. Detach-first
    // closes the window where an in-flight plugin-UI-thread parameter
    // callback could fire on the listener and over-write the -1 we're
    // about to store into the atom. Once the listener is removed from
    // each param's listener list, JUCE guarantees no further callbacks.
    if (ownedInstance != nullptr && lastTouchedListener != nullptr)
        for (auto* p : ownedInstance->getParameters())
            if (p != nullptr) p->removeListener (lastTouchedListener.get());
    lastTouchedListener.reset();
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);
    if (previousInstance != nullptr)
        previousInstance->releaseResources();
    previousInstance = std::move (ownedInstance);
    ownedInstance.reset();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Same deferred-destruction shape as loadFromDescription: defer the
    // child-process kill + SHM unmap to the NEXT swap so an in-flight
    // audio-thread processBlockSync on the just-cleared currentRemote
    // can complete cleanly. previousRemote.reset() destroys whatever
    // was deposed on the previous cycle (one full swap ago).
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    previousRemote.reset();
    previousRemote = std::move (ownedRemote);
    remoteNumIn .store (0, std::memory_order_relaxed);
    remoteNumOut.store (0, std::memory_order_relaxed);
    remoteIsInstrument.store (false, std::memory_order_relaxed);
    remoteCrashed.store (false, std::memory_order_relaxed);
    savedDescriptionXml.clear();
   #endif
    offlineDescriptionXml.clear();
    offlineStateBase64.clear();
}

juce::String PluginSlot::getDescriptionXmlForSave (int parkSleepMs)
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (currentRemote.load (std::memory_order_acquire) != nullptr
        && savedDescriptionXml.isNotEmpty())
    {
        // Saved at load time. The XML is already path-normalized
        // (PluginManager's caller passed a normalized desc, or
        // loadFromDescription's normalization ran).
        return savedDescriptionXml;
    }
   #endif

    // Slot is empty in-process but holds an offline placeholder from a
    // failed restoreFromSavedState. Return the stashed XML so the next
    // save round-trips the user's data instead of wiping it.
    if (currentInstance.load (std::memory_order_acquire) == nullptr
        && offlineDescriptionXml.isNotEmpty())
        return offlineDescriptionXml;

    juce::PluginDescription desc;
    bool ok = false;
    // fillInPluginDescription is metadata-only (uid + format + path + name)
    // and does NOT race the renderer. No releaseResources/prepareToPlay
    // needed — atomic-park alone is sufficient.
    withParkedAtomicPointer (currentInstance, [&] (juce::AudioPluginInstance& p)
    {
        p.fillInPluginDescription (desc);
        ok = true;
    }, parkSleepMs);
    if (! ok) return {};
    // JUCE's Linux VST3 wrapper fills the description with the inner-.so
    // path. Normalize back to the bundle path so the saved session loads
    // cleanly without relying on restoreFromSavedState's recovery walk.
    normalizeVst3FileOrIdentifier (desc);
    if (auto xml = desc.createXml())
        return xml->toString (juce::XmlElement::TextFormat().singleLine());
    return {};
}

bool PluginSlot::isLoadedPluginInstrument() const
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (currentRemote.load (std::memory_order_acquire) != nullptr)
        return remoteIsInstrument.load (std::memory_order_relaxed);
   #endif

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return false;
    juce::PluginDescription desc;
    p->fillInPluginDescription (desc);
    return desc.isInstrument;
}

juce::String PluginSlot::getStateBase64ForSave (int parkSleepMs)
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (auto* r = currentRemote.load (std::memory_order_acquire))
    {
        // OOP path: the parking + setActive(false) bracket is intrinsic
        // to the IPC — the child runs the plugin on its own audio worker
        // and serialises getStateInformation under MessageManagerLock.
        // No parent-side park needed; the parent just blocks on the
        // control-plane reply.
        std::vector<std::uint8_t> blob;
        std::string err;
        if (! r->getState (blob, err))
        {
            std::fprintf (stderr,
                          "[Dusk Studio/PluginSlot] OOP getState failed: %s\n",
                          err.c_str());
            return {};
        }
        if (blob.empty()) return {};
        return juce::MemoryBlock (blob.data(), blob.size()).toBase64Encoding();
    }
    juce::ignoreUnused (parkSleepMs);
   #endif

    // Slot is empty but holds an offline placeholder. Round-trip the
    // stashed state so a save while offline doesn't wipe the user's
    // data on disk.
    if (currentInstance.load (std::memory_order_acquire) == nullptr
        && offlineStateBase64.isNotEmpty())
        return offlineStateBase64;

    juce::MemoryBlock mb;
    // Atomic-park alone is NOT sufficient for state capture: it tells the
    // AUDIO thread to skip the plugin, but doesn't tell the PLUGIN that
    // it's now inactive. Plugins like u-he Diva keep their own
    // setActive(true) flag and check it inside getStateInformation - if
    // the flag is still on, Diva logs "ALERT getStateInfo INTERRUPTS
    // RENDER" and corrupts its internal state, which then blows up
    // later inside ~VST3PluginInstance with __cxa_pure_virtual.
    //
    // Bracket getStateInformation with releaseResources / prepareToPlay
    // so the plugin sees IComponent::setActive(false) before state I/O
    // and IComponent::setActive(true) after. The audio thread is parked
    // throughout, so the brief deactivation is invisible from its side.
    // If the slot has not been prepareToPlay'd yet (e.g. session load
    // mid-way), preparedSampleRate is 0 and we skip the resume - the
    // engine's next prepareToPlay will reconfigure it.
    auto* p = withParkedAtomicPointer (currentInstance,
        [&] (juce::AudioPluginInstance& inst)
        {
            inst.releaseResources();
            inst.getStateInformation (mb);
            if (preparedSampleRate > 0.0)
            {
                inst.setPlayConfigDetails (
                    inst.getTotalNumInputChannels(),
                    inst.getTotalNumOutputChannels(),
                    preparedSampleRate, preparedBlockSize);
                inst.prepareToPlay (preparedSampleRate, preparedBlockSize);
            }
        },
        parkSleepMs);
    if (p == nullptr) return {};
    return mb.toBase64Encoding();
}

bool PluginSlot::restoreFromSavedState (const juce::String& descriptionXml,
                                          const juce::String& stateBase64,
                                          juce::String& errorMessage)
{
    if (descriptionXml.isEmpty())
    {
        // Saved session had no plugin on this slot - make sure the slot is
        // empty. Returning success because "no plugin to restore" is the
        // valid steady state, not an error.
        unload();
        return true;
    }

    // Stash the inputs up front. On any failure path below the slot
    // ends up empty, but these copies survive so getDescriptionXmlForSave
    // / getStateBase64ForSave still return the user's saved data — a
    // subsequent autosave or manual save then round-trips it instead of
    // wiping it because the plugin happened to be unavailable. Cleared
    // by every success path and by unload().
    offlineDescriptionXml = descriptionXml;
    offlineStateBase64    = stateBase64;

    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound";
        return false;
    }

    // Parse the description.
    auto xml = juce::XmlDocument::parse (descriptionXml);
    if (xml == nullptr)
    {
        errorMessage = "Saved plugin description is not valid XML";
        return false;
    }
    juce::PluginDescription desc;
    if (! desc.loadFromXml (*xml))
    {
        errorMessage = "Saved plugin description failed to deserialise";
        return false;
    }

    normalizeVst3FileOrIdentifier (desc);

    // Try the OOP path first if enabled. On any failure we fall through
    // to the in-process load below — the user's session restore should
    // succeed even if the host child can't be spawned for some reason
    // (e.g. binary not present in a stripped-down build).
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (manager->isOopEnabled()
        && preparedBlockSize > 0
        && preparedBlockSize <= duskstudio::ipc::kMaxBlock)
    {
        // Use the standard load path, then setState. loadFromDescription
        // handles parking, swap, and OOP/in-process choice; on success,
        // currentRemote is non-null when the OOP path took.
        if (loadFromDescription (desc, errorMessage))
        {
            if (auto* r = currentRemote.load (std::memory_order_acquire);
                r != nullptr && stateBase64.isNotEmpty())
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (stateBase64) && mb.getSize() > 0)
                {
                    std::string err;
                    if (! r->setState (static_cast<const std::uint8_t*> (mb.getData()),
                                         mb.getSize(), err))
                    {
                        std::fprintf (stderr,
                                      "[Dusk Studio/PluginSlot] OOP setState failed: %s\n",
                                      err.c_str());
                    }
                }
            }
            offlineDescriptionXml.clear();
            offlineStateBase64.clear();
            return true;
        }
        // loadFromDescription set errorMessage; fall through to retry
        // in-process so the user's session still loads.
    }
   #endif

    // Same swap-load shape as loadFromFile/loadFromDescription, with the
    // same deferred-destruction-via-previousInstance discipline.
    currentInstance.store (nullptr, std::memory_order_release);
    if (previousInstance != nullptr)
        previousInstance->releaseResources();
    previousInstance = std::move (ownedInstance);
    ownedInstance.reset();

    auto fresh = manager->createPluginInstance (desc, preparedSampleRate,
                                                  preparedBlockSize, errorMessage);
    if (fresh == nullptr)
        return false;

    disableAuxiliaryBuses (*fresh);
    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate, preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);
    logLoadedPlugin (*fresh);

    // Apply the saved state blob (if any).
    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (stateBase64) && mb.getSize() > 0)
            fresh->setStateInformation (mb.getData(), (int) mb.getSize());
    }

    ownedInstance = std::move (fresh);
    if (hostPlayHead != nullptr)
        ownedInstance->setPlayHead (hostPlayHead);
    cachedLatencySamples.store (ownedInstance->getLatencySamples(),
                                  std::memory_order_relaxed);
    currentInstance.store (ownedInstance.get(), std::memory_order_release);
    offlineDescriptionXml.clear();
    offlineStateBase64.clear();
    return true;
}

void PluginSlot::processMonoBlock (float* monoData, int numSamples,
                                   juce::MidiBuffer& midiMessages) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

    if (bypassed.load (std::memory_order_relaxed)
        || autoBypassed.load (std::memory_order_relaxed))
        return;

    // Time-budget watchdog. A plugin that consistently overruns the buffer
    // gets auto-bypassed so it can't freeze the audio thread. Two
    // refinements over a naive single-block trip:
    //   • Warm-up grace: kGraceBlocks after a load (or prepareToPlay) the
    //     watchdog is silent. Reverbs / look-ahead limiters / oversamplers
    //     all do real work on their first few blocks (cold caches, internal
    //     ramps) and would otherwise be auto-bypassed before they ever
    //     produce wet output.
    //   • Consecutive-overrun threshold: a single late block (other-thread
    //     preemption, GC, kernel scheduling jitter) shouldn't kill a
    //     plugin. Require kMaxConsecutiveOverruns in a row.
    constexpr int    kGraceBlocks            = 16;
    constexpr int    kMaxConsecutiveOverruns = 4;
    const double bufferMs = (preparedSampleRate > 0.0)
                              ? 1000.0 * (double) numSamples / preparedSampleRate
                              : 0.0;

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (auto* r = currentRemote.load (std::memory_order_acquire))
    {
        const auto rt0 = juce::Time::getHighResolutionTicks();
        const int rNumIn  = remoteNumIn .load (std::memory_order_relaxed);
        const int rNumOut = remoteNumOut.load (std::memory_order_relaxed);

        const float* inPtrs[2] = { monoData, nullptr };
        if (rNumIn >= 2)
        {
            // Stereo-in plugin in a mono slot: duplicate mono into both
            // channels of the scratch and feed both as input pointers.
            if (numSamples > stereoScratch.getNumSamples()) return;
            stereoScratch.copyFrom (0, 0, monoData, numSamples);
            stereoScratch.copyFrom (1, 0, monoData, numSamples);
            inPtrs[0] = stereoScratch.getReadPointer (0);
            inPtrs[1] = stereoScratch.getReadPointer (1);
        }
        else if (rNumIn == 0)
        {
            inPtrs[0] = nullptr;
        }

        if (! r->processBlockSync (inPtrs, juce::jmax (rNumIn, 0),
                                       numSamples, midiMessages,
                                       kOopProcessTimeoutNs))
        {
            engageAutoBypass();
            return;
        }

        if (rNumOut == 1)
        {
            std::memcpy (monoData, r->readOutChannel (0),
                          sizeof (float) * (size_t) numSamples);
        }
        else if (rNumOut >= 2)
        {
            const float* oL = r->readOutChannel (0);
            const float* oR = r->readOutChannel (1);
            for (int i = 0; i < numSamples; ++i)
                monoData[i] = (oL[i] + oR[i]) * 0.5f;
        }
        // rNumOut == 0: plugin produced nothing; leave monoData untouched.

        if (bufferMs > 0.0 && blocksSinceLoad >= kGraceBlocks)
        {
            const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - rt0)
                                      * secondsPerTick * 1000.0;
            if (elapsedMs > bufferMs * kOopBudgetFraction)
            {
                if (++consecutiveOverruns >= kMaxConsecutiveOverruns)
                    engageAutoBypass();
            }
            else
            {
                consecutiveOverruns = 0;
            }
        }
        if (blocksSinceLoad < kGraceBlocks) ++blocksSinceLoad;
        return;
    }
   #endif

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;

    constexpr double kBudgetFraction = 0.6;
    const auto t0 = juce::Time::getHighResolutionTicks();

    const int numIn  = p->getTotalNumInputChannels();
    const int numOut = p->getTotalNumOutputChannels();

    if (numIn == 1 && numOut == 1)
    {
        // Mono in / mono out - process directly in place via a thin
        // AudioBuffer wrapper around the existing buffer.
        float* channels[1] = { monoData };
        juce::AudioBuffer<float> buf (channels, 1, numSamples);
        p->processBlock (buf, midiMessages);
    }
    else if (numIn == 0 && numOut >= 1)
    {
        // Instrument plugin (no audio input - generates audio from MIDI).
        // Mirror of the corresponding branch in processStereoBlock so
        // either entry point handles instruments correctly. Today the
        // picker filter prevents loading an instrument on a Mono channel
        // slot, so this branch is unreachable in normal use; keeping it
        // symmetrical avoids a foot-gun if filtering ever loosens.
        if (numSamples > stereoScratch.getNumSamples()) return;

        const int procCh = juce::jmin (numOut, stereoScratch.getNumChannels());
        for (int c = 0; c < procCh; ++c)
            stereoScratch.clear (c, 0, numSamples);

        float* procPtrs[2] = { stereoScratch.getWritePointer (0),
                               procCh > 1 ? stereoScratch.getWritePointer (1) : nullptr };
        juce::AudioBuffer<float> buf (procPtrs, procCh, numSamples);
        p->processBlock (buf, midiMessages);

        const float* outL = stereoScratch.getReadPointer (0);
        const float* outR = (numOut >= 2 && procCh >= 2)
                              ? stereoScratch.getReadPointer (1) : outL;
        for (int i = 0; i < numSamples; ++i)
            monoData[i] = (outL[i] + outR[i]) * 0.5f;
    }
    else
    {
        // Stereo (or wider) plugin: duplicate mono → L+R, process, average
        // back to mono. Use the pre-allocated scratch so we don't touch
        // the heap.
        if (numSamples > stereoScratch.getNumSamples())
            return;

        stereoScratch.copyFrom (0, 0, monoData, numSamples);
        stereoScratch.copyFrom (1, 0, monoData, numSamples);
        p->processBlock (stereoScratch, midiMessages);

        const float* l = stereoScratch.getReadPointer (0);
        const float* r = stereoScratch.getReadPointer (1);
        for (int i = 0; i < numSamples; ++i)
            monoData[i] = (l[i] + r[i]) * 0.5f;
    }

    if (bufferMs > 0.0 && blocksSinceLoad >= kGraceBlocks)
    {
        const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - t0)
                                  * secondsPerTick * 1000.0;
        if (elapsedMs > bufferMs * kBudgetFraction)
        {
            if (++consecutiveOverruns >= kMaxConsecutiveOverruns)
                engageAutoBypass();
        }
        else
        {
            consecutiveOverruns = 0;
        }
    }
    if (blocksSinceLoad < kGraceBlocks) ++blocksSinceLoad;
}

void PluginSlot::processStereoBlock (float* L, float* R, int numSamples,
                                     juce::MidiBuffer& midiMessages) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

    if (bypassed.load (std::memory_order_relaxed)
        || autoBypassed.load (std::memory_order_relaxed))
        return;

    constexpr int    kGraceBlocks            = 16;
    constexpr int    kMaxConsecutiveOverruns = 4;
    const double bufferMs = (preparedSampleRate > 0.0)
                              ? 1000.0 * (double) numSamples / preparedSampleRate
                              : 0.0;

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (auto* r = currentRemote.load (std::memory_order_acquire))
    {
        constexpr double kRemoteBudgetFraction = kOopBudgetFraction;
        const auto rt0 = juce::Time::getHighResolutionTicks();

        const int rNumIn  = remoteNumIn .load (std::memory_order_relaxed);
        const int rNumOut = remoteNumOut.load (std::memory_order_relaxed);

        // Build input channel array for the IPC call. The parent's
        // memcpy loop in RemotePluginConnection only walks the first
        // rNumIn pointers, so for instruments (rNumIn=0) inChannels is
        // unused.
        const float* inPtrs[2] = { L, R };
        if (rNumIn == 1)
        {
            // Mono-in plugin on a stereo bus: average L+R into the
            // pre-allocated scratch and feed that as the single input
            // channel.
            if (numSamples > stereoScratch.getNumSamples()) return;
            float* mono = stereoScratch.getWritePointer (0);
            for (int i = 0; i < numSamples; ++i)
                mono[i] = (L[i] + R[i]) * 0.5f;
            inPtrs[0] = mono;
            inPtrs[1] = nullptr;
        }
        else if (rNumIn == 0)
        {
            inPtrs[0] = nullptr;
            inPtrs[1] = nullptr;
        }
        else if (rNumIn > 2)
        {
            // Should not happen - kMaxChans=2 in PluginIpc - but if a
            // future build raises the cap and this code wasn't updated,
            // bail rather than read garbage.
            engageAutoBypass();
            return;
        }

        if (! r->processBlockSync (inPtrs, juce::jmax (rNumIn, 0),
                                       numSamples, midiMessages,
                                       kOopProcessTimeoutNs))
        {
            // Either the futex timed out or the connection was already
            // marked crashed. Engage auto-bypass so the engine sees a
            // clean silence/pass-through instead of repeating the
            // timeout every block (the futex cost itself is what we're
            // avoiding here — re-trying a dead connection still pays
            // the deadline cost).
            engageAutoBypass();
            return;
        }

        // Read output channels from SHM and copy into L/R.
        if (rNumOut <= 0)
        {
            // Plugin produced nothing - pass dry signal through (effect)
            // or leave silence (instrument was already silent).
        }
        else if (rNumOut == 1)
        {
            const float* o = r->readOutChannel (0);
            std::memcpy (L, o, sizeof (float) * (size_t) numSamples);
            std::memcpy (R, o, sizeof (float) * (size_t) numSamples);
        }
        else // rNumOut >= 2
        {
            const float* oL = r->readOutChannel (0);
            const float* oR = r->readOutChannel (1);
            std::memcpy (L, oL, sizeof (float) * (size_t) numSamples);
            std::memcpy (R, oR, sizeof (float) * (size_t) numSamples);
        }

        if (bufferMs > 0.0 && blocksSinceLoad >= kGraceBlocks)
        {
            const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - rt0)
                                      * secondsPerTick * 1000.0;
            if (elapsedMs > bufferMs * kRemoteBudgetFraction)
            {
                if (++consecutiveOverruns >= kMaxConsecutiveOverruns)
                    engageAutoBypass();
            }
            else
            {
                consecutiveOverruns = 0;
            }
        }
        if (blocksSinceLoad < kGraceBlocks) ++blocksSinceLoad;
        return;
    }
   #endif

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;

    constexpr double kBudgetFraction = 0.6;
    const auto t0 = juce::Time::getHighResolutionTicks();

    const int numIn  = p->getTotalNumInputChannels();
    const int numOut = p->getTotalNumOutputChannels();

    if (numIn >= 2 && numOut >= 2)
    {
        // Stereo plugin - wrap L/R as a 2-channel AudioBuffer and process in
        // place. Same shape as the per-aux EQ/comp pass above.
        float* channels[2] = { L, R };
        juce::AudioBuffer<float> buf (channels, 2, numSamples);
        p->processBlock (buf, midiMessages);
    }
    else if (numIn == 1 && numOut >= 1)
    {
        // Mono-input plugin on a stereo bus: collapse to mono, run, then fan
        // out the (possibly stereo) output back across L/R. Use the
        // pre-allocated stereoScratch as the working buffer.
        if (numSamples > stereoScratch.getNumSamples()) return;

        float* mono = stereoScratch.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            mono[i] = (L[i] + R[i]) * 0.5f;

        // Buffer width must be max(numIn, numOut) so the plugin can write
        // its stereo output. Pre-fill the extra channel with the mono mix
        // — JUCE's contract is that the plugin only reads numIn channels,
        // but copying mono there is harmless and avoids leaving uninit
        // memory in the buffer (which we otherwise read back as outR).
        const int procCh = juce::jmin (juce::jmax (numIn, numOut),
                                         stereoScratch.getNumChannels());
        for (int c = 1; c < procCh; ++c)
            stereoScratch.copyFrom (c, 0, mono, numSamples);

        float* procPtrs[2] = { stereoScratch.getWritePointer (0),
                               procCh > 1 ? stereoScratch.getWritePointer (1) : nullptr };
        juce::AudioBuffer<float> buf (procPtrs, procCh, numSamples);
        p->processBlock (buf, midiMessages);

        const float* outL = stereoScratch.getReadPointer (0);
        const float* outR = (numOut >= 2 && procCh >= 2)
                              ? stereoScratch.getReadPointer (1) : outL;
        std::memcpy (L, outL, sizeof (float) * (size_t) numSamples);
        std::memcpy (R, outR, sizeof (float) * (size_t) numSamples);
    }
    else if (numIn == 0 && numOut >= 1)
    {
        // Instrument plugin (synth / sampler): zero audio inputs, audio
        // output generated from the MIDI buffer. The buffer width must be
        // at least numOut so the plugin can write its output channels;
        // we clear it first because some plugins add to the existing
        // contents rather than overwriting (a fresh sampler voice on top
        // of garbage in the scratch would leak the previous block's
        // contents). Caller (ChannelStrip MIDI path) already cleared L/R
        // before calling; we still clear stereoScratch because it's
        // separate storage that may hold the previous block's plugin output.
        if (numSamples > stereoScratch.getNumSamples()) return;

        const int procCh = juce::jmin (numOut, stereoScratch.getNumChannels());
        for (int c = 0; c < procCh; ++c)
            stereoScratch.clear (c, 0, numSamples);

        float* procPtrs[2] = { stereoScratch.getWritePointer (0),
                               procCh > 1 ? stereoScratch.getWritePointer (1) : nullptr };
        juce::AudioBuffer<float> buf (procPtrs, procCh, numSamples);
        p->processBlock (buf, midiMessages);

        const float* outL = stereoScratch.getReadPointer (0);
        const float* outR = (numOut >= 2 && procCh >= 2)
                              ? stereoScratch.getReadPointer (1) : outL;
        std::memcpy (L, outL, sizeof (float) * (size_t) numSamples);
        std::memcpy (R, outR, sizeof (float) * (size_t) numSamples);
    }
    else
    {
        // Plugin layout we can't handle (zero outputs, etc.) - bail.
        return;
    }

    if (bufferMs > 0.0 && blocksSinceLoad >= kGraceBlocks)
    {
        const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - t0)
                                  * secondsPerTick * 1000.0;
        if (elapsedMs > bufferMs * kBudgetFraction)
        {
            if (++consecutiveOverruns >= kMaxConsecutiveOverruns)
                engageAutoBypass();
        }
        else
        {
            consecutiveOverruns = 0;
        }
    }
    if (blocksSinceLoad < kGraceBlocks) ++blocksSinceLoad;
}

void PluginSlot::setParamNormalised (int paramIndex, float value01) noexcept
{
    // Audio-thread entry. Read the current instance via acquire so we
    // don't race the load/unload swap. setValue (not
    // setValueNotifyingHost) avoids the host-notify path's listener
    // calls from the audio thread - the plugin will pick up the new
    // value on its next processBlock either way.
    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;
    const auto& params = p->getParameters();
    if (paramIndex < 0 || paramIndex >= params.size()) return;
    if (auto* param = params[paramIndex])
        param->setValue (juce::jlimit (0.0f, 1.0f, value01));
}
} // namespace duskstudio
