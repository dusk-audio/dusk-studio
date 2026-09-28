// LV2 Worker host: the length-prefixed rings, the realtime / synchronous /
// offline paths of Lv2Worker against a fake interface, and the whole round trip
// through Lv2Instance with the worker fixture (tests/fixtures/worker_lv2.cpp),
// whose output is its input times a gain only the worker produces.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "engine/hosting/PortBuffers.h"
#include "engine/lv2/Lv2Bundle.h"
#include "engine/lv2/Lv2Instance.h"
#include "engine/lv2/Lv2UridMap.h"
#include "engine/lv2/Lv2Worker.h"
#include "engine/lv2/NativeLv2Slot.h"

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr const char* kWorkerUri      = "urn:duskstudio:test:worker";
constexpr const char* kUnsupportedUri = "urn:duskstudio:test:worker-unsupported";
constexpr const char* kHardRtUri      = "urn:duskstudio:test:worker-hard-rt";
constexpr uint32_t    kTargetPort     = 4;
constexpr uint32_t    kDelayPort      = 5;
constexpr int         kBlock          = 256;
constexpr float       kInput          = 0.5f;

LV2_Worker_Status schedule (duskstudio::lv2::Lv2Worker& worker, const void* data, uint32_t size)
{
    const auto* sched = static_cast<const LV2_Worker_Schedule*> (worker.scheduleFeature()->data);
    return sched->schedule_work (sched->handle, size, data);
}

// A plugin stand-in: work() replies with what it was sent, with a burst of
// replies for kBurst, and with nothing for a request starting with kSilent; it
// can be held inside work() until the test lets it go.
struct FakePlugin
{
    static constexpr uint8_t kHold   = 0xAA;
    static constexpr uint8_t kBurst  = 0xBB;
    static constexpr uint8_t kSilent = 0xCC;
    static constexpr uint32_t kReplyBytes = 1024;

    std::mutex lock;
    std::condition_variable changed;
    bool holding = false, released = false, holdReturned = false;
    int burstReplies = 0;
    LV2_Worker_Status burstEnd = LV2_WORKER_SUCCESS;
    bool burstDone = false;

    std::thread::id lastWorkThread;
    std::map<uint8_t, std::thread::id> oneByteWorkThread;   // the thread work() ran each 1-byte request on
    std::map<uint8_t, bool> oneByteReplied;                 // its reply has been handed to respond
    std::vector<std::vector<uint8_t>> responses;   // audio / calling thread
    int endRuns = 0;

    // Polls rather than a timed condition-variable wait: GCC 11's TSan does not
    // intercept the pthread_cond_clockwait behind wait_for, misses the unlock
    // inside it, and reports the worker's next lock of this mutex as a double lock.
    template <typename Pred>
    bool waitFor (Pred pred)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
        for (;;)
        {
            {
                std::lock_guard<std::mutex> l (lock);
                if (pred()) return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }

    static LV2_Worker_Status work (LV2_Handle h, LV2_Worker_Respond_Function respond,
                                   LV2_Worker_Respond_Handle rh, uint32_t size, const void* data)
    {
        auto& self = *static_cast<FakePlugin*> (h);
        const auto* bytes = static_cast<const uint8_t*> (data);
        {
            std::lock_guard<std::mutex> l (self.lock);
            self.lastWorkThread = std::this_thread::get_id();
            if (size == 1)
                self.oneByteWorkThread[bytes[0]] = self.lastWorkThread;
            self.changed.notify_all();
        }
        if (size > 1 && bytes[0] == kSilent)
            return LV2_WORKER_SUCCESS;
        if (size == 1 && bytes[0] == kHold)
        {
            std::unique_lock<std::mutex> l (self.lock);
            self.holding = true;
            self.changed.notify_all();
            self.changed.wait (l, [&] { return self.released; });
            self.holdReturned = true;
            return LV2_WORKER_SUCCESS;
        }
        if (size == 1 && bytes[0] == kBurst)
        {
            std::vector<uint8_t> reply (kReplyBytes, 0x5A);
            int sent = 0;
            LV2_Worker_Status status = LV2_WORKER_SUCCESS;
            while ((status = respond (rh, kReplyBytes, reply.data())) == LV2_WORKER_SUCCESS)
                ++sent;
            std::lock_guard<std::mutex> l (self.lock);
            self.burstReplies = sent;
            self.burstEnd = status;
            self.burstDone = true;
            self.changed.notify_all();
            return LV2_WORKER_SUCCESS;
        }
        const auto status = respond (rh, size, data);
        if (size == 1)
        {
            std::lock_guard<std::mutex> l (self.lock);
            self.oneByteReplied[bytes[0]] = true;
            self.changed.notify_all();
        }
        return status;
    }

    bool replied (uint8_t request)
    {
        return waitFor ([&] { return oneByteReplied.count (request) != 0; });
    }

    std::thread::id workThreadFor (uint8_t request)
    {
        std::lock_guard<std::mutex> l (lock);
        const auto it = oneByteWorkThread.find (request);
        return it != oneByteWorkThread.end() ? it->second : std::thread::id();
    }

    static LV2_Worker_Status workResponse (LV2_Handle h, uint32_t size, const void* data)
    {
        auto& self = *static_cast<FakePlugin*> (h);
        const auto* bytes = static_cast<const uint8_t*> (data);
        self.responses.emplace_back (bytes, bytes + size);
        return LV2_WORKER_SUCCESS;
    }

    static LV2_Worker_Status endRun (LV2_Handle h)
    {
        ++static_cast<FakePlugin*> (h)->endRuns;
        return LV2_WORKER_SUCCESS;
    }

    void release()
    {
        std::lock_guard<std::mutex> l (lock);
        released = true;
        changed.notify_all();
    }
};

const LV2_Worker_Interface kFakeInterface { &FakePlugin::work, &FakePlugin::workResponse,
                                            &FakePlugin::endRun };

// Holds kInput on both inputs and runs one block of `instance`.
struct Block
{
    std::array<float, kBlock> inL {}, inR {}, outL {}, outR {};
    std::array<float*, 2> ins { inL.data(), inR.data() };
    std::array<float*, 2> outs { outL.data(), outR.data() };

    Block() { inL.fill (kInput); inR.fill (kInput); }

    float run (duskstudio::lv2::Lv2Instance& instance, bool offline)
    {
        duskstudio::hosting::PortBuffers io;
        io.mainIn = ins.data();
        io.mainInChannels = 2;
        io.mainOut = outs.data();
        io.mainOutChannels = 2;
        io.numFrames = kBlock;
        io.offlineRender = offline;
        instance.processBlock (io);
        return outL[kBlock - 1];
    }
};

struct WorkerFixture
{
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;

    WorkerFixture() { REQUIRE (bundle.load (DUSKSTUDIO_WORKER_LV2_FIXTURE_PATH, error)); }

    void activate (duskstudio::lv2::Lv2Instance& instance, double rate = 48000.0)
    {
        REQUIRE (instance.create (bundle, kWorkerUri, error));
        INFO (error);
        REQUIRE (instance.activate (rate, kBlock, error));
    }
};
} // namespace

TEST_CASE ("LV2 worker ring keeps message order across the wrap and refuses what does not fit",
           "[lv2][worker]")
{
    duskstudio::lv2::WorkerMessageRing ring;
    ring.allocate (64);
    std::array<uint8_t, 64> scratch {};
    uint32_t size = 0;

    REQUIRE_FALSE (ring.read (scratch.data(), (uint32_t) scratch.size(), size));
    REQUIRE_FALSE (ring.write (scratch.data(), 61));   // 4-byte header + 61 > 64

    // Odd sizes walk the write position round the buffer several times, so
    // headers and payloads both land split across the end.
    uint8_t next = 0, expect = 0;
    for (int round = 0; round < 40; ++round)
    {
        const uint32_t n = 1 + (uint32_t) (round * 7) % 13;
        std::array<uint8_t, 16> message {};
        for (uint32_t i = 0; i < n; ++i) message[i] = next++;
        REQUIRE (ring.write (message.data(), n));
        REQUIRE (ring.read (scratch.data(), (uint32_t) scratch.size(), size));
        REQUIRE (size == n);
        for (uint32_t i = 0; i < n; ++i) REQUIRE (scratch[i] == expect++);
    }

    // Full: 7 messages of 4 + 5 bytes fit in 64, the 8th does not.
    std::array<uint8_t, 5> five {};
    int written = 0;
    while (ring.write (five.data(), (uint32_t) five.size())) ++written;
    REQUIRE (written == 7);
    int drained = 0;
    ring.drain (scratch.data(), (uint32_t) scratch.size(),
                [&] (const uint8_t*, uint32_t n) { REQUIRE (n == 5); ++drained; });
    REQUIRE (drained == 7);
    REQUIRE (ring.write (five.data(), (uint32_t) five.size()));
}

TEST_CASE ("LV2 worker returns NO_SPACE on a full ring without blocking", "[lv2][worker]")
{
    FakePlugin plugin;
    duskstudio::lv2::Lv2Worker worker;
    std::string error;
    REQUIRE (worker.attach (&plugin, &kFakeInterface, error));
    REQUIRE (worker.isRunning());

    SECTION ("requests: the thread is held inside work() while run() keeps scheduling")
    {
        worker.beginRun();
        REQUIRE (schedule (worker, &FakePlugin::kHold, 1) == LV2_WORKER_SUCCESS);
        worker.finishRun (false);
        REQUIRE (plugin.waitFor ([&] { return plugin.holding; }));

        // The ring is empty again (the held request was taken off it), so
        // exactly capacity / (header + payload) requests fit.
        std::vector<uint8_t> request (1024, 0x11);
        const int fits = (int) (duskstudio::lv2::Lv2Worker::kRingBytes / (4 + request.size()));
        worker.beginRun();
        int accepted = 0;
        LV2_Worker_Status status = LV2_WORKER_SUCCESS;
        while ((status = schedule (worker, request.data(), (uint32_t) request.size()))
               == LV2_WORKER_SUCCESS)
            ++accepted;
        worker.finishRun (false);
        CHECK (status == LV2_WORKER_ERR_NO_SPACE);
        CHECK (accepted == fits);
        plugin.release();
    }

    SECTION ("replies: work() responds while the audio thread drains nothing")
    {
        // Queue the burst behind a held request, so no block can drain a reply
        // while the burst is still filling the ring.
        worker.beginRun();
        REQUIRE (schedule (worker, &FakePlugin::kHold, 1) == LV2_WORKER_SUCCESS);
        worker.finishRun (false);
        REQUIRE (plugin.waitFor ([&] { return plugin.holding; }));
        worker.beginRun();
        REQUIRE (schedule (worker, &FakePlugin::kBurst, 1) == LV2_WORKER_SUCCESS);
        worker.finishRun (false);
        plugin.release();
        REQUIRE (plugin.waitFor ([&] { return plugin.burstDone; }));
        CHECK (plugin.burstEnd == LV2_WORKER_ERR_NO_SPACE);
        CHECK (plugin.burstReplies
               == (int) (duskstudio::lv2::Lv2Worker::kRingBytes / (4 + FakePlugin::kReplyBytes)));
        CHECK (plugin.responses.empty());

        // The next block hands every queued reply over, then calls end_run once.
        worker.beginRun();
        worker.finishRun (false);
        CHECK ((int) plugin.responses.size() == plugin.burstReplies);
        CHECK (plugin.endRuns == 3);
    }

    worker.detach();
    REQUIRE_FALSE (worker.isRunning());
}

TEST_CASE ("LV2 worker runs work synchronously inside a host call, replies included",
           "[lv2][worker]")
{
    FakePlugin plugin;
    duskstudio::lv2::Lv2Worker worker;
    std::string error;

    SECTION ("scheduled during instantiate: held until the handle exists")
    {
        const duskstudio::lv2::Lv2Worker::HostCallScope hostCall (worker);
        worker.beginInstantiate();
        const uint8_t early[] = { 1, 2, 3 };
        REQUIRE (schedule (worker, early, sizeof (early)) == LV2_WORKER_SUCCESS);
        REQUIRE (plugin.responses.empty());
        REQUIRE (worker.attach (&plugin, &kFakeInterface, error));
        REQUIRE (plugin.responses.size() == 1);
        CHECK (plugin.responses[0] == std::vector<uint8_t> { 1, 2, 3 });
        CHECK (plugin.lastWorkThread == std::this_thread::get_id());
    }

    SECTION ("scheduled from the message thread with the worker running")
    {
        REQUIRE (worker.attach (&plugin, &kFakeInterface, error));
        const duskstudio::lv2::Lv2Worker::HostCallScope hostCall (worker);
        const uint8_t now[] = { 7 };
        REQUIRE (schedule (worker, now, sizeof (now)) == LV2_WORKER_SUCCESS);
        REQUIRE (plugin.responses.size() == 1);
        CHECK (plugin.responses[0] == std::vector<uint8_t> { 7 });
        CHECK (plugin.lastWorkThread == std::this_thread::get_id());
        CHECK (plugin.endRuns == 0);   // no run cycle, so no end_run
    }

    SECTION ("outside a host call it goes to the worker thread, the reply to the next run")
    {
        REQUIRE (worker.attach (&plugin, &kFakeInterface, error));
        const uint8_t later[] = { 9 };
        REQUIRE (schedule (worker, later, sizeof (later)) == LV2_WORKER_SUCCESS);
        REQUIRE (plugin.replied (9));
        CHECK (plugin.workThreadFor (9) != std::this_thread::get_id());
        CHECK (plugin.responses.empty());
        worker.beginRun();
        worker.finishRun (false);
        REQUIRE (plugin.responses.size() == 1);
        CHECK (plugin.responses[0] == std::vector<uint8_t> { 9 });
    }

    SECTION ("a plugin without the interface gets an error, and no thread starts")
    {
        REQUIRE (worker.attach (&plugin, nullptr, error));
        CHECK_FALSE (worker.isRunning());
        const uint8_t any[] = { 0 };
        CHECK (schedule (worker, any, sizeof (any)) == LV2_WORKER_ERR_UNKNOWN);
        worker.beginRun();
        CHECK (schedule (worker, any, sizeof (any)) == LV2_WORKER_ERR_UNKNOWN);
        worker.finishRun (true);
    }
    worker.detach();
}

TEST_CASE ("LV2 worker fixture instantiates and its work reaches the output in real time",
           "[lv2][worker][ipc]")
{
    // [ipc]: paced at the real block rate against a live thread, so it runs
    // alone. The bound is generous; the worker usually answers inside a block.
    WorkerFixture fixture;
    duskstudio::lv2::Lv2Instance instance;
    fixture.activate (instance);

    Block block;
    REQUIRE_THAT (block.run (instance, false), WithinAbs (0.0f, 0.0f));   // run() only asked for the gain

    constexpr int kMaxBlocks = 400;   // ~2 s at 48 kHz / 256
    const auto period = std::chrono::microseconds ((int) (1.0e6 * kBlock / 48000.0));
    auto deadline = std::chrono::steady_clock::now();
    int blocks = 0;
    float out = 0.0f;
    while (blocks < kMaxBlocks && std::abs (out) < 1.0e-6f)
    {
        deadline += period;
        std::this_thread::sleep_until (deadline);
        out = block.run (instance, false);
        ++blocks;
    }
    INFO ("blocks until the worker's gain arrived: " << blocks);
    REQUIRE (blocks < kMaxBlocks);
    REQUIRE_THAT (out, WithinAbs (kInput * 0.5f, 1.0e-6f));
    REQUIRE_THAT (block.outR[0], WithinAbs (kInput * 0.5f, 1.0e-6f));
}

TEST_CASE ("LV2 worker fixture renders offline the same way every time", "[lv2][worker]")
{
    WorkerFixture fixture;
    auto render = [&]
    {
        duskstudio::lv2::Lv2Instance instance;
        fixture.activate (instance);
        Block block;
        std::vector<float> firstSamples;
        for (int b = 0; b < 12; ++b)
        {
            if (b == 5)
                instance.setControlPortValue (kTargetPort, 80.0f);
            if (b == 8)
            {
                // A slow request must still land in the block that asked for it.
                instance.setControlPortValue (kDelayPort, 30.0f);
                instance.setControlPortValue (kTargetPort, 20.0f);
            }
            block.run (instance, true);
            firstSamples.push_back (block.outL[0]);
        }
        return firstSamples;
    };

    const auto first = render();
    const auto second = render();
    REQUIRE (first.size() == second.size());
    REQUIRE (std::memcmp (first.data(), second.data(), first.size() * sizeof (float)) == 0);

    // The gain asked for in block n applies from block n + 1.
    CHECK_THAT (first[0], WithinAbs (0.0f, 0.0f));
    CHECK_THAT (first[1], WithinAbs (kInput * 0.5f, 1.0e-6f));
    CHECK_THAT (first[5], WithinAbs (kInput * 0.5f, 1.0e-6f));
    CHECK_THAT (first[6], WithinAbs (kInput * 0.8f, 1.0e-6f));
    CHECK_THAT (first[8], WithinAbs (kInput * 0.8f, 1.0e-6f));
    CHECK_THAT (first[9], WithinAbs (kInput * 0.2f, 1.0e-6f));
}

TEST_CASE ("LV2 worker fixture restores through the worker synchronously", "[lv2][worker][state]")
{
    WorkerFixture fixture;
    Block block;

    duskstudio::lv2::Lv2Instance source;
    fixture.activate (source);
    source.setControlPortValue (kTargetPort, 80.0f);
    block.run (source, true);
    REQUIRE_THAT (block.run (source, true), WithinAbs (kInput * 0.8f, 1.0e-6f));
    std::vector<uint8_t> blob;
    REQUIRE (source.saveState (blob));

    duskstudio::lv2::Lv2Instance restored;
    fixture.activate (restored);
    REQUIRE (restored.loadState (blob));
    // Asynchronous work would still be queued here; the first block after a
    // synchronous restore already plays the restored gain.
    CHECK_THAT (block.run (restored, false), WithinAbs (kInput * 0.8f, 1.0e-6f));

    SECTION ("reactivation carries it across and brings the worker back")
    {
        std::string error;
        REQUIRE (restored.reactivate (44100.0, kBlock, error));
        CHECK_THAT (block.run (restored, true), WithinAbs (kInput * 0.8f, 1.0e-6f));
        restored.setControlPortValue (kTargetPort, 30.0f);
        block.run (restored, true);
        CHECK_THAT (block.run (restored, true), WithinAbs (kInput * 0.3f, 1.0e-6f));
    }
}

TEST_CASE ("LV2 worker stop waits for the work() in flight", "[lv2][worker]")
{
    FakePlugin plugin;
    duskstudio::lv2::Lv2Worker worker;
    std::string error;
    REQUIRE (worker.attach (&plugin, &kFakeInterface, error));
    worker.beginRun();
    REQUIRE (schedule (worker, &FakePlugin::kHold, 1) == LV2_WORKER_SUCCESS);
    worker.finishRun (false);
    REQUIRE (plugin.waitFor ([&] { return plugin.holding; }));

    // A stop that let the plugin go while work() still ran would be back
    // before the release; the join cannot be.
    auto stopped = std::async (std::launch::async, [&] { worker.stop(); });
    CHECK (stopped.wait_for (std::chrono::milliseconds (50)) == std::future_status::timeout);
    plugin.release();
    stopped.get();
    CHECK (plugin.holdReturned);
    CHECK_FALSE (worker.isRunning());
    worker.detach();
}

TEST_CASE ("LV2 worker offline wait ends when the render is cancelled", "[lv2][worker]")
{
    FakePlugin plugin;
    duskstudio::lv2::Lv2Worker worker;
    std::string error;
    REQUIRE (worker.attach (&plugin, &kFakeInterface, error));

    // The render thread asks for work that never finishes by itself, then
    // waits for it the way an offline block does.
    std::atomic<bool> cancelled { false };
    auto block = std::async (std::launch::async, [&]
    {
        worker.beginRun();
        const auto status = schedule (worker, &FakePlugin::kHold, 1);
        worker.finishRun (true, &cancelled);
        return status;
    });
    // Destroyed before `block` and `worker`, so the held work is let go before
    // either waits on it, a failed check included.
    struct ReleaseOnExit
    {
        FakePlugin& plugin;
        ~ReleaseOnExit() { plugin.release(); }
    } releaseOnExit { plugin };
    REQUIRE (plugin.waitFor ([&] { return plugin.holding; }));
    CHECK (block.wait_for (std::chrono::milliseconds (50)) == std::future_status::timeout);

    cancelled.store (true);
    REQUIRE (block.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
    CHECK (block.get() == LV2_WORKER_SUCCESS);
    CHECK (plugin.endRuns == 1);
    CHECK_FALSE (plugin.holdReturned);   // the block did not wait the work out
}

TEST_CASE ("LV2 worker delivers work queued just before a reactivation", "[lv2][worker][state]")
{
    // The fixture's state is the Target work() last committed, so a request
    // dropped at the restart would leave the new instance on an older value.
    WorkerFixture fixture;
    Block block;
    duskstudio::lv2::Lv2Instance instance;
    fixture.activate (instance);

    // A slow request, then the one that matters queued behind it.
    instance.setControlPortValue (kDelayPort, 100.0f);
    instance.setControlPortValue (kTargetPort, 80.0f);
    block.run (instance, false);
    instance.setControlPortValue (kDelayPort, 0.0f);
    instance.setControlPortValue (kTargetPort, 30.0f);
    block.run (instance, false);

    std::string error;
    REQUIRE (instance.reactivate (44100.0, kBlock, error));
    CHECK_THAT (block.run (instance, false), WithinAbs (kInput * 0.3f, 1.0e-6f));
}

TEST_CASE ("LV2 worker keeps other threads off the request ring during run()", "[lv2][worker]")
{
    FakePlugin plugin;
    duskstudio::lv2::Lv2Worker worker;
    std::string error;
    REQUIRE (worker.attach (&plugin, &kFakeInterface, error));

    worker.beginRun();
    REQUIRE (schedule (worker, &FakePlugin::kHold, 1) == LV2_WORKER_SUCCESS);
    worker.finishRun (false);
    REQUIRE (plugin.waitFor ([&] { return plugin.holding; }));

    // Inside run(), with the ring full: a request from this thread is refused,
    // one from another thread in the same moment (a plug-in's save() during an
    // autosave, or its own thread) is taken, so it did not go through the ring.
    worker.beginRun();
    std::vector<uint8_t> filler (1020, FakePlugin::kSilent);
    while (schedule (worker, filler.data(), (uint32_t) filler.size()) == LV2_WORKER_SUCCESS) {}
    const uint8_t fromElsewhere[] = { 0x42 };
    std::thread::id otherThread;
    LV2_Worker_Status otherStatus = LV2_WORKER_ERR_UNKNOWN;
    std::thread other ([&]
    {
        otherThread = std::this_thread::get_id();
        otherStatus = schedule (worker, fromElsewhere, sizeof (fromElsewhere));
    });
    other.join();
    CHECK (otherStatus == LV2_WORKER_SUCCESS);
    CHECK (schedule (worker, fromElsewhere, sizeof (fromElsewhere)) == LV2_WORKER_ERR_NO_SPACE);
    worker.finishRun (false);

    // Its work() runs on the worker thread after the queue ahead of it, and
    // the reply reaches work_response on the audio side, after a run.
    plugin.release();
    REQUIRE (plugin.replied (0x42));
    CHECK (plugin.workThreadFor (0x42) != otherThread);
    CHECK (plugin.workThreadFor (0x42) != std::this_thread::get_id());
    CHECK (plugin.responses.empty());
    worker.beginRun();
    worker.finishRun (false);
    REQUIRE (plugin.responses.size() == 1);
    CHECK (plugin.responses[0] == std::vector<uint8_t> { 0x42 });
    worker.detach();
}

TEST_CASE ("LV2 worker fixture unloads safely with work queued", "[lv2][worker]")
{
    // The fixture aborts if work() touches a freed instance or runs twice at
    // once; the Delay port keeps each request inside work() for 50 ms, so the
    // queue is still full when the instance goes.
    WorkerFixture fixture;
    Block block;

    SECTION ("deactivate and destroy the instance")
    {
        duskstudio::lv2::Lv2Instance instance;
        fixture.activate (instance);
        instance.setControlPortValue (kDelayPort, 50.0f);
        for (int b = 0; b < 4; ++b)
        {
            instance.setControlPortValue (kTargetPort, 10.0f * (float) (b + 1));
            block.run (instance, false);
        }
        instance.deactivate();
        CHECK_FALSE (instance.isActive());
    }

    SECTION ("unload the slot")
    {
        duskstudio::lv2::NativeLv2Slot slot;
        std::string error;
        REQUIRE (slot.load (DUSKSTUDIO_WORKER_LV2_FIXTURE_PATH, 48000.0, kBlock, error,
                            kWorkerUri));
        auto* instance = slot.getInstance();
        REQUIRE (instance != nullptr);
        instance->setControlPortValue (kDelayPort, 50.0f);
        for (int b = 0; b < 4; ++b)
        {
            instance->setControlPortValue (kTargetPort, 10.0f * (float) (b + 1));
            slot.processStereo (block.inL.data(), block.inR.data(), block.outL.data(),
                                block.outR.data(), kBlock);
        }
        slot.unload();
        CHECK_FALSE (slot.isLoaded());
    }
}

TEST_CASE ("LV2 URID map agrees across threads and keeps unmapped strings in place",
           "[lv2][worker]")
{
    using duskstudio::lv2::Lv2UridMap;
    Lv2UridMap map;
    const auto first = map.map ("urn:duskstudio:test:urid#first");
    REQUIRE (first == 1);
    const char* firstUri = map.unmap (first);
    REQUIRE (firstUri != nullptr);
    CHECK (map.map (nullptr) == 0);
    CHECK (map.unmap (0) == nullptr);

    // Past the lock-free table too, and in a different order on each thread, so
    // lookups race first-time inserts in both halves of the map.
    static constexpr uint32_t kUris = Lv2UridMap::kLockFreeIds + 500;
    std::vector<std::string> uris;
    for (uint32_t i = 0; i < kUris; ++i)
        uris.push_back ("urn:duskstudio:test:urid#" + std::to_string (i));
    static constexpr std::array<uint32_t, 4> kStrides { 1, 3, 5, 11 };   // coprime with kUris
    std::array<std::vector<uint32_t>, 4> seen;
    std::vector<std::thread> threads;
    for (size_t t = 0; t < kStrides.size(); ++t)
        threads.emplace_back ([&, t]
        {
            seen[t].assign (kUris, 0);
            for (uint32_t i = 0; i < kUris; ++i)
            {
                const auto at = (uint32_t) (((uint64_t) i * kStrides[t]) % kUris);
                seen[t][at] = map.map (uris[at].c_str());
            }
        });
    for (auto& thread : threads) thread.join();

    for (uint32_t i = 0; i < kUris; ++i)
    {
        const auto id = seen[0][i];
        REQUIRE (id > first);
        for (size_t t = 1; t < seen.size(); ++t) REQUIRE (seen[t][i] == id);
        REQUIRE (map.unmap (id) != nullptr);
        REQUIRE (std::string (map.unmap (id)) == uris[i]);
    }
    CHECK (map.unmap (kUris + 2) == nullptr);
    CHECK (map.unmap (first) == firstUri);
}

TEST_CASE ("LV2 plug-in listing lv2core properties as required features loads", "[lv2][worker]")
{
    // lv2:hardRTCapable, isLive and inPlaceBroken are plug-in properties, but
    // older TTLs list them under requiredFeature; 0.13 loaded those. The state
    // path features are provided, just at save and restore, not instantiate.
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_WORKER_LV2_FIXTURE_PATH, error));
    duskstudio::lv2::Lv2Instance instance;
    REQUIRE (instance.create (bundle, kHardRtUri, error));
    INFO (error);
    REQUIRE (instance.activate (48000.0, kBlock, error));
    Block block;
    block.run (instance, true);
    CHECK_THAT (block.run (instance, true), WithinAbs (kInput * 0.5f, 1.0e-6f));
}

TEST_CASE ("LV2 plug-in requiring a feature the host lacks names it in the load error",
           "[lv2][worker]")
{
    duskstudio::lv2::Lv2Bundle bundle;
    std::string error;
    REQUIRE (bundle.load (DUSKSTUDIO_WORKER_LV2_FIXTURE_PATH, error));

    duskstudio::lv2::Lv2Instance instance;
    REQUIRE (instance.create (bundle, kUnsupportedUri, error));
    REQUIRE_FALSE (instance.activate (48000.0, kBlock, error));
    CHECK_FALSE (instance.isActive());
    CHECK (error == "requires the LV2 feature <urn:duskstudio:test:feature-dusk-studio-lacks>, "
                    "which Dusk Studio does not provide");

    duskstudio::lv2::NativeLv2Slot slot;
    std::string slotError;
    REQUIRE_FALSE (slot.load (DUSKSTUDIO_WORKER_LV2_FIXTURE_PATH, 48000.0, kBlock, slotError,
                              kUnsupportedUri));
    CHECK_THAT (slotError, ContainsSubstring ("<urn:duskstudio:test:feature-dusk-studio-lacks>"));
}
