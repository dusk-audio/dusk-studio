#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"

#include <atomic>
#include <chrono>
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
