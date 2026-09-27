#include <catch2/catch_test_macros.hpp>

#include "engine/PdcMath.h"
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

    void prepareToPlay (double, int) override { setLatencySamples (latencyOnPrepare); }
    void releaseResources() override          {}

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        ++processCalls;
        // Some hosts pick a latency change up inside the block the plug-in runs
        // (JUCE's LV2 host reads the latency port after each run), so the
        // latency can be written on the audio thread. Once armed, every block
        // reports one sample more than the last, so the plug-in's final access
        // before a bypass is a write.
        if (const int latency = nextLatencyInProcess.load (std::memory_order_relaxed); latency >= 0)
        {
            setLatencySamples (latency);
            nextLatencyInProcess.store (latency + 1, std::memory_order_relaxed);
        }
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
    std::atomic<int> processCalls { 0 };
    std::atomic<int> nextLatencyInProcess { -1 };
    int latencyOnPrepare = 0;
};

struct Harness
{
    PluginManager manager;
    PluginSlot slot;
    SlowPluginInstance* plugin = nullptr;

    explicit Harness (int latency = 0)
    {
        slot.setManager (manager);
        slot.prepareToPlay (kRate, kBlock);
        auto instance = std::make_unique<SlowPluginInstance>();
        plugin = instance.get();
        plugin->latencyOnPrepare = latency;
        REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    }

    // Saving reads the state between a release and a prepare, the way a
    // session save does with the audio thread held out.
    void save() { (void) slot.getStateBase64ForSave (0); }

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

// A save reads each plug-in's state between a release and a prepare, so the
// plug-in comes back as cold as after a load. A reverb or look-ahead limiter
// saved while playing must not be bypassed for its first blocks back.
TEST_CASE ("a plugin re-prepared by a save gets the warm-up grace back", "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();

    h.save();
    h.pump (kGraceBlocks, 3);
    CHECK_FALSE (h.slot.wasAutoBypassed());
}

// Late blocks from before the save do not count against the plug-in after it.
TEST_CASE ("a save starts the count of late blocks again", "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();
    h.pump (kOverrunsToTrip - 1, 3);
    REQUIRE_FALSE (h.slot.wasAutoBypassed());

    h.save();
    h.pump (kGraceBlocks, 0);
    h.pump (1, 3);
    CHECK_FALSE (h.slot.wasAutoBypassed());
}

// An auto-bypassed plug-in passes dry and adds no delay for PDC to make up. The
// save's re-prepare lets it report a latency again, which must not reach PDC
// until the plug-in is back in the path, and then it is the latency it settled
// on at that prepare.
TEST_CASE ("a save leaves an auto-bypassed plugin reporting no latency", "[plugin][autobypass]")
{
    Harness h (64);
    REQUIRE (h.slot.getLatencySamples() == 64);
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());
    REQUIRE (h.slot.getLatencySamples() == 0);

    h.plugin->latencyOnPrepare = 96;
    h.save();
    CHECK (h.slot.wasAutoBypassed());
    CHECK (h.slot.getLatencySamples() == 0);

    h.slot.clearAutoBypass();
    CHECK (h.slot.getLatencySamples() == 96);
}

// Re-enable puts the plug-in back in the signal path, and its delay with it, so
// delay compensation has to make room for that delay again.
TEST_CASE ("re-enabling an auto-bypassed plugin brings its latency back", "[plugin][autobypass]")
{
    Harness h (64);
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());
    REQUIRE (h.slot.getLatencySamples() == 0);

    h.slot.clearAutoBypass();
    CHECK (h.slot.getLatencySamples() == 64);
}

// A look-ahead limiter whose look-ahead is raised in its editor while it sits
// out reports a longer latency. Re-enable has to put it back in the path with
// that latency, not the one it had when it was bypassed.
TEST_CASE ("a latency a plugin changes while it is auto-bypassed is reported after Re-enable",
           "[plugin][autobypass]")
{
    Harness h (64);
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());

    h.plugin->setLatencySamples (192);
    CHECK (h.slot.getLatencySamples() == 0);

    h.slot.clearAutoBypass();
    CHECK (h.slot.getLatencySamples() == 192);
}

// The same with a real audio thread, and a plug-in that reports its latency from
// inside the blocks it runs, as JUCE's LV2 host does: the blocks that carry the
// longer look-ahead are the ones that overrun. Re-enable reads the latency on the
// message thread after the audio thread wrote it in the plug-in's last block, so
// under ThreadSanitizer this case shows the read is ordered after that block.
// The grace is used up first, so exactly kOverrunsToTrip blocks run on the audio
// thread before the bypass, and the last of them reports kLatencyFrom + 3.
TEST_CASE ("Re-enable reads a latency the plugin reported from its last blocks on the audio thread",
           "[plugin][autobypass]")
{
    using namespace std::chrono_literals;
    constexpr int kLatencyFrom = 192;

    Harness h (64);
    h.warmUp();
    h.plugin->nextLatencyInProcess = kLatencyFrom;
    h.plugin->blockMs = 3;

    std::atomic<bool> stop { false };
    std::thread audioThread ([&h, &stop]
    {
        float left[kBlock] {};
        float right[kBlock] {};
        juce::MidiBuffer midi;
        while (! stop.load (std::memory_order_relaxed))
        {
            h.slot.processStereoBlock (left, right, kBlock, midi);
            std::this_thread::yield();
        }
    });
    const auto waitFor = [] (auto condition)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (! condition() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (1ms);
        return condition();
    };

    const bool tripped = waitFor ([&h] { return h.slot.wasAutoBypassed(); });
    int reported = -1;
    if (tripped)
    {
        CHECK (h.slot.getLatencySamples() == 0);
        h.plugin->blockMs = 0;
        h.slot.clearAutoBypass();
        reported = h.slot.getLatencySamples();
        const int calls = h.plugin->processCalls;
        CHECK (waitFor ([&h, calls] { return h.plugin->processCalls > calls + 2; }));
    }
    stop.store (true, std::memory_order_relaxed);
    audioThread.join();

    REQUIRE (tripped);
    CHECK (reported == kLatencyFrom + kOverrunsToTrip - 1);
    CHECK_FALSE (h.slot.wasAutoBypassed());
}

// A device change re-prepares a slot that is still bypassed. It still passes
// dry, so it still adds no delay; the latency the prepare settled on comes back
// only when the plug-in does.
TEST_CASE ("preparing an auto-bypassed slot again keeps it reporting no latency", "[plugin][autobypass]")
{
    Harness h (64);
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());

    h.plugin->latencyOnPrepare = 128;
    h.slot.prepareToPlay (kRate, kBlock);
    CHECK (h.slot.wasAutoBypassed());
    CHECK (h.slot.getLatencySamples() == 0);

    h.slot.clearAutoBypass();
    CHECK (h.slot.getLatencySamples() == 128);
}

// The slot's report is what the engine feeds the PDC math for its track, every
// block and after every change it makes itself, so the compensation the other
// tracks get follows the plug-in out of the path and back into it.
TEST_CASE ("delay compensation follows a plugin in and out of auto-bypass", "[plugin][autobypass][pdc]")
{
    Harness h (64);
    const auto otherTrackDelay = [&h]
    {
        const int latency[2] { h.slot.getLatencySamples(), 0 };
        int compensation[2] {};
        pdc::computeCompensations (latency, compensation, 2);
        return compensation[1];
    };
    h.warmUp();
    CHECK (otherTrackDelay() == 64);

    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());
    CHECK (otherTrackDelay() == 0);

    h.plugin->latencyOnPrepare = 128;
    h.slot.prepareToPlay (kRate, kBlock);
    CHECK (otherTrackDelay() == 0);

    h.slot.clearAutoBypass();
    CHECK (otherTrackDelay() == 128);
}

// "Four consecutive blocks" holds after a re-enable as well: the late blocks
// that tripped the bypass do not count against the plug-in once it is back.
TEST_CASE ("a re-enabled plugin gets four late blocks again before it is bypassed", "[plugin][autobypass]")
{
    Harness h;
    h.warmUp();
    h.pump (kOverrunsToTrip, 3);
    REQUIRE (h.slot.wasAutoBypassed());

    h.slot.clearAutoBypass();
    h.pump (kOverrunsToTrip - 1, 3);
    CHECK_FALSE (h.slot.wasAutoBypassed());

    h.pump (1, 3);
    CHECK (h.slot.wasAutoBypassed());
}
