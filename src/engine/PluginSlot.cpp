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

PluginSlot::PluginSlot()
{
    // Drain the param-write SPSC FIFO at 30 Hz on the message thread.
    // The rate is the same the meter UI ticks at — fine-grained enough
    // that an automated controller stream feels live, cheap enough that
    // an idle slot pays one atomic-load + branch per tick. Started here
    // (not lazily) so the queue is live from the moment the audio
    // callback could push into it; the engine constructs every
    // PluginSlot on the message thread with MessageManager already up.
    startTimerHz (30);
}

PluginSlot::~PluginSlot()
{
    // Stop our own timer FIRST. juce::Timer's dtor also stopTimer's, but
    // doing it explicitly here closes the window where a tick could fire
    // mid-destruction and observe a half-torn currentInstance / ownedRemote.
    stopTimer();

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Release the Mac shell instance before tearing down the OOP child.
    // releaseShellInstance refuses if a wrapper is still outstanding;
    // we issue a stderr line + skip the release in that case rather than
    // destroying the AudioProcessor under a live editor (would dangle
    // the editor's processor reference and crash on the next event).
    // The wrapper itself is owned upstream (by ChannelStripComponent's
    // pluginEditorModal); its dtor runs when that owner releases it.
    releaseShellInstance();
   #endif

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
    for (auto& slot : previousInstances)
        if (slot != nullptr) slot->releaseResources();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    // ~RemotePluginConnection sends SIGTERM/SIGKILL to the child and
    // unmaps SHM. Safe at process shutdown - audio thread is detached.
    ownedRemote.reset();
    for (auto& slot : previousRemotes) slot.reset();
   #endif
}

void PluginSlot::leakInstanceForShutdown()
{
    currentInstance.store (nullptr, std::memory_order_release);
    if (ownedInstance != nullptr)
        (void) ownedInstance.release();
    for (auto& slot : previousInstances)
        if (slot != nullptr) (void) slot.release();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // OOP plugins live in a separate process — the in-process leak hack
    // is irrelevant. Cleanly disconnecting kills the child, which
    // releases its plugin in its own address space (so any plugin-side
    // dtor crash is confined to the child and ignored).
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    ownedRemote.reset();
    for (auto& slot : previousRemotes) slot.reset();
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
        previousRemotes[1].reset();
        previousRemotes[1] = std::move (previousRemotes[0]);
        previousRemotes[0] = std::move (ownedRemote);
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

    // Park the audio thread first. Detach the current instance, then
    // rotate the prior plugin through the two-deep keep-alive ring so
    // its destructor is deferred by TWO swaps (see previousInstances
    // doc comment). Two slots, not one, because the audio thread can
    // hold a pointer from the latest swap for a full block-worth of
    // time; a second rapid Replace within that window would destroy
    // the instance under it.
    //
    // Mirror unload(): detach the existing lastTouchedListener from
    // ownedInstance BEFORE the rotation. Without this, lastTouchedListener
    // is reassigned via make_unique below (destroying the old listener
    // object) while the just-deposed instance still has the old listener
    // pointer registered on its param listener lists. A param callback
    // from the deposed instance's editor during the swap (e.g. fired
    // by the editor's teardown that follows a Replace action) would
    // then dereference freed memory.
    if (ownedInstance != nullptr && lastTouchedListener != nullptr)
        for (auto* p : ownedInstance->getParameters())
            if (p != nullptr) p->removeListener (lastTouchedListener.get());
    lastTouchedListener.reset();
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);

    currentInstance.store (nullptr, std::memory_order_release);
    if (previousInstances[1] != nullptr)
        previousInstances[1]->releaseResources();
    previousInstances[1] = std::move (previousInstances[0]);
    previousInstances[0] = std::move (ownedInstance);
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

    // Same swap-load shape as loadFromFile; rotates through the
    // two-deep keep-alive ring so the deposed instance survives TWO
    // swaps before destruction. See previousInstances doc comment.
    //
    // Detach the existing lastTouchedListener from ownedInstance
    // BEFORE rotation (mirrors unload()). The listener gets recreated
    // via make_unique further down; if we left the old listener
    // pointer in the deposed instance's param listener lists, a param
    // callback from that instance (typical during Replace-action
    // editor teardown) would hit freed memory.
    if (ownedInstance != nullptr && lastTouchedListener != nullptr)
        for (auto* p : ownedInstance->getParameters())
            if (p != nullptr) p->removeListener (lastTouchedListener.get());
    lastTouchedListener.reset();
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);

    currentInstance.store (nullptr, std::memory_order_release);
    if (previousInstances[1] != nullptr)
        previousInstances[1]->releaseResources();
    previousInstances[1] = std::move (previousInstances[0]);
    previousInstances[0] = std::move (ownedInstance);
    ownedInstance.reset();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Tear down any prior OOP slot before deciding which path the new
    // load takes. Mode is per-load: we may end up in-process this time
    // even if the previous load was OOP (and vice versa).
    //
    // Two-deep deferred-destruction via previousRemotes ring (see
    // declaration). Slot [0] holds the just-deposed connection; slot
    // [1] holds the one-before-that. A third rapid swap evicts [1] and
    // destroys it, by which point any audio block that captured the
    // ring's contents has had two block-worths of time to drain.
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    previousRemotes[1].reset();
    previousRemotes[1] = std::move (previousRemotes[0]);
    previousRemotes[0] = std::move (ownedRemote);
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
   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Track-unload is an explicit user action and the modal-editor close
    // protocol (closePluginEditor → modal close → wrapper dtor) is
    // assumed to have run by now. releaseShellInstance refuses + logs
    // if the wrapper is still alive; the shell instance then sticks
    // around until the next unload / dtor when the wrapper has died.
    releaseShellInstance();
   #endif

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
    if (previousInstances[1] != nullptr)
        previousInstances[1]->releaseResources();
    previousInstances[1] = std::move (previousInstances[0]);
    previousInstances[0] = std::move (ownedInstance);
    ownedInstance.reset();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Two-deep deferred-destruction via previousRemotes ring; see
    // loadFromDescription for the rationale.
    reaperTimer.stopTimer();
    currentRemote.store (nullptr, std::memory_order_release);
    previousRemotes[1].reset();
    previousRemotes[1] = std::move (previousRemotes[0]);
    previousRemotes[0] = std::move (ownedRemote);
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
    // same two-deep deferred-destruction-via-previousInstances ring and
    // the same detach-listener-before-rotate discipline (see
    // loadFromFile for the rationale).
    if (ownedInstance != nullptr && lastTouchedListener != nullptr)
        for (auto* p : ownedInstance->getParameters())
            if (p != nullptr) p->removeListener (lastTouchedListener.get());
    lastTouchedListener.reset();
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);

    currentInstance.store (nullptr, std::memory_order_release);
    if (previousInstances[1] != nullptr)
        previousInstances[1]->releaseResources();
    previousInstances[1] = std::move (previousInstances[0]);
    previousInstances[0] = std::move (ownedInstance);
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
    // Re-install the last-touched listener on the restored instance so
    // MIDI Learn's "last-touched parameter" works after session reload.
    // Pre-existing miss: the OOP path inherits its own listener
    // machinery on the child side, but the in-process fallback dropped
    // it on the floor.
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);
    lastTouchedListener = std::make_unique<LastTouchedListener> (lastTouchedParamIndex);
    for (auto* p : ownedInstance->getParameters())
        if (p != nullptr) p->addListener (lastTouchedListener.get());
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
                                       juce::jmax (rNumOut, 0),
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
                                       juce::jmax (rNumOut, 0),
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
    // Audio-thread entry. Lock-free SPSC push into paramFifo; the actual
    // JUCE setValueNotifyingHost call runs on the message thread inside
    // timerCallback().
    //
    // Why we DON'T call param->setValue here, even though the comment
    // history said it was "safe enough": JUCE param setters can fire
    // synchronous listener callbacks (parameter UI components, host
    // automation listeners, the plugin's own internal listeners) and may
    // acquire internal locks inside the plugin's parameter management
    // code. Real plugins observed in the wild (Diva, Massive X, Spitfire
    // BBC SO) take std::mutex / WaitableEvent inside their parameter-
    // change paths. Calling setValue from the audio thread therefore
    // violates the project-wide "no locks on the audio thread" rule
    // (CLAUDE.md §"Audio thread rules") even when the underlying
    // assignment looks atomic.
    //
    // Negative paramIndex = silent no-op; bounds against params.size()
    // also runs on the message-thread drain to guard against a hot-swap
    // shrinking the parameter list between push and apply.
    if (paramIndex < 0) return;

    int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
    paramFifo.prepareToWrite (1, s1, sz1, s2, sz2);
    if (sz1 + sz2 == 0)
    {
        // FIFO full. Drop the write — protecting the audio thread from
        // blocking is more important than a single missed param update,
        // and the 30 Hz drain catches up within one tick once the queue
        // pressure subsides. With 256 entries this branch is only
        // reachable if the audio thread is producing > 7680 writes/s
        // sustained (256 × 30 Hz), which no realistic MIDI controller
        // can do.
        return;
    }

    const int slot = (sz1 > 0) ? s1 : s2;
    paramQueue[(size_t) slot] = ParamWrite { paramIndex,
                                              juce::jlimit (0.0f, 1.0f, value01) };
    paramFifo.finishedWrite (1);
}

void PluginSlot::timerCallback()
{
    // Message thread. Drain every pending ParamWrite into the live
    // plugin instance. Bounded loop — caller (juce::Timer) marshals us
    // here so JUCE_ASSERT_MESSAGE_THREAD in applyParamWriteOnMessageThread
    // is satisfied by construction.
    const int avail = paramFifo.getNumReady();
    if (avail > 0)
    {
        int s1 = 0, sz1 = 0, s2 = 0, sz2 = 0;
        paramFifo.prepareToRead (avail, s1, sz1, s2, sz2);
        for (int i = 0; i < sz1; ++i)
            applyParamWriteOnMessageThread (paramQueue[(size_t) (s1 + i)]);
        for (int i = 0; i < sz2; ++i)
            applyParamWriteOnMessageThread (paramQueue[(size_t) (s2 + i)]);
        paramFifo.finishedRead (sz1 + sz2);
    }

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // 3c-4: detect OOP child crash and tear down the parameter mirror.
    // Without this, every subsequent ShellParamListener fire would
    // call ownedRemote->setRemoteParam which silently returns false
    // (peer closed). The user would see the shell editor responding
    // but no DSP would update — confusing. Better: detach the
    // listener + clear the sink so the editor functions locally
    // without pretending to drive a dead DSP.
    //
    // The shell instance + editor + wrapper stay alive. The audio
    // path's existing reaper (pollRemoteReaper) already auto-bypasses
    // the slot so the transport keeps rolling. The shell editor goes
    // "dark" in the sense that knob moves are no-ops, matching
    // user expectation for a degraded slot.
    //
    // Idempotent: if shellParamListener is already null (post-teardown
    // or never installed), the early-return below skips repeated work.
    if (shellInstanceForEditor != nullptr
        && shellParamListener != nullptr
        && ownedRemote != nullptr
        && ownedRemote->isCrashed())
    {
        for (auto* param : shellInstanceForEditor->getParameters())
            if (param != nullptr) param->removeListener (shellParamListener.get());
        shellParamListener.reset();
        ownedRemote->setParamChangedSink ({});
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP child crashed; mirror "
                      "detached, shell editor degraded to local-only. "
                      "Reload the plugin to recover.\n");
    }
   #endif
}

void PluginSlot::applyParamWriteOnMessageThread (const ParamWrite& pw) noexcept
{
    // This is the ONE place where a JUCE parameter setter is invoked.
    // The assertion fires in any debug build (and therefore under
    // DUSKSTUDIO_RUN_SELFTEST=1, which the test runner builds as Debug)
    // if a future change ever routes the call from the audio thread —
    // exactly the regression Phase 2 is meant to prevent.
    JUCE_ASSERT_MESSAGE_THREAD;

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;

    const auto& params = p->getParameters();
    if (pw.paramIndex < 0 || pw.paramIndex >= params.size()) return;
    if (auto* param = params[pw.paramIndex])
        param->setValueNotifyingHost (pw.value);
}

#if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
bool PluginSlot::ensureShellInstanceForEditor (juce::String& err)
{
    JUCE_ASSERT_MESSAGE_THREAD;

    if (shellInstanceForEditor != nullptr)
    {
        err.clear();
        return true;  // idempotent — already loaded for this slot
    }

    if (manager == nullptr)
    {
        err = "PluginSlot has no PluginManager bound";
        return false;
    }
    if (savedDescriptionXml.isEmpty())
    {
        err = "No plugin loaded — nothing to host in the shell";
        return false;
    }

    // Parse the saved description (populated by loadFromDescription's
    // OOP path) and load a fresh in-process instance against the same
    // file. The instance is editor-only: prepareToPlay is called so
    // the editor's bounds + initial parameter snapshot match the
    // engine's current SR/BS, but processBlock is NEVER invoked on
    // this instance — DSP runs in the OOP child.
    auto xml = juce::XmlDocument::parse (savedDescriptionXml);
    if (xml == nullptr)
    {
        err = "Saved plugin description is not valid XML";
        return false;
    }
    juce::PluginDescription desc;
    if (! desc.loadFromXml (*xml))
    {
        err = "Saved plugin description failed to deserialise";
        return false;
    }

    auto fresh = manager->createPluginInstance (desc,
                                                  preparedSampleRate > 0.0
                                                       ? preparedSampleRate : 48000.0,
                                                  preparedBlockSize  > 0
                                                       ? preparedBlockSize  : 512,
                                                  err);
    if (fresh == nullptr)
        return false;

    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                   fresh->getTotalNumOutputChannels(),
                                   preparedSampleRate > 0.0 ? preparedSampleRate : 48000.0,
                                   preparedBlockSize  > 0 ? preparedBlockSize  : 512);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);

    shellInstanceForEditor = std::move (fresh);

    // 3c-3b: install the shell-side parameter listener + register the
    // ParamChangedFromChild sink on ownedRemote. Both sides of the
    // mirror are wired before the editor opens — the next user knob
    // move on the shell sends SetParam to the child, and any child-
    // initiated change (host automation, preset reload) arrives back
    // via the sink. applyingFromMirror breaks the listener loop.
    shellParamListener = std::make_unique<ShellParamListener> (*this);
    for (auto* param : shellInstanceForEditor->getParameters())
        if (param != nullptr) param->addListener (shellParamListener.get());

    if (ownedRemote != nullptr)
    {
        ownedRemote->setParamChangedSink (
            [this] (int paramIndex, float value01, std::uint32_t /*seq*/)
            {
                // Already on the message thread — RemotePluginConnection's
                // reader marshalled here via juce::MessageManager::callAsync.
                applyShellParamFromChild (paramIndex, value01);
            });
    }

    err.clear();
    return true;
}

void PluginSlot::ShellParamListener::parameterValueChanged (int paramIndex, float newValue)
{
    // Loop breaker: if we're inside applyShellParamFromChild /
    // syncShellStateFromChild, skip the outbound echo. Acquire pairs
    // with the release store the mirror-apply path does.
    if (slot.applyingFromMirror.load (std::memory_order_acquire)) return;
    if (slot.ownedRemote == nullptr) return;
    // 3c-4 fast-path crash check. setRemoteParam would return false
    // anyway (peer closed → write fails) but isCrashed() short-circuits
    // before touching controlMutex, so a knob drag against a dead
    // child doesn't pay the lock-acquire cost per listener fire. The
    // 30 Hz timerCallback will detach this listener on the next tick;
    // until then any in-flight callbacks bail here.
    if (slot.ownedRemote->isCrashed()) return;
    // Fire-and-forget over the existing 3c-3a control channel. Returns
    // false only on socket peer-close (child died); in that case the
    // OOP slot has already been auto-bypassed by the reaper, so the
    // dropped write is harmless.
    (void) slot.ownedRemote->setRemoteParam (paramIndex, newValue);
}

void PluginSlot::applyShellParamFromChild (int paramIndex, float value01) noexcept
{
    JUCE_ASSERT_MESSAGE_THREAD;
    if (shellInstanceForEditor == nullptr) return;
    const auto& params = shellInstanceForEditor->getParameters();
    if (paramIndex < 0 || paramIndex >= params.size()) return;
    auto* param = params[paramIndex];
    if (param == nullptr) return;

    applyingFromMirror.store (true, std::memory_order_release);
    param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value01));
    applyingFromMirror.store (false, std::memory_order_release);
}

bool PluginSlot::syncShellStateFromChild (juce::String& err)
{
    JUCE_ASSERT_MESSAGE_THREAD;
    err.clear();

    if (shellInstanceForEditor == nullptr)
    {
        err = "shell instance not loaded";
        return false;
    }
    if (ownedRemote == nullptr)
    {
        // No OOP connection (e.g. in-process fallback). Nothing to
        // sync; shell state is authoritative.
        return true;
    }

    std::vector<std::uint8_t> blob;
    std::string serr;
    if (! ownedRemote->getState (blob, serr))
    {
        err = juce::String ("child getState failed: ") + serr;
        return false;
    }
    if (blob.empty())
    {
        // Child has no state to share (just-loaded plugin at defaults).
        // Shell already at defaults too. Nothing to do.
        return true;
    }

    // Apply with the loop breaker engaged. setStateInformation may fire
    // dozens of parameter listeners synchronously; echoing every one
    // back to the child would saturate the control socket + briefly
    // pin the child's setValueNotifyingHost on the JUCE message thread.
    applyingFromMirror.store (true, std::memory_order_release);
    shellInstanceForEditor->setStateInformation (blob.data(), (int) blob.size());
    applyingFromMirror.store (false, std::memory_order_release);
    return true;
}

juce::AudioProcessorEditor* PluginSlot::createShellEditor()
{
    JUCE_ASSERT_MESSAGE_THREAD;

    if (shellInstanceForEditor == nullptr) return nullptr;

    // Refuse a second concurrent editor: JUCE's createEditorIfNeeded
    // would hand the same pointer to two unique_ptr-owning wrappers,
    // resulting in a double-free when the second wrapper destructs.
    if (outstandingShellWrapper.getComponent() != nullptr) return nullptr;

    return shellInstanceForEditor->createEditorIfNeeded();
}

void PluginSlot::notifyShellEditorWrapper (juce::Component* wrapper) noexcept
{
    JUCE_ASSERT_MESSAGE_THREAD;
    outstandingShellWrapper = wrapper;  // SafePointer auto-nulls on wrapper dtor
}

void PluginSlot::releaseShellInstance()
{
    JUCE_ASSERT_MESSAGE_THREAD;

    if (shellInstanceForEditor == nullptr) return;

    if (outstandingShellWrapper.getComponent() != nullptr)
    {
        // Wrapper is still alive — its inner unique_ptr<editor> holds a
        // reference to this AudioProcessor. Destroying the processor here
        // would dangle the editor's processor pointer and crash on the
        // next AppKit event. Skip + log; the next unload / dtor will
        // re-try once the wrapper has died.
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] releaseShellInstance: editor "
                      "wrapper still outstanding; deferring shell teardown.\n");
        return;
    }

    // Detach the parameter listener + clear the ParamChangedFromChild
    // sink BEFORE destroying the AudioProcessor. JUCE's listener list
    // lives inside each parameter; destroying the processor while a
    // listener is registered dangles the parameter's listener-list
    // entry. Clearing the sink stops any in-flight callAsync lambda
    // from invoking the listener on a freed processor — the lambda
    // re-reads paramChangedSink_ under controlMutex at dispatch time,
    // so this clear races safely with a queued push.
    if (shellParamListener != nullptr)
    {
        for (auto* param : shellInstanceForEditor->getParameters())
            if (param != nullptr) param->removeListener (shellParamListener.get());
        shellParamListener.reset();
    }
    if (ownedRemote != nullptr)
        ownedRemote->setParamChangedSink ({});

    shellInstanceForEditor->releaseResources();
    shellInstanceForEditor.reset();
}
#endif
} // namespace duskstudio
