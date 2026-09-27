#include <catch2/catch_test_macros.hpp>

#include "engine/PdcMath.h"
#include "engine/PluginManager.h"
#include "engine/PluginSlot.h"
#include "foundation/MessageThread.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

using namespace duskstudio;

namespace
{
constexpr double kRate  = 48000.0;
constexpr int    kBlock = 64;

// A look-ahead limiter in miniature: its latency is whatever it was last told,
// from the editor on the message thread or from inside processBlock, where
// JUCE's LV2 host sets it after every run.
class LookAheadPluginInstance final : public juce::AudioPluginInstance
{
public:
    using juce::AudioPluginInstance::processBlock;

    LookAheadPluginInstance()
        : AudioPluginInstance (BusesProperties()
            .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Look-ahead test"; }

    void prepareToPlay (double, int) override { setLatencySamples (latencyOnPrepare); }
    void releaseResources() override          {}

    // Each armed block moves the latency on by one sample until the steps run
    // out, so the plug-in writes it from the audio thread block after block.
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        ++processCalls;
        if (latencySteps.load (std::memory_order_relaxed) > 0)
        {
            setLatencySamples (getLatencySamples() + 1);
            latencySteps.fetch_sub (1, std::memory_order_relaxed);
        }
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
        description.fileOrIdentifier = "look-ahead-test";
    }

    std::atomic<int> latencySteps { 0 };
    std::atomic<int> processCalls { 0 };
    int latencyOnPrepare = 0;
};

struct Harness
{
    PluginManager manager;
    PluginSlot slot;
    LookAheadPluginInstance* plugin = nullptr;

    explicit Harness (int latency, int blockSize = kBlock)
    {
        slot.setManager (manager);
        slot.prepareToPlay (kRate, blockSize);
        auto instance = std::make_unique<LookAheadPluginInstance>();
        plugin = instance.get();
        plugin->latencyOnPrepare = latency;
        REQUIRE (slot.installInProcessInstanceForTest (std::move (instance)));
    }

    void pump (int blocks)
    {
        float left[kBlock] {};
        float right[kBlock] {};
        juce::MidiBuffer midi;
        for (int i = 0; i < blocks; ++i)
            slot.processStereoBlock (left, right, kBlock, midi);
    }

    // What the engine's per-block PDC pass gives a zero-latency track next to
    // this one.
    int otherTrackDelay() const
    {
        const int latency[2] { slot.getLatencySamples(), 0 };
        int compensation[2] {};
        pdc::computeCompensations (latency, compensation, 2);
        return compensation[1];
    }
};

// Ends the process when the scope is still open at the deadline. A call that
// spins on a lock its own thread holds never returns, and a hung case would
// otherwise only fail when something outside the run gives up on it. It polls
// rather than making a timed condition-variable wait: GCC 11's TSan does not
// intercept the pthread_cond_clockwait that wait_for uses, misses the unlock
// inside it, and reports the next lock of that mutex as a double lock.
class HangWatchdog
{
public:
    HangWatchdog (std::chrono::seconds limit, const char* what)
        : watcher ([this, limit, what]
          {
              const auto deadline = std::chrono::steady_clock::now() + limit;
              while (! done.load (std::memory_order_acquire))
              {
                  if (std::chrono::steady_clock::now() >= deadline)
                  {
                      std::fprintf (stderr, "FAIL: %s did not return within %lld s\n",
                                    what, (long long) limit.count());
                      std::_Exit (EXIT_FAILURE);
                  }
                  std::this_thread::sleep_for (std::chrono::milliseconds (5));
              }
          })
    {
    }

    ~HangWatchdog()
    {
        done.store (true, std::memory_order_release);
        watcher.join();
    }

private:
    std::atomic<bool> done { false };
    std::thread watcher;
};

#if ! defined (__APPLE__)
// Runs the dispatch loop until `done` holds or the deadline passes. This JUCE
// build has no runDispatchLoopUntil, and stopDispatchLoop latches the quit flag
// for the life of the MessageManager, so a test gets exactly one pump.
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

#if ! defined (__APPLE__)
// In the running app nothing calls refreshLatencyIfChanged but the slot's own
// 30 Hz timer, so that timer is what carries a plug-in's announcement to PDC.
TEST_CASE ("the slot's own timer re-reads a latency the plugin announces",
           "[plugin][latency][pdc][issue-764]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Harness h (64);
    h.plugin->setLatencySamples (320);
    REQUIRE (h.slot.getLatencySamples() == 64);

    pumpUntil ([&h] { return h.slot.getLatencySamples() == 320; }, std::chrono::seconds (5));

    CHECK (h.slot.getLatencySamples() == 320);
    CHECK (h.otherTrackDelay() == 320);
}
#endif

// Raising a look-ahead in the plug-in's editor changes its latency on the
// message thread. Delay compensation has to follow while the plug-in runs,
// not wait for the next prepare or save.
TEST_CASE ("a latency a plugin changes on the message thread reaches the slot and PDC",
           "[plugin][latency][pdc][issue-764]")
{
    Harness h (64);
    h.pump (4);
    REQUIRE (h.slot.getLatencySamples() == 64);
    REQUIRE_FALSE (h.slot.refreshLatencyIfChanged());

    h.plugin->setLatencySamples (192);
    CHECK (h.slot.refreshLatencyIfChanged());
    CHECK (h.slot.getLatencySamples() == 192);
    CHECK (h.otherTrackDelay() == 192);

    // Nothing announced since, so nothing to re-read.
    CHECK_FALSE (h.slot.refreshLatencyIfChanged());

    h.plugin->setLatencySamples (32);
    CHECK (h.slot.refreshLatencyIfChanged());
    CHECK (h.slot.getLatencySamples() == 32);
    CHECK (h.otherTrackDelay() == 32);
}

// The message thread holds the slot's process lock while a plug-in builds its
// editor, and an editor that opens a licence dialog from its constructor runs a
// nested message loop in which the slot's timer still fires. The re-read has
// to come back without the lock, keep the change for a later tick, and apply
// it once the lock is free.
TEST_CASE ("a latency re-read under a process lock its own thread holds stays pending",
           "[plugin][latency][pdc][issue-768]")
{
    Harness h (64);
    h.pump (4);
    h.plugin->setLatencySamples (192);

    {
        const HangWatchdog watchdog (std::chrono::seconds (10),
                                     "refreshLatencyIfChanged under a held process lock");
        const juce::SpinLock::ScopedLockType heldByThisThread (h.slot.getProcessLock());
        CHECK_FALSE (h.slot.refreshLatencyIfChanged());
        CHECK_FALSE (h.slot.refreshLatencyIfChanged());
        CHECK (h.slot.getLatencySamples() == 64);
        CHECK (h.otherTrackDelay() == 64);
    }

    CHECK (h.slot.refreshLatencyIfChanged());
    CHECK (h.slot.getLatencySamples() == 192);
    CHECK (h.otherTrackDelay() == 192);
    CHECK_FALSE (h.slot.refreshLatencyIfChanged());
}

// JUCE's LV2 host sets the latency from inside processBlock, so the
// announcement arrives on the audio thread.
TEST_CASE ("a latency a plugin changes inside processBlock reaches the slot and PDC",
           "[plugin][latency][pdc][issue-764]")
{
    Harness h (64);
    h.plugin->latencySteps = 3;
    h.pump (5);
    REQUIRE (h.plugin->getLatencySamples() == 67);

    CHECK (h.slot.refreshLatencyIfChanged());
    CHECK (h.slot.getLatencySamples() == 67);
    CHECK (h.otherTrackDelay() == 67);
}

// A bypassed plug-in passes dry, so a latency it changes meanwhile stays out of
// PDC until it is back in the path, and is the one it has then.
TEST_CASE ("a latency a bypassed plugin changes reaches PDC when it is back in the path",
           "[plugin][latency][pdc][issue-764]")
{
    Harness h (64);
    h.slot.setBypassed (true);
    h.plugin->setLatencySamples (256);
    (void) h.slot.refreshLatencyIfChanged();
    CHECK (h.slot.getLatencySamples() == 0);
    CHECK (h.otherTrackDelay() == 0);

    h.slot.setBypassed (false);
    CHECK (h.slot.getLatencySamples() == 256);
    CHECK (h.otherTrackDelay() == 256);
}

// The slot lets go of an unloaded plug-in's announcements along with the
// plug-in: nothing it reports afterwards reaches the slot.
TEST_CASE ("an unloaded plugin's latency changes do not reach the slot",
           "[plugin][latency][issue-764]")
{
    Harness h (64);
    auto* deposed = h.plugin;
    h.slot.unload();
    REQUIRE (h.slot.getLatencySamples() == 0);

    // The unload keeps the instance alive in the slot's keep-alive ring.
    deposed->setLatencySamples (512);
    CHECK_FALSE (h.slot.refreshLatencyIfChanged());
    CHECK (h.slot.getLatencySamples() == 0);
}

// The same with a real audio thread that keeps moving the latency while the
// message thread follows it. Under ThreadSanitizer this shows the message
// thread's read of the plug-in's latency is ordered against the blocks that
// write it.
//
// The blocks are large because the slot's time-budget watchdog measures each
// block against its own length: at 64 samples a few preempted blocks (under
// ThreadSanitizer or a parallel ctest) read as a plug-in overrunning, and the
// slot bypasses it before the latency has finished moving.
TEST_CASE ("the slot follows a latency the plugin keeps changing on the audio thread",
           "[plugin][latency][pdc][issue-764]")
{
    using namespace std::chrono_literals;
    static constexpr int kSteps = 200;
    static constexpr int kLargeBlock = 4096;

    Harness h (64, kLargeBlock);
    h.plugin->latencySteps = kSteps;

    std::atomic<bool> stop { false };
    std::thread audioThread ([&h, &stop]
    {
        std::vector<float> left ((size_t) kLargeBlock, 0.0f);
        std::vector<float> right ((size_t) kLargeBlock, 0.0f);
        juce::MidiBuffer midi;
        while (! stop.load (std::memory_order_relaxed))
        {
            h.slot.processStereoBlock (left.data(), right.data(), kLargeBlock, midi);
            std::this_thread::yield();
        }
    });

    int refreshes = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (h.slot.getLatencySamples() != 64 + kSteps
           && std::chrono::steady_clock::now() < deadline)
    {
        if (h.slot.refreshLatencyIfChanged())
            ++refreshes;
        std::this_thread::yield();
    }
    stop.store (true, std::memory_order_relaxed);
    audioThread.join();

    CHECK (h.slot.getLatencySamples() == 64 + kSteps);
    CHECK (h.otherTrackDelay() == 64 + kSteps);
    CHECK (refreshes > 0);
}
