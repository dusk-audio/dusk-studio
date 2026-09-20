#include <catch2/catch_test_macros.hpp>

#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace duskstudio;

namespace
{
constexpr double kRate = 48000.0;
constexpr int    kBlock = 64;
// The watchdog's own constants. A block at this rate and size is 1.33 ms of
// audio and the in-process budget is 60% of it, so "slow" only has to be a
// millisecond to be unambiguously over.
constexpr int kGraceBlocks = 16;
constexpr int kOverrunsToTrip = 4;

// The unit under test is a wall-clock watchdog, so the only way to overrun its
// budget is to actually take the time.
class SlowPluginInstance final : public juce::AudioPluginInstance
{
public:
    using juce::AudioPluginInstance::processBlock;

    SlowPluginInstance()
        : AudioPluginInstance (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Slow test"; }

    void prepareToPlay (double, int) override {}
    void releaseResources() override          {}

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        ++processCalls;
        const int ms = blockMs.load (std::memory_order_relaxed);
        if (ms > 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (ms));
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
        description.fileOrIdentifier = "slow-test";
    }

    std::atomic<int> blockMs { 0 };
    int processCalls = 0;
};

struct Harness
{
    PluginManager manager;
    PluginSlot slot;
    SlowPluginInstance* plugin = nullptr;

    Harness()
    {
        slot.setManager (manager);
        slot.prepareToPlay (kRate, kBlock);
        auto instance = std::make_unique<SlowPluginInstance>();
        plugin = instance.get();
        REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    }

    void pump (int blocks, int blockMs)
    {
        plugin->blockMs.store (blockMs, std::memory_order_relaxed);
        float left[kBlock] {};
        float right[kBlock] {};
        juce::MidiBuffer midi;
        for (int i = 0; i < blocks; ++i)
            slot.processStereoBlock (left, right, kBlock, midi);
    }

    void warmUp() { pump (kGraceBlocks, 0); }
};
} // namespace

// A plugin that keeps overrunning its share of the buffer is bypassed so it
// cannot take the audio thread down with it. One late block is not enough:
// scheduling jitter would bypass innocent plugins.
TEST_CASE ("a plugin over its budget is bypassed once it overruns four blocks in a row",
           "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();
    REQUIRE_FALSE (h.slot.wasAutoBypassed());

    h.pump (kOverrunsToTrip - 1, 3);
    CHECK_FALSE (h.slot.wasAutoBypassed());

    h.pump (1, 3);
    CHECK (h.slot.wasAutoBypassed());

    // Bypassed means the plugin stops being called at all.
    const int calls = h.plugin->processCalls;
    h.pump (4, 0);
    CHECK (h.plugin->processCalls == calls);
}

// A run of late blocks broken by a block inside the budget starts the count
// again, so a plugin that is merely occasionally late keeps running.
TEST_CASE ("an occasional late block does not bypass a plugin", "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();

    for (int i = 0; i < 6; ++i)
    {
        h.pump (kOverrunsToTrip - 1, 3);
        h.pump (1, 0);
    }

    CHECK_FALSE (h.slot.wasAutoBypassed());
    CHECK (h.plugin->processCalls > 0);
}

// Reverbs, look-ahead limiters and oversamplers all do real work on their first
// blocks. Bypassing them for it would mean they never produce wet output at all.
TEST_CASE ("a plugin is not bypassed for overrunning its first blocks", "[plugin][autobypass]")
{
    Harness h;
    h.pump (kGraceBlocks, 3);
    CHECK_FALSE (h.slot.wasAutoBypassed());
}

// Re-enable plugin, from the slot's right-click menu, puts it back to work.
TEST_CASE ("re-enabling a bypassed plugin puts it back in the chain", "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());

    h.slot.clearAutoBypass();
    CHECK_FALSE (h.slot.wasAutoBypassed());

    const int calls = h.plugin->processCalls;
    h.pump (2, 0);
    CHECK (h.plugin->processCalls == calls + 2);
}

// Re-preparing the slot is how a device change reaches a plugin. It re-arms
// the warm-up grace, but the bypass itself is the user's to lift: a plugin that
// was overrunning does not quietly come back under a new buffer size.
TEST_CASE ("preparing a slot again leaves an auto-bypass in place", "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());

    h.slot.prepareToPlay (kRate, kBlock);
    CHECK (h.slot.wasAutoBypassed());

    h.slot.clearAutoBypass();
    // The grace is back, so the first blocks after the re-prepare cannot trip
    // the watchdog again.
    h.pump (kGraceBlocks, 3);
    CHECK_FALSE (h.slot.wasAutoBypassed());
}
