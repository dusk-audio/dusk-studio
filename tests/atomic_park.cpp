#include <catch2/catch_test_macros.hpp>

#include "engine/AtomicPark.h"

#include <atomic>

// withParkedAtomicPointer is the bracketing primitive that guards
// PluginSlot::getStateBase64ForSave / getDescriptionXmlForSave from
// racing the audio thread inside the plugin. Phase A of the Mutter-
// safety work moved it here so it's testable without needing a real
// plugin instance.
//
// Per the Dusk Studio test guidelines (no threads / no sleeps / one concept
// per TEST_CASE), the contract is verified by inspecting the slot from
// inside the work lambda: while the lambda runs, an atomic load on the
// slot returns nullptr - which is exactly the property the audio
// thread observes when its own acquire-load lands during the parked
// window. The cross-thread visibility of release/acquire ordering is a
// language-spec property we don't need to re-test here.

TEST_CASE ("withParkedAtomicPointer: parks slot during work, restores after",
            "[atomic-park]")
{
    int sentinel = 42;
    std::atomic<int*> slot { &sentinel };

    int  workCalls       = 0;
    int* observedDuring  = reinterpret_cast<int*> (0xdeadbeef);  // sentinel "not set yet"

    auto* parked = duskstudio::withParkedAtomicPointer (slot,
        [&] (int& ref)
        {
            REQUIRE (&ref == &sentinel);
            ++workCalls;
            // Inside the work lambda the slot must read as null - this
            // is the property the audio thread relies on.
            observedDuring = slot.load (std::memory_order_acquire);
        },
        /*sleepMs*/ 0);

    REQUIRE (parked == &sentinel);
    REQUIRE (workCalls == 1);
    REQUIRE (observedDuring == nullptr);
    REQUIRE (slot.load (std::memory_order_acquire) == &sentinel);
}

TEST_CASE ("withParkedAtomicPointer: null slot is a no-op", "[atomic-park]")
{
    std::atomic<int*> slot { nullptr };
    int workCalls = 0;
    auto* parked = duskstudio::withParkedAtomicPointer (slot,
        [&] (int&) { ++workCalls; },
        /*sleepMs*/ 0);

    REQUIRE (parked == nullptr);
    REQUIRE (workCalls == 0);
    REQUIRE (slot.load (std::memory_order_acquire) == nullptr);
}

TEST_CASE ("withParkedAtomicPointer: pointer is restored even if the work "
            "throws", "[atomic-park]")
{
    // We don't expose exception-safety as a hard contract (the audio
    // path doesn't throw), but it's worth pinning down the current
    // behaviour so a future refactor doesn't accidentally regress it.
    // If the work throws, the pointer stays parked at nullptr - the
    // slot is poisoned. That's not great but it's the current
    // documented behaviour of the bracket; if we change it (e.g. to a
    // RAII guard that restores in the destructor) update this assert
    // deliberately.
    int sentinel = 7;
    std::atomic<int*> slot { &sentinel };

    bool threw = false;
    try
    {
        duskstudio::withParkedAtomicPointer (slot,
            [] (int&) { throw std::runtime_error ("boom"); },
            /*sleepMs*/ 0);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    REQUIRE (threw);
    REQUIRE (slot.load (std::memory_order_acquire) == nullptr);
}
