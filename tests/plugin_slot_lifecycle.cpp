#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#if ! defined (_WIN32)
 #include <csignal>
 #include <sys/types.h>
#endif

using namespace duskstudio;

namespace
{
class LifecyclePluginInstance final : public juce::AudioPluginInstance
{
public:
    using juce::AudioPluginInstance::processBlock;

    LifecyclePluginInstance()
        : AudioPluginInstance (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Lifecycle test"; }

    void prepareToPlay (double, int) override
    {
        ++prepareCalls;
        setLatencySamples (latencyOnPrepare);
    }
    void releaseResources() override          { ++releaseCalls; }

    void processBlock (juce::AudioBuffer<float>&,
                       juce::MidiBuffer&) override
    {
        ++processCalls;
    }

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                      { return false; }
    bool acceptsMidi() const override                    { return false; }
    bool producesMidi() const override                   { return false; }
    double getTailLengthSeconds() const override         { return 0.0; }

    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override    {}

    void fillInPluginDescription (juce::PluginDescription& description) const override
    {
        description.name = getName();
        description.pluginFormatName = "Test";
        description.fileOrIdentifier = "lifecycle-test";
    }

    int prepareCalls = 0;
    int releaseCalls = 0;
    int processCalls = 0;
    int latencyOnPrepare = 0;
};
} // namespace

TEST_CASE ("PluginSlot republishes an in-process instance after release and prepare")
{
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    auto instance = std::make_unique<LifecyclePluginInstance>();
    auto* lifecycle = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));

    slot.releaseResources();
    slot.prepareToPlay (48000.0, 64);

    float left[64] {};
    float right[64] {};
    juce::MidiBuffer midi;
    slot.processStereoBlock (left, right, 64, midi);

    CHECK (lifecycle->releaseCalls == 2);
    CHECK (lifecycle->prepareCalls == 2);
    CHECK (lifecycle->processCalls == 1);
}

// A save reads a plug-in's state between a release and a prepare, and a plug-in
// can settle on another latency when it is prepared, as a look-ahead limiter
// does after its look-ahead has been changed. PDC has to follow the latency the
// plug-in has once the save is done, not the one it had before.
TEST_CASE ("PluginSlot re-reads a plugin's latency when a save re-prepares it", "[plugin]")
{
    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    auto instance = std::make_unique<LifecyclePluginInstance>();
    auto* lifecycle = instance.get();
    lifecycle->latencyOnPrepare = 32;
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    REQUIRE (slot.getLatencySamples() == 32);

    lifecycle->latencyOnPrepare = 128;
    (void) slot.getStateBase64ForSave (0);
    CHECK (lifecycle->releaseCalls == 1);
    CHECK (lifecycle->prepareCalls == 2);
    CHECK (slot.getLatencySamples() == 128);

    // And the slot is back on the audio path, re-prepared.
    float left[64] {};
    float right[64] {};
    juce::MidiBuffer midi;
    slot.processStereoBlock (left, right, 64, midi);
    CHECK (lifecycle->processCalls == 1);
}

// With no audio device open nothing has given the slot a sample rate or block
// size, so a user load is refused the way the native hosts refuse theirs, before
// any binary is opened and without disturbing what the slot already holds. The
// paths below do not exist: a refusal for the right reason carries the
// not-prepared message, not a file-not-found one.
TEST_CASE ("PluginSlot refuses in-process loads until a device prepares it",
           "[plugin][device]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File missing = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                   .getChildFile ("dusk-unprepared-slot-never-opened.vst3");
    PluginDescriptor descriptor;
    descriptor.name = "Never opened";
    descriptor.formatName = "VST3";
    descriptor.location = missing.getFullPathName().toStdString();

    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);

    auto instance = std::make_unique<LifecyclePluginInstance>();
    auto* lifecycle = instance.get();
    REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    REQUIRE (lifecycle->prepareCalls == 0);

    const auto keptPrevious = [&]
    {
        CHECK (slot.isLoaded());
        CHECK (slot.getInstance() == lifecycle);
        CHECK (slot.getLoadedName() == "Lifecycle test");
        CHECK_FALSE (slot.isOffline());
    };

    juce::String error;
    CHECK_FALSE (slot.loadFromFile (missing, error));
    CHECK (error == "plugin slot not prepared");
    keptPrevious();

    error.clear();
    CHECK_FALSE (slot.loadFromDescriptor (descriptor, error));
    CHECK (error == "plugin slot not prepared");
    keptPrevious();

    bool completed = false;
    bool succeeded = true;
    juce::String asyncError;
    slot.loadFromDescriptorAsync (descriptor, [&] (bool ok, juce::String err)
    {
        completed = true;
        succeeded = ok;
        asyncError = err;
    });
    CHECK (completed);
    CHECK_FALSE (succeeded);
    CHECK (asyncError == "plugin slot not prepared");
    keptPrevious();

    // The kept plugin is prepared once a device arrives, and from then on the
    // same load reaches the loader and fails on its own terms.
    slot.prepareToPlay (48000.0, 64);
    CHECK (lifecycle->prepareCalls == 1);

    error.clear();
    CHECK_FALSE (slot.loadFromFile (missing, error));
    CHECK (error.isNotEmpty());
    CHECK (error != "plugin slot not prepared");
}

#if DUSKSTUDIO_HAS_OOP_PLUGINS
namespace
{
PluginDescriptor sandboxTestDescriptor()
{
    PluginDescriptor descriptor;
    descriptor.name = "sandbox stub";
    descriptor.formatName = "VST3";
    descriptor.location = "/nonexistent/sandbox-stub.vst3";
    return descriptor;
}

// The child is normally resolved beside the running executable; under ctest that
// is the Catch2 binary, so the sandboxed branch is unreachable without pointing
// the slot at the built child and one of its stub modes.
void useSandboxStub (PluginManager& manager, const char* modeArg)
{
    manager.setOopEnabled (true);
    manager.setHostExecutableOverride (DUSKSTUDIO_PLUGIN_HOST_PATH, modeArg);
}

#if ! defined (__APPLE__)
// Runs the dispatch loop until `done` holds or the deadline passes. This JUCE
// build has no runDispatchLoopUntil, and stopDispatchLoop latches the quit flag
// for the life of the MessageManager, so a test gets exactly one pump: a loop
// of short pumps would dispatch nothing after the first.
struct LoopStopper final : dusk::Timer
{
    std::function<bool()> done;
    std::chrono::steady_clock::time_point deadline;

    void timerCallback() override
    {
        if (! done() && std::chrono::steady_clock::now() < deadline) return;
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
};

void pumpUntil (std::function<bool()> done, std::chrono::milliseconds timeout)
{
    LoopStopper stopper;
    stopper.done = std::move (done);
    stopper.deadline = std::chrono::steady_clock::now() + timeout;
    stopper.startTimer (10);
    juce::MessageManager::getInstance()->runDispatchLoop();
}
#endif
} // namespace

// Connecting to the plugin host waits up to 5 s for the handshake and its
// LoadPlugin RPC up to 30 s, so running either on the message thread hands a
// plugin that stalls in the child the power to freeze the editor for over half
// a minute - the failure the sandbox exists to contain. The load has to be
// handed off, which shows up here as the completion never arriving inside the
// call and the slot only going remote once the message loop runs again.
#if ! defined (__APPLE__)
TEST_CASE ("PluginSlot completes an out-of-process load off the message thread")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    bool succeeded = false;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        completed = true;
        succeeded = ok;
    });

    CHECK_FALSE (completed);
    CHECK_FALSE (slot.isRemote());

    // Generous: a sanitizer build spawns the child slowly, and the loop stops
    // as soon as the completion lands.
    pumpUntil ([&] { return completed; }, std::chrono::seconds (15));

    CHECK (completed);
    CHECK (succeeded);
    CHECK (slot.isRemote());
}

#if ! defined (_WIN32)
// A sandboxed plug-in's latency is in delay compensation while its child runs
// it. A child that dies takes the plug-in out of the signal path, and Re-enable
// on the crashed slot drops the dead child, so neither leaves a latency behind
// for the other tracks to be delayed by.
TEST_CASE ("PluginSlot reports no latency for a crashed sandbox child before or after Re-enable",
           "[plugin][ipc]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    bool succeeded = false;
    bool killed = false;
    int latencyWhileRunning = -1;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        completed = true;
        succeeded = ok;
    });

    // The crash is noticed by the slot's reaper timer, so the whole case runs
    // inside the one dispatch loop a test gets.
    pumpUntil ([&]
    {
        if (! completed) return false;
        if (! succeeded || ! slot.isRemote()) return true;
        if (! killed)
        {
            latencyWhileRunning = slot.getLatencySamples();
            killed = ::kill ((pid_t) slot.getRemoteChildPid(), SIGKILL) == 0;
            return ! killed;
        }
        return slot.wasCrashed();
    }, std::chrono::seconds (20));

    REQUIRE (succeeded);
    REQUIRE (killed);
    REQUIRE (slot.wasCrashed());
    CHECK (latencyWhileRunning == ipc::kLoadStubLatencySamples);
    CHECK (slot.wasAutoBypassed());
    CHECK (slot.getLatencySamples() == 0);

    slot.clearAutoBypass();
    CHECK_FALSE (slot.wasCrashed());
    CHECK_FALSE (slot.isRemote());
    CHECK (slot.getLatencySamples() == 0);
}
#endif

// With no child binary where the loader looks, the sandbox is simply not
// available: the load must still take the in-process path rather than fail or
// wait on a host that will never answer.
TEST_CASE ("PluginSlot falls back to in-process when the host binary is missing")
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    manager.setOopEnabled (true);
    manager.setHostExecutableOverride (
        (juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("dusk-studio-plugin-host-that-is-not-there")
            .getFullPathName()).toStdString(),
        "--ipc-load-reply-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    bool completed = false;
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool, juce::String) { completed = true; });

    pumpUntil ([&] { return completed; }, std::chrono::seconds (15));

    CHECK (completed);
    // The descriptor names a plugin that does not exist either, so the
    // in-process load it fell back to fails on its own terms. What matters is
    // that it never went remote and never waited for a handshake.
    CHECK_FALSE (slot.isRemote());
}

// The same async load, with the device callback already running across it. A
// child spawned from the load worker dies with that worker, which the startup
// restore never sees because it spawns from the message thread. Needs the stub
// that answers block commands as well as control RPCs; the control-only one
// cannot keep any slot alive under a live callback.
TEST_CASE ("PluginSlot keeps its sandbox child when the graph is live across the load")
{
    using namespace std::chrono_literals;

    juce::ScopedJuceInitialiser_GUI juceInit;

    PluginManager manager;
    useSandboxStub (manager, "--ipc-load-audio-stub");

    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    std::atomic<bool> stopAudio { false };
    std::atomic<int>  blocksRun { 0 };

    std::thread audioThread ([&]
    {
        float left[64] {};
        float right[64] {};
        juce::MidiBuffer midi;
        while (! stopAudio.load (std::memory_order_acquire))
        {
            slot.processStereoBlock (left, right, 64, midi);
            blocksRun.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::sleep_for (1333us);
        }
    });

    while (blocksRun.load (std::memory_order_relaxed) < 8)
        std::this_thread::yield();

    std::atomic<bool> completed { false };
    std::atomic<bool> succeeded { false };
    slot.loadFromDescriptorAsync (sandboxTestDescriptor(),
                                  [&] (bool ok, juce::String)
    {
        succeeded.store (ok, std::memory_order_release);
        completed.store (true, std::memory_order_release);
    });

    // Keeps pumping past the completion: the child is lost about 200 ms after a
    // load that reported success, so stopping at the completion would pass.
    std::chrono::steady_clock::time_point settleUntil {};
    pumpUntil ([&]
    {
        if (! completed.load (std::memory_order_acquire)) return false;
        if (settleUntil.time_since_epoch().count() == 0)
            settleUntil = std::chrono::steady_clock::now() + 1500ms;
        return std::chrono::steady_clock::now() >= settleUntil;
    }, std::chrono::seconds (20));

    stopAudio.store (true, std::memory_order_release);
    audioThread.join();

    REQUIRE (completed.load (std::memory_order_acquire));
    REQUIRE (succeeded.load (std::memory_order_acquire));
    CHECK_FALSE (slot.wasCrashed());
    CHECK_FALSE (slot.wasAutoBypassed());
    CHECK (slot.isRemote());
}
#endif

// Quitting while a child stalls used to cost the destructor the whole handshake
// budget plus the whole LoadPlugin deadline, per slot, on the message thread.
// The worker is cancellable now, so both stalls unwind in about one poll slice
// plus the child teardown.
TEST_CASE ("PluginSlot destruction cancels a stalled out-of-process load")
{
    using namespace std::chrono_literals;

    // Enough for the fork/exec and, where the child acks, the handshake - so
    // the worker really is parked in the wait this covers.
    constexpr auto kSettleTime = 200ms;
    constexpr auto kBound      = 1000ms;

    const char* modeArg = nullptr;
    SECTION ("stalled in the LoadPlugin reply wait") { modeArg = "--ipc-stub"; }
    SECTION ("stalled in the ready handshake")       { modeArg = "--ipc-mute-handshake-stub"; }

    PluginManager manager;
    useSandboxStub (manager, modeArg);

    std::chrono::steady_clock::duration elapsed {};
    {
        auto slot = std::make_unique<PluginSlot>();
        slot->setManager (manager);
        slot->prepareToPlay (48000.0, 64);
        slot->loadFromDescriptorAsync (sandboxTestDescriptor(),
                                       [] (bool, juce::String) {});

        std::this_thread::sleep_for (kSettleTime);

        const auto start = std::chrono::steady_clock::now();
        slot.reset();
        elapsed = std::chrono::steady_clock::now() - start;
    }

    INFO ("destructor took "
          << std::chrono::duration_cast<std::chrono::milliseconds> (elapsed).count()
          << " ms");
    CHECK (elapsed < kBound);
}

// The audio thread try-locks processLock and then stays inside
// RemotePluginConnection::processBlockSync for up to the OOP block timeout,
// reading the child's shared memory the whole time. Every message-thread path
// that rotates the deferred-destruction ring destroys the connection evicted
// from the far slot, which unmaps that shared memory - so the rotation has to
// happen with the lock held, not merely after currentRemote has been nulled.
TEST_CASE ("PluginSlot rotates the remote ring under the process lock")
{
    using namespace std::chrono_literals;

    PluginManager manager;
    PluginSlot slot;
    slot.setManager (manager);
    slot.prepareToPlay (48000.0, 64);

    std::atomic<bool> holdingLock { false };
    std::atomic<bool> holdComplete { false };

    std::thread audioThread ([&]
    {
        const juce::SpinLock::ScopedLockType processGuard (slot.getProcessLock());
        holdingLock.store (true, std::memory_order_release);
        std::this_thread::sleep_for (150ms);
        holdComplete.store (true, std::memory_order_release);
    });

    while (! holdingLock.load (std::memory_order_acquire))
        std::this_thread::yield();

    slot.unload();
    const bool waitedForAudioThread = holdComplete.load (std::memory_order_acquire);

    audioThread.join();
    CHECK (waitedForAudioThread);
}
#endif
