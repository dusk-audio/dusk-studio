#include <catch2/catch_test_macros.hpp>

#include "engine/alsa/AlsaAudioIODevice.h"
#include "foundation/AutoResetEvent.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// The stop() timed-join / abandon path: a wedged I/O thread is detached after
// the 2000 ms join window and the whole device must be leaked - never freed -
// because the detached thread keeps dereferencing it until it finally exits.

using duskstudio::AlsaAudioIODevice;

TEST_CASE ("AlsaAudioIODevice: wedged I/O thread leaks the device, never frees it", "[audio][device]")
{
    auto dev = std::make_unique<AlsaAudioIODevice> ("teardown-test", "", "");
    auto* raw = dev.get();

    // Heap-shared so the detached thread can't dangle into this test's stack
    // if an assertion unwinds before the final wait.
    auto lateTouchOk = std::make_shared<std::atomic<bool>> (false);
    auto threadDone  = std::make_shared<std::atomic<bool>> (false);

    // Body ignores the exit flag well past stop()'s 2000 ms join, then touches
    // the device's state before signalling - if the owner had freed the object,
    // those dereferences are use-after-free (caught under ASan, and pinned here
    // by the state assertions below).
    raw->startThreadForTest ([lateTouchOk, threadDone, raw]
                             (std::atomic<bool>&, dusk::AutoResetEvent& exited)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (3500));
        lateTouchOk->store (raw->getName() == "teardown-test"
                            && raw->getXRunCount() == 0);
        exited.signal();
        threadDone->store (true);
    });

    const auto t0 = std::chrono::steady_clock::now();
    raw->stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::steady_clock::now() - t0).count();

    // stop() must hold the full join window, then detach - neither joining
    // until the thread exits nor returning early.
    REQUIRE (stopMs >= 1900);
    REQUIRE (stopMs < 3000);
    REQUIRE (raw->ioThreadWasAbandoned());

    // An abandoned device refuses to rejoin the streaming lifecycle.
    duskstudio::device::ChannelSet mask;
    mask.setBit (0);
    REQUIRE_FALSE (raw->open (mask, mask, 48000.0, 256).empty());

    // Destruction parks (leaks) the whole object instead of destroying it.
    const int parkedBefore = AlsaAudioIODevice::abandonedCount();
    AlsaAudioIODevice::destroyOrPark (std::move (dev));
    REQUIRE (AlsaAudioIODevice::abandonedCount() == parkedBefore + 1);

    // The parked object stays valid until (and past) the thread's final
    // signal: wait out the body's late touch and verify it saw intact state.
    for (int i = 0; i < 500 && ! threadDone->load(); ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    REQUIRE (threadDone->load());
    REQUIRE (lateTouchOk->load());
    REQUIRE (raw->getName() == "teardown-test");
}

TEST_CASE ("AlsaAudioIODevice: exiting thread joins inside the window, no abandonment", "[audio][device]")
{
    auto dev = std::make_unique<AlsaAudioIODevice> ("teardown-clean", "", "");
    auto* raw = dev.get();

    raw->startThreadForTest ([] (std::atomic<bool>& shouldExit, dusk::AutoResetEvent& exited)
    {
        while (! shouldExit.load (std::memory_order_acquire))
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        exited.signal();
    });

    raw->stop();
    REQUIRE_FALSE (raw->ioThreadWasAbandoned());

    // A device with a cleanly-joined thread is destroyed normally.
    const int parkedBefore = AlsaAudioIODevice::abandonedCount();
    AlsaAudioIODevice::destroyOrPark (std::move (dev));
    REQUIRE (AlsaAudioIODevice::abandonedCount() == parkedBefore);
}
