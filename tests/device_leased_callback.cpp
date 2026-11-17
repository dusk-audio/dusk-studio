#include <catch2/catch_test_macros.hpp>

#include "engine/device/LeasedCallback.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

// A device thread keeps running cycles while the message thread stops and
// starts the stream, as the PipeWire backend's does when the engine follows a
// graph that moved its quantum or rate. The cycle takes no lock, so stop() has
// to wait out a cycle in progress itself, and a restarted callback has to get
// blocks of the size it was started at.

using duskstudio::device::CallbackContext;
using duskstudio::device::ClockWatch;
using duskstudio::device::dispatchCycle;
using duskstudio::device::IODevice;
using duskstudio::device::IODeviceCallback;
using duskstudio::device::LeasedCallback;

namespace
{
// Stands in for the engine: what audioDeviceAboutToStart sets up,
// audioDeviceStopped takes down. Those two fields are plain on purpose: a block
// run after stop() returned races with the message thread's writes to them,
// which ThreadSanitizer reports even when the values happen to look right.
class EngineStandIn final : public IODeviceCallback
{
public:
    int nextBlockSize = 0;

    void audioDeviceAboutToStart (IODevice*) override
    {
        preparedBlock = nextBlockSize;
        running = true;
    }

    void audioDeviceStopped() override
    {
        running = false;
        preparedBlock = 0;
    }

    void audioDeviceIOCallback (const float* const*, int, float* const* out, int numOut,
                                int numSamples, const CallbackContext&) override
    {
        if (! running || numSamples != preparedBlock)
            wrongBlocks.fetch_add (1, std::memory_order_relaxed);
        for (int ch = 0; ch < numOut; ++ch)
            for (int i = 0; i < numSamples; ++i)
                out[ch][i] = 0.25f;
        blocks.fetch_add (1, std::memory_order_release);
    }

    std::atomic<std::int64_t> blocks { 0 };
    std::atomic<int> wrongBlocks { 0 };

private:
    int  preparedBlock = 0;
    bool running = false;
};

// PipeWireAudioIODevice::start() and stop() without the graph.
void startStream (LeasedCallback& callback, ClockWatch& clock, EngineStandIn& engine, int blockSize)
{
    clock.prepare (blockSize, 48000);
    engine.nextBlockSize = blockSize;
    engine.audioDeviceAboutToStart (nullptr);
    callback.attach (&engine);
}

void stopStream (LeasedCallback& callback)
{
    if (auto* cb = callback.detach())
        cb->audioDeviceStopped();
}

// Polls without sleeping; false only when the other thread never got there.
template <class Done>
bool pollUntil (Done&& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (! done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

constexpr int kCycle = 512;

// One device cycle of kCycle samples, one input and two outputs.
struct CycleBuffers
{
    std::vector<float> input = std::vector<float> (kCycle, 0.0f);
    std::vector<float> left  = std::vector<float> (kCycle, 0.0f);
    std::vector<float> right = std::vector<float> (kCycle, 0.0f);

    bool run (LeasedCallback& callback, const ClockWatch& clock)
    {
        const float* in[] = { input.data() };
        float* out[] = { left.data(), right.data() };
        return dispatchCycle (callback, clock, in, 1, out, 2, kCycle, {});
    }
};
} // namespace

TEST_CASE ("A device cycle runs the attached callback and none once it is detached", "[audio][device]")
{
    LeasedCallback callback;
    ClockWatch clock;
    EngineStandIn engine;
    CycleBuffers buffers;

    SECTION ("detach")
    {
        REQUIRE_FALSE (buffers.run (callback, clock));

        startStream (callback, clock, engine, 128);
        REQUIRE (buffers.run (callback, clock));
        REQUIRE (engine.blocks.load() == kCycle / 128);
        REQUIRE (engine.wrongBlocks.load() == 0);

        REQUIRE (callback.detach() == &engine);
        REQUIRE_FALSE (buffers.run (callback, clock));
        REQUIRE (engine.blocks.load() == kCycle / 128);
        REQUIRE (callback.detach() == nullptr);
    }

    SECTION ("detachIfIdle with no cycle running")
    {
        callback.attach (&engine);
        REQUIRE (callback.detachIfIdle() == &engine);
        REQUIRE_FALSE (buffers.run (callback, clock));
    }

    SECTION ("detachIfIdle while a cycle holds the callback")
    {
        callback.attach (&engine);
        {
            const LeasedCallback::Lease lease (callback);
            REQUIRE (lease.get() == &engine);
            REQUIRE (callback.detachIfIdle() == nullptr);
        }
        const LeasedCallback::Lease next (callback);
        REQUIRE (next.get() == nullptr);
    }
}

TEST_CASE ("Stopping a stream waits for the cycle that is running its callback", "[audio][device]")
{
    LeasedCallback callback;
    EngineStandIn engine;
    callback.attach (&engine);

    std::atomic<bool> entered { false }, letGo { false }, detaching { false }, detached { false };
    bool cycleRanCallback = false;   // device thread, inside its cycle
    bool cycleDone = false;          // device thread, before its lease ends
    bool cycleDoneAtDetach = false;  // stopper, once detach() returned

    std::thread device ([&]
    {
        const LeasedCallback::Lease lease (callback);
        cycleRanCallback = lease.get() == &engine;
        entered.store (true, std::memory_order_release);
        while (! letGo.load (std::memory_order_acquire))
            std::this_thread::yield();
        cycleDone = true;
    });

    pollUntil ([&] { return entered.load (std::memory_order_acquire); });

    std::thread stopper ([&]
    {
        detaching.store (true, std::memory_order_release);
        auto* cb = callback.detach();
        cycleDoneAtDetach = cycleDone && cb == &engine;
        detached.store (true, std::memory_order_release);
    });

    const bool reachedDetach = pollUntil ([&] { return detaching.load (std::memory_order_acquire); });
    for (int i = 0; i < 2000; ++i)
        std::this_thread::yield();
    const bool returnedEarly = detached.load (std::memory_order_acquire);

    letGo.store (true, std::memory_order_release);
    device.join();
    stopper.join();

    REQUIRE (cycleRanCallback);
    REQUIRE (reachedDetach);
    REQUIRE_FALSE (returnedEarly);
    REQUIRE (cycleDoneAtDetach);
}

TEST_CASE ("A stream stopped and started while its device thread runs cycles never runs a stopped callback",
           "[audio][device][pipewire]")
{
    // A cycle splits evenly into each of these, so every block the callback
    // gets is exactly the size it was started at, or the wrong one.
    static constexpr std::array<int, 4> kBlockSizes { 512, 64, 256, 128 };
    static constexpr int kRestarts = 2000;

    LeasedCallback callback;
    ClockWatch clock;
    EngineStandIn engine;
    std::atomic<bool> quit { false };
    std::atomic<std::int64_t> cycles { 0 };

    std::thread device ([&]
    {
        CycleBuffers buffers;
        while (! quit.load (std::memory_order_acquire))
        {
            buffers.run (callback, clock);
            cycles.fetch_add (1, std::memory_order_release);
        }
    });

    int lateBlocks = 0, stalls = 0;
    for (int r = 0; r < kRestarts && stalls == 0; ++r)
    {
        const auto blocksBefore = engine.blocks.load (std::memory_order_acquire);
        startStream (callback, clock, engine, kBlockSizes[(std::size_t) r % kBlockSizes.size()]);

        // Every other restart stops at once, as a stop and start in quick
        // succession do; the rest stop while the callback is being run.
        if (r % 2 == 1
            && ! pollUntil ([&] { return engine.blocks.load (std::memory_order_acquire) > blocksBefore; }))
            ++stalls;

        stopStream (callback);

        // The cycle in progress ends, then one starts after stop() returned:
        // neither may have run the callback.
        const auto blocksAtStop = engine.blocks.load (std::memory_order_acquire);
        const auto cyclesAtStop = cycles.load (std::memory_order_acquire);
        if (! pollUntil ([&] { return cycles.load (std::memory_order_acquire) >= cyclesAtStop + 2; }))
            ++stalls;
        if (engine.blocks.load (std::memory_order_acquire) != blocksAtStop)
            ++lateBlocks;
    }

    quit.store (true, std::memory_order_release);
    device.join();

    REQUIRE (stalls == 0);
    REQUIRE (lateBlocks == 0);
    REQUIRE (engine.wrongBlocks.load() == 0);
    REQUIRE (engine.blocks.load() >= kRestarts / 2);
}
