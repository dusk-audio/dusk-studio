#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

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

    void prepareToPlay (double, int) override { ++prepareCalls; }
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
    manager.setHostExecutableForTest (DUSKSTUDIO_PLUGIN_HOST_PATH, modeArg);
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
