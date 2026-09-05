#include "PluginSlot.h"
#include "AtomicPark.h"
#include "PluginManager.h"
#include <algorithm>
#include <cmath>
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
// through session persistence fails to load
// with "No compatible plug-in format exists for this plug-in", AND a
// freshly-picked-from-cache description has the same problem because the
// scanned descriptions in KnownPluginList carry the same inner-.so path.
//
// Walk parents until we find a `.vst3` ancestor and rewrite the path.
// No-op when the path is already a `.vst3` (macOS / non-Linux / future
// JUCE that fixes this).
void normalizeVst3Location (PluginDescriptor& descriptor)
{
    if (descriptor.formatName != "VST3") return;
    if (juce::File (descriptor.location).hasFileExtension (".vst3")) return;

    for (auto walk = juce::File (descriptor.location).getParentDirectory();
         walk.exists() && walk.getFullPathName().isNotEmpty();
         walk = walk.getParentDirectory())
    {
        if (walk.hasFileExtension (".vst3"))
        {
            descriptor.location = walk.getFullPathName().toStdString();
            return;
        }
        if (walk.getParentDirectory() == walk) break;  // hit fs root
    }
}

PluginDescriptor refreshFromInstance (PluginManager& manager,
                                      juce::AudioPluginInstance& instance,
                                      const PluginDescriptor& reloadDescriptor)
{
    return mergeLoadedPluginDescriptor (
        reloadDescriptor, manager.descriptorForInstance (instance));
}

// One-shot diagnostic so the user (and we) can see exactly what JUCE is
// reporting after load + bus-layout pass. Helps debug "plugin loaded but
// silent" cases like an instrument that ends up looking like an effect
// because of an auto-enabled sidechain bus.
void logLoadedPlugin (const juce::AudioPluginInstance& instance,
                      const PluginDescriptor& descriptor)
{
    std::fprintf (stderr,
                  "[Dusk Studio/PluginSlot] Loaded \"%s\" (instrument=%d) - "
                  "totalIn=%d totalOut=%d busesIn=%d busesOut=%d latency=%d\n",
                  descriptor.name.c_str(),
                  (int) descriptor.isInstrument,
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
    // The rate is the same the meter UI ticks at - fine-grained enough
    // that an automated controller stream feels live, cheap enough that
    // an idle slot pays one atomic-load + branch per tick. Started here
    // (not lazily) so the queue is live from the moment the audio
    // callback could push into it; the engine constructs every
    // PluginSlot on the message thread with MessageManager already up.
    startTimerHz (30);
}

PluginSlot::~PluginSlot()
{
    // Stop our own timer FIRST. dusk::Timer's dtor also stopTimer's, but
    // doing it explicitly here closes the window where a tick could fire
    // mid-destruction and observe a half-torn currentInstance / ownedRemote.
    stopTimer();

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Release the Mac shell instance before tearing down the OOP child.
    // releaseShellInstance refuses if a wrapper is still outstanding;
    // when that happens we cannot let the shellInstanceForEditor unique
    // _ptr's member dtor run at the end of ~PluginSlot - that would
    // destroy the AudioProcessor while the wrapper's editor still
    // holds a non-owning reference to it, dangling the editor's
    // processor pointer on the next AppKit event.
    //
    // Detect refusal by checking whether shellInstanceForEditor is
    // still non-null after the call. In that case detach the listener
    // (so the dangling processor isn't dereferenced from a fire-on-
    // shutdown listener callback) and intentionally LEAK the
    // AudioProcessor via .release(). The wrapper continues to hold a
    // reference to a leaked-but-still-mapped processor; the OS reclaims
    // its memory at process exit. Acceptable in the dtor path - the
    // alternative is a UAF.
    releaseShellInstance();
    if (shellInstanceForEditor != nullptr)
    {
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] ~PluginSlot: shell wrapper still "
                      "outstanding; detaching listener + leaking shell "
                      "AudioProcessor to avoid dangling its editor's processor "
                      "pointer.\n");
        if (shellParamListener != nullptr)
        {
            for (auto* param : shellInstanceForEditor->getParameters())
                if (param != nullptr) param->removeListener (shellParamListener.get());
            shellParamListener.reset();
        }
        // Critical: the sink lambda captures `this` (PluginSlot). After
        // this dtor returns, that lambda would UAF on the dead slot.
        // Clear the sink so any pending callAsync queued by the
        // RemotePluginConnection reader dispatches with an empty local
        // sink and no-ops. The shared SinkState keeps the cleared
        // function alive for any in-flight lambda; PluginSlot itself
        // is safely no longer referenced.
        if (ownedRemote != nullptr)
            ownedRemote->setParamChangedSink ({});
        (void) shellInstanceForEditor.release();
    }
   #endif

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // An in-flight off-thread load still posts its completion; joining before
    // the members below go away keeps those workers from outliving the slot.
    cancelAndJoinRemoteLoads();
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
    // OOP plugins live in a separate process - the in-process leak hack
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
    // Without this the only reaper is the next load, so a finished worker sits
    // in the vector until then and the destructor always has one to join.
    reapFinishedRemoteLoads();

    auto* r = ownedRemote.get();
    if (r != nullptr && r->pollReaper())
    {
        // Child has exited. Park the audio path immediately (defense in
        // depth - the audio thread will set autoBypassed itself on the
        // next processBlockSync, but proactively bypassing closes the
        // window where transport-stopped slots silently hold a dead
        // connection).
        autoBypassed .store (true, std::memory_order_relaxed);
        remoteCrashed.store (true, std::memory_order_relaxed);
        currentRemote.store (nullptr, std::memory_order_release);
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP child process exited; slot "
                      "auto-bypassed. Reload the plugin to recover.\n");
    }

    // Stop once there is no live child left to watch and no load still running.
    const bool watchingChild = r != nullptr
                                 && ! remoteCrashed.load (std::memory_order_relaxed);
    if (! watchingChild && remoteLoads.empty())
        reaperTimer.stopTimer();
}

void PluginSlot::publishRemoteConnection (
    std::unique_ptr<duskstudio::ipc::RemotePluginConnection> connection,
    PluginDescriptor descriptor,
    int numIn, int numOut, int latency, bool isInstrument)
{
    ownedRemote = std::move (connection);
    {
        // Same drain-then-reset-and-publish as loadFromFile.
        const juce::SpinLock::ScopedLockType processGuard (processLock);
        remoteNumIn .store (numIn,  std::memory_order_relaxed);
        remoteNumOut.store (numOut, std::memory_order_relaxed);
        remoteIsInstrument.store (isInstrument, std::memory_order_relaxed);
        cachedLatencySamples.store (latency, std::memory_order_relaxed);
        blocksSinceLoad     = 0;
        consecutiveOverruns = 0;
        autoBypassed .store (false, std::memory_order_relaxed);
        remoteCrashed.store (false, std::memory_order_relaxed);
        currentRemote.store (ownedRemote.get(), std::memory_order_release);
    }
    reaperTimer.startTimer (kReaperPeriodMs);
    descriptor.isInstrument = isInstrument;
    descriptor.numInputChannels = numIn;
    descriptor.numOutputChannels = numOut;
    loadedDescriptor = std::move (descriptor);
    offlineDescriptor.reset();
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64.clear();
    offlineCapturePlaceholder = false;
    lastKnownStateBase64.clear();
}

void PluginSlot::reapFinishedRemoteLoads() noexcept
{
    for (auto entry = remoteLoads.begin(); entry != remoteLoads.end();)
    {
        if (entry->finished->load (std::memory_order_acquire))
        {
            if (entry->worker.joinable()) entry->worker.join();
            entry = remoteLoads.erase (entry);
        }
        else
        {
            ++entry;
        }
    }
}

void PluginSlot::cancelAndJoinRemoteLoads() noexcept
{
    // Every flag goes up before the first join so the workers unwind in
    // parallel; joining one at a time after its own cancel would serialise the
    // child teardowns.
    for (auto& entry : remoteLoads)
        entry.cancel->store (true, std::memory_order_release);
    for (auto& entry : remoteLoads)
        if (entry.worker.joinable()) entry.worker.join();
    remoteLoads.clear();
}

void PluginSlot::beginRemoteLoad (PluginDescriptor descriptor,
                                  std::uint32_t epoch,
                                  std::string hostPath,
                                  LoadCompletion onDone)
{
    reapFinishedRemoteLoads();

    struct Outcome
    {
        std::unique_ptr<duskstudio::ipc::RemotePluginConnection> connection;
        int  numIn { 0 };
        int  numOut { 0 };
        int  latency { 0 };
        bool isInstrument { false };
        std::string error;
    };

    // Everything the worker touches is copied across: it must not read the slot,
    // whose completion is the only part that runs back on the message thread.
    const auto descriptionXml = manager->descriptorToLegacyXml (descriptor).toStdString();
    const auto modeArg = manager->getHostModeArg();
    const auto sampleRate = preparedSampleRate;
    const auto blockSize  = preparedBlockSize;
    std::weak_ptr<char> life = lifeToken;
    auto finished = std::make_shared<std::atomic<bool>> (false);
    auto cancel   = std::make_shared<std::atomic<bool>> (false);

    std::thread worker (
        [this, life, epoch, onDone, descriptor, hostPath, modeArg, descriptionXml,
         sampleRate, blockSize, finished, cancel]
    {
        auto outcome = std::make_shared<Outcome>();
        auto remote = std::make_unique<duskstudio::ipc::RemotePluginConnection>();
        remote->setCancelFlag (cancel);

        if (! remote->connect (hostPath, modeArg, outcome->error))
            remote.reset();
        else if (! remote->loadPlugin (descriptionXml, sampleRate, blockSize,
                                        outcome->numIn, outcome->numOut,
                                        outcome->latency, outcome->isInstrument,
                                        outcome->error))
            remote.reset();
        else
            outcome->connection = std::move (remote);

        if (cancel->load (std::memory_order_acquire))
        {
            // The slot is going away. Drop the child here rather than posting a
            // completion the slot could not receive anyway - the destructor is
            // waiting on this thread, so the teardown belongs on it.
            outcome->connection.reset();
            finished->store (true, std::memory_order_release);
            return;
        }
        // Cancellation covers the load only. Past this point the slot owns the
        // connection and disconnect() is what tears it down.
        if (outcome->connection != nullptr)
            outcome->connection->setCancelFlag (nullptr);

        // A queue that has gone away (shutdown) drops the lambda, and the
        // connection dies with it - killing the child, which is what we want.
        (void) dusk::callAsync ([this, life, epoch, onDone, descriptor, outcome,
                                 sampleRate, blockSize]
        {
            if (! life.lock()) return;
            if (currentLoadEpoch.load (std::memory_order_relaxed) != epoch)
            {
                if (onDone) onDone (false, "load superseded");
                return;
            }
            if (outcome->connection == nullptr)
            {
                std::fprintf (stderr,
                              "[Dusk Studio/PluginSlot] OOP load failed (%s); "
                              "falling back to in-process.\n",
                              outcome->error.c_str());
                beginInProcessLoad (descriptor, epoch, onDone);
                return;
            }
            // A device change while the load was in flight leaves the child on
            // the configuration the load captured. prepareToPlay only reaches a
            // published connection, and this one is not published yet, so the
            // re-prepare has to happen here or the child would process its first
            // block at the wrong rate and block size.
            const bool configurationMoved =
                preparedBlockSize != blockSize
                || std::abs (preparedSampleRate - sampleRate) > 0.0;
            if (configurationMoved)
            {
                std::string prepareError;
                if (preparedBlockSize > duskstudio::ipc::kMaxBlock
                    || ! outcome->connection->prepareToPlay (preparedSampleRate,
                                                              preparedBlockSize,
                                                              prepareError))
                {
                    std::fprintf (stderr,
                                  "[Dusk Studio/PluginSlot] OOP re-prepare after a device "
                                  "change failed (%s); falling back to in-process.\n",
                                  prepareError.c_str());
                    beginInProcessLoad (descriptor, epoch, onDone);
                    return;
                }
            }

            publishRemoteConnection (std::move (outcome->connection), descriptor,
                                      outcome->numIn, outcome->numOut,
                                      outcome->latency, outcome->isInstrument);
            if (onDone) onDone (true, {});
        });

        finished->store (true, std::memory_order_release);
    });

    remoteLoads.push_back ({ std::move (worker), std::move (finished),
                             std::move (cancel) });
    // The reaper tick is the only thing that joins a finished worker while the
    // slot sits idle between loads.
    reaperTimer.startTimer (kReaperPeriodMs);
}

void PluginSlot::retireRemoteConnection()
{
    reaperTimer.stopTimer();

    std::unique_ptr<duskstudio::ipc::RemotePluginConnection> retired;
    {
        const juce::SpinLock::ScopedLockType processGuard (processLock);
        currentRemote.store (nullptr, std::memory_order_release);
        retired = std::move (previousRemotes[1]);
        previousRemotes[1] = std::move (previousRemotes[0]);
        previousRemotes[0] = std::move (ownedRemote);
    }
    retired.reset();
}
#endif

void PluginSlot::clearAutoBypass() noexcept
{
    autoBypassed.store (false, std::memory_order_relaxed);
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Clearing crashed state too: if the user explicitly asks to
    // re-enable a crashed slot, drop the dead connection so the next
    // load (or processBlock attempt) doesn't see a stale carcass.
    if (remoteCrashed.load (std::memory_order_relaxed))
    {
        retireRemoteConnection();
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
    // A crashed or wedged child gets a short deadline: that close is driven by
    // the 30 Hz strip timer rather than by the user, and a child that already
    // missed a 100 ms block or reported its message thread stalled is not about
    // to answer inside five seconds either. Replies are correlated by request
    // ID, so one that lands after the short deadline is dropped rather than
    // desyncing the reader.
    constexpr int kHideEditorTimeoutMs             = 5000;
    constexpr int kUnresponsiveHideEditorTimeoutMs = 300;
    const bool unresponsive = remoteCrashed.load (std::memory_order_relaxed)
                                || r->isCrashed()
                                || r->isMessageThreadWedged();
    std::string err;
    if (! r->hideEditor (err, unresponsive ? kUnresponsiveHideEditorTimeoutMs
                                            : kHideEditorTimeoutMs))
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
    (void) width;
    (void) height;
    return false;
   #endif
}

void PluginSlot::prepareToPlay (double sampleRate, int blockSize)
{
    // Excludes a concurrent processMono/StereoBlock for THIS slot (they
    // try-lock and pass dry). Defense in depth under the engine's process
    // gate: holds even on a reconfigure path that reaches us with a live
    // callback still running somewhere.
    const juce::SpinLock::ScopedLockType processGuard (processLock);

    preparedSampleRate = sampleRate;
    preparedBlockSize  = std::max (1, blockSize);
    secondsPerTick     = 1.0 / (double) juce::Time::getHighResolutionTicksPerSecond();

    // Pre-size the stereo scratch so the audio thread never allocates when
    // a stereo-only plugin is in the slot.
    stereoScratch.setSize (2, preparedBlockSize, false, false, true);

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // The wire format sets this one, not the shared ceiling - but it must not
    // fall under it, or the OOP path alone would start emptying blocks the rest
    // of the MIDI path delivers.
    static_assert (duskstudio::ipc::kMidiBytes >= dusk::kMidiBlockBytes,
                   "IPC MIDI cap must not sit below the shared MIDI ceiling");
    oopMidiScratch.reserveBytes (duskstudio::ipc::kMidiBytes);
   #endif

    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;

    if (ownedInstance != nullptr)
    {
        // Release BEFORE re-preparing. setPlayConfigDetails updates the
        // processor's cached block size, and a JUCE VST3 wrapper's prepareToPlay
        // skips re-running setupProcessing when it sees (isActive && same SR &&
        // same blockSize) - so a block-size change could leave the plugin's
        // INTERNAL buffers at the old size while it's fed the new one (overrun /
        // crash in a multi-bus instrument). releaseResources() clears isActive
        // so the re-prepare always takes. This is the conventional host cycle.
        ownedInstance->releaseResources();
        ownedInstance->setPlayConfigDetails (
            ownedInstance->getTotalNumInputChannels(),
            ownedInstance->getTotalNumOutputChannels(),
            sampleRate, preparedBlockSize);
        ownedInstance->prepareToPlay (sampleRate, preparedBlockSize);
        cachedLatencySamples.store (ownedInstance->getLatencySamples(),
                                      std::memory_order_relaxed);
        // Re-publish the live instance last: releaseResources() nulls
        // currentInstance while keeping ownedInstance, so without this a
        // release / re-prepare cycle leaves the slot silent until a reload.
        currentInstance.store (ownedInstance.get(), std::memory_order_release);
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
        // Re-publish the live remote: a prior oversized-block prepare or
        // releaseResources() may have nulled currentRemote, and the success
        // path must restore it or the slot stays silent until a reload.
        currentRemote.store (ownedRemote.get(), std::memory_order_release);
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
    {
        const juce::SpinLock::ScopedLockType processGuard (processLock);

        currentInstance.store (nullptr, std::memory_order_release);
        if (ownedInstance != nullptr)
        {
            ownedInstance->releaseResources();
            juce::MemoryBlock state;
            ownedInstance->getStateInformation (state);
            lastKnownStateBase64 = state.toBase64Encoding();
        }

       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        reaperTimer.stopTimer();
        currentRemote.store (nullptr, std::memory_order_release);
       #endif
    }

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Outside the lock: these two RPCs cost seconds against a slow child, and
    // the audio thread takes the lock before it reads currentRemote, so a block
    // that starts after the store above already skips this slot and one that
    // started before it has finished by the time the store happened.
    if (ownedRemote != nullptr)
    {
        std::string err;
        std::vector<std::uint8_t> state;
        if (ownedRemote->getState (state, err))
            lastKnownStateBase64 = state.empty()
                ? juce::String()
                : juce::MemoryBlock (state.data(), state.size()).toBase64Encoding();
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
    if (loadedDescriptor.has_value())
        return loadedDescriptor->name;
    return {};
}

bool PluginSlot::isOffline() const noexcept
{
    return offlineDescriptor.has_value() || offlineLegacyDescriptionXml.isNotEmpty();
}

void PluginSlot::setOfflineForCapture (const juce::String& displayName)
{
    if (displayName.isEmpty())
    {
        offlineDescriptor.reset();
        offlineLegacyDescriptionXml.clear();
        offlineStateBase64.clear();
        offlineCapturePlaceholder = false;
        return;
    }
    PluginDescriptor descriptor;
    descriptor.name = displayName.toStdString();
    offlineDescriptor = std::move (descriptor);
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64.clear();
    offlineCapturePlaceholder = true;
}

#if defined(DUSKSTUDIO_TESTS)
void PluginSlot::setTemporarilyInactivePersistenceForTest (
    PluginDescriptor descriptor, const juce::String& stateBase64)
{
    unload();
    loadedDescriptor = std::move (descriptor);
    lastKnownStateBase64 = stateBase64;
}

bool PluginSlot::installInProcessInstanceForTest (
    PluginInstancePtr instance)
{
    PluginDescriptor descriptor;
    descriptor.name = instance != nullptr ? instance->getName().toStdString()
                                          : std::string();
    descriptor.formatName = "Test";
    return installInProcessInstance (std::move (instance), std::move (descriptor));
}
#endif

juce::String PluginSlot::getOfflineName() const
{
    if (offlineDescriptor.has_value())
        return offlineDescriptor->name;
    return {};
}

bool PluginSlot::loadFromFile (const juce::File& pluginFile, juce::String& errorMessage)
{
    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound - call setManager() first";
        return false;
    }

    // Invalidate any audio-thread-queued ParamWrites still in the FIFO -
    // their paramIndex targeted the now-deposed plugin. Release pairs
    // with the audio-thread acquire load in setParamNormalised so the
    // bump is observed before the next stamp.
    currentLoadEpoch.fetch_add (1, std::memory_order_release);

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Drop any cached shell instance from a prior plugin so the Mac
    // shell-editor doesn't keep driving the new plugin's DSP with
    // stale param indices. If a wrapper is still outstanding the
    // releaseShellInstance refusal-log fires; ~PluginSlot handles the
    // tail leak.
    releaseShellInstance();
   #endif

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
    loadedDescriptor.reset();
    lastKnownStateBase64.clear();

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
    auto descriptor = manager->descriptorForInstance (*fresh);
    normalizeVst3Location (descriptor);
    logLoadedPlugin (*fresh, descriptor);

    ownedInstance = std::move (fresh);
    {
        // Blocking-acquire drains any in-flight process call before the
        // watchdog reset + publish - its overrun accounting could otherwise
        // land after the clear and re-bypass the fresh instance. Held through
        // the publish; the audio side only try-locks, so it dry-passes.
        const juce::SpinLock::ScopedLockType processGuard (processLock);
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
    }
    loadedDescriptor = std::move (descriptor);
    offlineDescriptor.reset();
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64.clear();
    offlineCapturePlaceholder = false;
    lastKnownStateBase64.clear();
    return true;
}

bool PluginSlot::loadFromDescriptor (const PluginDescriptor& descriptor,
                                     juce::String& errorMessage)
{
    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound - call setManager() first";
        return false;
    }

    // Bump first so any audio-thread setParamNormalised currently mid-
    // push stamps the NEW epoch (a write racing the swap is for the
    // freshly-loaded plugin; the drain compares epochs and applies it).
    // A write that completed BEFORE the bump carries the OLD epoch
    // and gets dropped at drain.
    currentLoadEpoch.fetch_add (1, std::memory_order_release);

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Drop any cached shell instance + listener so user knob moves on
    // a still-visible editor don't fan out to the new OOP child's
    // mismatched param table. Refusal-on-outstanding-wrapper is
    // handled by ~PluginSlot's leak-tail.
    releaseShellInstance();
   #endif

    // Normalize the path - cached KnownPluginList descriptions on Linux
    // carry the inner-.so path which findFormatForDescription rejects.
    PluginDescriptor fixedDescriptor = descriptor;
    normalizeVst3Location (fixedDescriptor);

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
    loadedDescriptor.reset();
    lastKnownStateBase64.clear();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // Tear down any prior OOP slot before deciding which path the new
    // load takes. Mode is per-load: we may end up in-process this time
    // even if the previous load was OOP (and vice versa).
    retireRemoteConnection();
    remoteCrashed.store (false, std::memory_order_relaxed);

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
                const auto descXmlStr = manager->descriptorToLegacyXml (fixedDescriptor);
                int  numIn = 0, numOut = 0, latency = 0;
                bool isInstrument = false;
                if (! remote->loadPlugin (descXmlStr.toStdString(),
                                            preparedSampleRate, preparedBlockSize,
                                            numIn, numOut, latency,
                                            isInstrument, err))
                {
                    std::fprintf (stderr,
                                  "[Dusk Studio/PluginSlot] OOP loadPlugin failed (%s); "
                                  "falling back to in-process.\n",
                                  err.c_str());
                }
                else
                {
                    publishRemoteConnection (std::move (remote),
                                              std::move (fixedDescriptor),
                                              numIn, numOut, latency, isInstrument);
                    return true;
                }
            }
        }
        // Fall through into the in-process load below - we still want
        // the user's load to succeed.
    }
   #endif

    auto fresh = manager->createPluginInstance (fixedDescriptor, preparedSampleRate,
                                                  preparedBlockSize, errorMessage);
    if (fresh == nullptr)
    {
        // No new plugin to install. Slot is now empty (currentInstance is
        // nullptr, ownedInstance reset). previousInstance still holds the
        // pre-swap plugin and will be released on the next swap.
        return false;
    }

    return installInProcessInstance (std::move (fresh), std::move (fixedDescriptor));
}

// Message-thread install tail shared by synchronous and off-thread descriptor
// loads. Primes the freshly-built
// instance, then atomically swaps it into currentInstance - the audio thread
// reads via acquire. Caller has already rotated the keep-alive ring + nulled
// currentInstance, so this only installs the new owner.
bool PluginSlot::installInProcessInstance (std::unique_ptr<juce::AudioPluginInstance> fresh,
                                           PluginDescriptor descriptor)
{
    if (fresh == nullptr) return false;

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
    descriptor = refreshFromInstance (*manager, *fresh, descriptor);
    logLoadedPlugin (*fresh, descriptor);

    ownedInstance = std::move (fresh);
    {
        // Same drain-then-reset-and-publish as loadFromFile above.
        const juce::SpinLock::ScopedLockType processGuard (processLock);
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
    }
    loadedDescriptor = std::move (descriptor);
    offlineDescriptor.reset();
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64.clear();
    offlineCapturePlaceholder = false;
    lastKnownStateBase64.clear();
    return true;
}

void PluginSlot::loadFromDescriptorAsync (const PluginDescriptor& descriptor,
                                          LoadCompletion onDone)
{
    if (manager == nullptr)
    {
        if (onDone) onDone (false, "PluginSlot has no PluginManager bound");
        return;
    }

    // Bump the load epoch first: a stale async completion compares epochs and
    // bails if a newer load / unload superseded it.
    currentLoadEpoch.fetch_add (1, std::memory_order_release);
    const auto epoch = currentLoadEpoch.load (std::memory_order_relaxed);

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    releaseShellInstance();
   #endif

    PluginDescriptor fixedDescriptor = descriptor;
    normalizeVst3Location (fixedDescriptor);

    // Rotate the keep-alive ring + null currentInstance now - the slot goes
    // silent for the duration of the off-thread load. Same shape as the
    // synchronous path's pre-swap rotation.
    if (ownedInstance != nullptr && lastTouchedListener != nullptr)
        for (auto* p : ownedInstance->getParameters())
            if (p != nullptr) p->removeListener (lastTouchedListener.get());
    lastTouchedListener.reset();
    lastTouchedParamIndex.store (-1, std::memory_order_relaxed);
    currentInstance.store (nullptr, std::memory_order_release);
    // Parked: clear the cached latency so the deposed plugin's value can't leak
    // into PDC accounting during the off-thread load window. installInProcess-
    // Instance restores it from the new instance on completion.
    cachedLatencySamples.store (0, std::memory_order_relaxed);
    if (previousInstances[1] != nullptr)
        previousInstances[1]->releaseResources();
    previousInstances[1] = std::move (previousInstances[0]);
    previousInstances[0] = std::move (ownedInstance);
    ownedInstance.reset();
    loadedDescriptor.reset();
    lastKnownStateBase64.clear();

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // A prior load may have been OOP (mode is per-load). Mirror the sync path's
    // remote teardown so processBlock stops routing to the remote before the
    // in-process instance becomes the active processor - without this the slot
    // keeps playing the OOP plugin after currentInstance is nulled.
    retireRemoteConnection();
    remoteCrashed.store (false, std::memory_order_relaxed);

    if (manager->isOopEnabled()
        && preparedBlockSize > 0
        && preparedBlockSize <= duskstudio::ipc::kMaxBlock)
    {
        const auto hostPath = manager->getHostExecutablePath();
        if (hostPath.isNotEmpty() && juce::File (hostPath).existsAsFile())
        {
            beginRemoteLoad (std::move (fixedDescriptor), epoch,
                              hostPath.toStdString(), std::move (onDone));
            return;
        }
        std::fprintf (stderr,
                      "[Dusk Studio/PluginSlot] OOP requested but host binary not found "
                      "at \"%s\"; falling back to in-process.\n",
                      hostPath.toRawUTF8());
    }
   #endif

    beginInProcessLoad (std::move (fixedDescriptor), epoch, std::move (onDone));
}

void PluginSlot::beginInProcessLoad (PluginDescriptor descriptor,
                                     std::uint32_t epoch,
                                     LoadCompletion onDone)
{
    // Off-thread create; the completion fires on the MESSAGE thread. weak_ptr
    // to lifeToken guards against the slot being destroyed mid-load (token dies
    // with the slot -> weak_ptr expires -> bail before touching `this`).
    std::weak_ptr<char> life = lifeToken;
    manager->createPluginInstanceAsync (descriptor, preparedSampleRate, preparedBlockSize,
        [this, life, epoch, onDone, descriptor]
        (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String err)
    {
        if (! life.lock()) return;                      // slot destroyed mid-load
        if (currentLoadEpoch.load (std::memory_order_relaxed) != epoch)
        {
            if (onDone) onDone (false, "load superseded");   // newer load / unload won
            return;
        }
        if (inst == nullptr)
        {
            if (onDone) onDone (false, err.isNotEmpty() ? err
                                                        : juce::String ("plugin creation failed"));
            return;
        }
        const bool ok = installInProcessInstance (std::move (inst), descriptor);
        if (onDone) onDone (ok, ok ? juce::String() : juce::String ("install failed"));
    });
}

void PluginSlot::unload()
{
    // Invalidate any audio-thread-queued ParamWrites for the deposed
    // plugin. Release pairs with the audio-thread acquire load.
    currentLoadEpoch.fetch_add (1, std::memory_order_release);

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    // Track-unload is an explicit user action and the modal-editor close
    // protocol (closePluginEditor -> modal close -> wrapper dtor) is
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
    retireRemoteConnection();
    remoteNumIn .store (0, std::memory_order_relaxed);
    remoteNumOut.store (0, std::memory_order_relaxed);
    remoteIsInstrument.store (false, std::memory_order_relaxed);
    remoteCrashed.store (false, std::memory_order_relaxed);
   #endif
    loadedDescriptor.reset();
    offlineDescriptor.reset();
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64.clear();
    offlineCapturePlaceholder = false;
    lastKnownStateBase64.clear();
}

std::optional<PluginDescriptor> PluginSlot::getDescriptorForSave (int parkSleepMs)
{
    (void) parkSleepMs;
    if (loadedDescriptor.has_value())
        return loadedDescriptor;
    if (offlineCapturePlaceholder)
        return std::nullopt;
    return offlineDescriptor;
}

juce::String PluginSlot::getLegacyDescriptionXmlForSave() const
{
    if (loadedDescriptor.has_value()
        || (offlineDescriptor.has_value() && ! offlineCapturePlaceholder))
        return {};
    return offlineLegacyDescriptionXml;
}

bool PluginSlot::isLoadedPluginInstrument() const
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (currentRemote.load (std::memory_order_acquire) != nullptr)
        return remoteIsInstrument.load (std::memory_order_relaxed);
   #endif

    return loadedDescriptor.has_value() && loadedDescriptor->isInstrument;
}

juce::String PluginSlot::getStateBase64ForSave (int parkSleepMs)
{
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (auto* r = currentRemote.load (std::memory_order_acquire))
    {
        // OOP path: the parking + setActive(false) bracket is intrinsic
        // to the IPC - the child runs the plugin on its own audio worker
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
            return lastKnownStateBase64;
        }
        lastKnownStateBase64 = blob.empty()
            ? juce::String()
            : juce::MemoryBlock (blob.data(), blob.size()).toBase64Encoding();
        return lastKnownStateBase64;
    }
    (void) parkSleepMs;

    if (ownedRemote != nullptr && loadedDescriptor.has_value())
        return lastKnownStateBase64;
   #endif

    // Slot is empty but holds an offline placeholder. Round-trip the
    // stashed state so a save while offline doesn't wipe the user's
    // data on disk.
    if (currentInstance.load (std::memory_order_acquire) == nullptr
        && offlineStateBase64.isNotEmpty())
        return offlineStateBase64;

    if (currentInstance.load (std::memory_order_acquire) == nullptr
        && ownedInstance != nullptr && loadedDescriptor.has_value())
    {
        juce::MemoryBlock inactiveState;
        ownedInstance->getStateInformation (inactiveState);
        lastKnownStateBase64 = inactiveState.toBase64Encoding();
        return lastKnownStateBase64;
    }

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
    if (p == nullptr) return lastKnownStateBase64;
    lastKnownStateBase64 = mb.toBase64Encoding();
    return lastKnownStateBase64;
}

bool PluginSlot::restoreFromSavedState (
    const std::optional<PluginDescriptor>& descriptor,
    const juce::String& legacyDescriptionXml,
    const juce::String& stateBase64,
    juce::String& errorMessage)
{
    if (! descriptor.has_value() && legacyDescriptionXml.isEmpty())
    {
        unload();
        return true;
    }

    currentLoadEpoch.fetch_add (1, std::memory_order_release);

   #if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
    releaseShellInstance();
   #endif

    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound";
        return false;
    }

    PluginDescriptor persistedDescriptor;
    if (descriptor.has_value())
    {
        persistedDescriptor = *descriptor;
    }
    else if (! manager->descriptorFromLegacyXml (legacyDescriptionXml,
                                                  persistedDescriptor))
    {
        unload();
        offlineDescriptor.reset();
        offlineLegacyDescriptionXml = legacyDescriptionXml;
        offlineStateBase64 = stateBase64;
        offlineCapturePlaceholder = false;
        errorMessage = "Saved plugin description is not valid legacy XML";
        return false;
    }

    PluginDescriptor descriptorToLoad = persistedDescriptor;
    normalizeVst3Location (descriptorToLoad);
    offlineDescriptor = persistedDescriptor;
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64 = stateBase64;
    offlineCapturePlaceholder = false;
    lastKnownStateBase64 = stateBase64;

    // Try the OOP path first if enabled. On any failure we fall through
    // to the in-process load below - the user's session restore should
    // succeed even if the host child can't be spawned for some reason
    // (e.g. binary not present in a stripped-down build).
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    if (manager->isOopEnabled()
        && preparedBlockSize > 0
        && preparedBlockSize <= duskstudio::ipc::kMaxBlock)
    {
        if (loadFromDescriptor (descriptorToLoad, errorMessage))
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
            offlineDescriptor.reset();
            offlineLegacyDescriptionXml.clear();
            offlineStateBase64.clear();
            offlineCapturePlaceholder = false;
            lastKnownStateBase64 = stateBase64;
            return true;
        }
    }
   #endif

    // Same swap-load shape as loadFromFile/loadFromDescriptor, with the
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

    auto fresh = manager->createPluginInstance (descriptorToLoad, preparedSampleRate,
                                                  preparedBlockSize, errorMessage);
    if (fresh == nullptr)
        return false;

    disableAuxiliaryBuses (*fresh);
    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate, preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);
    auto loaded = refreshFromInstance (*manager, *fresh, descriptorToLoad);
    logLoadedPlugin (*fresh, loaded);

    // Apply the saved state blob (if any).
    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (stateBase64) && mb.getSize() > 0)
            fresh->setStateInformation (mb.getData(), (int) mb.getSize());
    }

    ownedInstance = std::move (fresh);
    {
        // Same watchdog reset as loadFromFile / installInProcessInstance -
        // autoBypassed is cleared nowhere else, so without this a slot whose
        // plugin tripped the time-budget watchdog stays silently bypassed
        // across a session load. Blocking-acquire the process lock so an
        // audio block still running against the parked instance drains
        // first; its overrun accounting could otherwise land after the
        // clear and re-bypass the freshly restored plugin.
        const juce::SpinLock::ScopedLockType processGuard (processLock);
        blocksSinceLoad     = 0;
        consecutiveOverruns = 0;
        autoBypassed.store (false, std::memory_order_relaxed);
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
    }
    loadedDescriptor = std::move (loaded);
    offlineDescriptor.reset();
    offlineLegacyDescriptionXml.clear();
    offlineStateBase64.clear();
    offlineCapturePlaceholder = false;
    lastKnownStateBase64 = stateBase64;
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

    // Dry-pass if prepareToPlay / releaseResources holds the lock (see header).
    const juce::SpinLock::ScopedTryLockType processGuard (processLock);
    if (! processGuard.isLocked())
        return;

    // Time-budget watchdog. A plugin that consistently overruns the buffer
    // gets auto-bypassed so it can't freeze the audio thread. Two
    // refinements over a naive single-block trip:
    //   - Warm-up grace: kGraceBlocks after a load (or prepareToPlay) the
    //     watchdog is silent. Reverbs / look-ahead limiters / oversamplers
    //     all do real work on their first few blocks (cold caches, internal
    //     ramps) and would otherwise be auto-bypassed before they ever
    //     produce wet output.
    //   - Consecutive-overrun threshold: a single late block (other-thread
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

        dusk::copyEventsWhole (midiMessages, oopMidiScratch);
        if (! r->processBlockSync (inPtrs, std::max (rNumIn, 0),
                                       std::max (rNumOut, 0),
                                       numSamples, oopMidiScratch,
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

        const int procCh = std::min (numOut, stereoScratch.getNumChannels());
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
        // Stereo (or wider) plugin: duplicate mono -> L+R, process, average
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

    // Dry-pass if prepareToPlay / releaseResources holds the lock (see header).
    const juce::SpinLock::ScopedTryLockType processGuard (processLock);
    if (! processGuard.isLocked())
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

        dusk::copyEventsWhole (midiMessages, oopMidiScratch);
        if (! r->processBlockSync (inPtrs, std::max (rNumIn, 0),
                                       std::max (rNumOut, 0),
                                       numSamples, oopMidiScratch,
                                       kOopProcessTimeoutNs))
        {
            // Either the futex timed out or the connection was already
            // marked crashed. Engage auto-bypass so the engine sees a
            // clean silence/pass-through instead of repeating the
            // timeout every block (the futex cost itself is what we're
            // avoiding here - re-trying a dead connection still pays
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
        // - JUCE's contract is that the plugin only reads numIn channels,
        // but copying mono there is harmless and avoids leaving uninit
        // memory in the buffer (which we otherwise read back as outR).
        const int procCh = std::min (std::max (numIn, numOut),
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

        const int procCh = std::min (numOut, stereoScratch.getNumChannels());
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
        // FIFO full. Drop the write - protecting the audio thread from
        // blocking is more important than a single missed param update,
        // and the 30 Hz drain catches up within one tick once the queue
        // pressure subsides. With 256 entries this branch is only
        // reachable if the audio thread is producing > 7680 writes/s
        // sustained (256 × 30 Hz), which no realistic MIDI controller
        // can do.
        return;
    }

    const int slot = (sz1 > 0) ? s1 : s2;
    paramQueue[(size_t) slot] = ParamWrite {
        paramIndex,
        std::clamp (value01, 0.0f, 1.0f),
        currentLoadEpoch.load (std::memory_order_acquire)
    };
    paramFifo.finishedWrite (1);
}

void PluginSlot::timerCallback()
{
    // Message thread. Drain every pending ParamWrite into the live
    // plugin instance. Bounded loop - caller (dusk::Timer) marshals us
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
    // but no DSP would update - confusing. Better: detach the
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
    // if a future change ever routes the call from the audio thread -
    // exactly the regression Phase 2 is meant to prevent.
    JUCE_ASSERT_MESSAGE_THREAD;

    // Discard writes queued against a prior plugin load. The audio
    // thread stamped pw.loadEpoch from currentLoadEpoch at push time;
    // any unload / load / restore on the message thread fetch_add's
    // currentLoadEpoch, so a stale entry's epoch lags behind and is
    // dropped here. Without this, paramIndex 5 queued for plugin A
    // would apply to plugin B's param 5 after a hot-swap.
    if (pw.loadEpoch != currentLoadEpoch.load (std::memory_order_acquire))
        return;

    if (pw.paramIndex < 0) return;

    if (auto* p = currentInstance.load (std::memory_order_acquire))
    {
        const auto& params = p->getParameters();
        if (pw.paramIndex >= params.size()) return;
        if (auto* param = params[pw.paramIndex])
            param->setValueNotifyingHost (pw.value);
        return;
    }

   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    // In-process instance not present - this slot is OOP-routed.
    // Forward the write across the IPC control channel; the child
    // applies it on its DSP-side instance via the existing
    // SetParam -> callAsync -> setValueNotifyingHost path. Acquire
    // load pairs with the message-thread release stores in
    // descriptor load / unload that swap currentRemote.
    if (auto* r = currentRemote.load (std::memory_order_acquire))
        (void) r->setRemoteParam (pw.paramIndex, pw.value);
   #endif
}

#if JUCE_MAC && DUSKSTUDIO_HAS_OOP_PLUGINS
bool PluginSlot::ensureShellInstanceForEditor (juce::String& err)
{
    JUCE_ASSERT_MESSAGE_THREAD;

    if (shellInstanceForEditor != nullptr)
    {
        err.clear();
        return true;  // idempotent - already loaded for this slot
    }

    if (manager == nullptr)
    {
        err = "PluginSlot has no PluginManager bound";
        return false;
    }
    if (! loadedDescriptor.has_value())
    {
        err = "No plugin loaded - nothing to host in the shell";
        return false;
    }

    // Load a fresh in-process instance against the same
    // file. The instance is editor-only: prepareToPlay is called so
    // the editor's bounds + initial parameter snapshot match the
    // engine's current SR/BS, but processBlock is NEVER invoked on
    // this instance - DSP runs in the OOP child.
    auto fresh = manager->createPluginInstance (*loadedDescriptor,
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
    // mirror are wired before the editor opens - the next user knob
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
                // Already on the message thread - RemotePluginConnection's
                // reader marshalled here via dusk::callAsync.
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
    // anyway (peer closed -> write fails) but isCrashed() short-circuits
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
    param->setValueNotifyingHost (std::clamp (value01, 0.0f, 1.0f));
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
        // Wrapper is still alive - its inner unique_ptr<editor> holds a
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
    // from invoking the listener on a freed processor - the lambda
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
