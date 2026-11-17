#include <catch2/catch_test_macros.hpp>

#include "foundation/RtWakeEvent.h"
#include "support/ThreadWakeups.h"

#include <atomic>
#include <cstdint>
#include <thread>

// The worker pool's lanes park on this event and the audio thread wakes them,
// so these cases run real threads, as the pool's own cases do.

using dusk::RtWakeEvent;
using duskstudio::test::eventually;
using duskstudio::test::staysAt;

TEST_CASE ("RtWakeEvent: a signal made before the wait is kept, and signals made together wake once",
           "[rt-wake-event]")
{
    RtWakeEvent event;
    event.signal();
    event.signal();
    event.signal();

    std::atomic<int> woken { 0 };
    std::thread waiter ([&event, &woken]
    {
        for (int i = 0; i < 2; ++i)
        {
            event.wait();
            woken.fetch_add (1);
        }
    });
    REQUIRE (eventually ([&woken] { return woken.load() == 1; }));
    REQUIRE (staysAt (woken, 1));

    event.signal();
    REQUIRE (eventually ([&woken] { return woken.load() == 2; }));
    waiter.join();
}

TEST_CASE ("RtWakeEvent: the woken thread sees what the signaller wrote before it signalled",
           "[rt-wake-event]")
{
    // Plain fields handed back and forth through two events. A wake that did
    // not order them is a data race ThreadSanitizer reports.
    static constexpr int kRounds = 20000;
    RtWakeEvent ping, pong;
    std::uint64_t request = 0, reply = 0;
    std::thread echo ([&ping, &pong, &request, &reply]
    {
        for (int i = 0; i < kRounds; ++i)
        {
            ping.wait();
            reply = request * 2 + 1;
            pong.signal();
        }
    });

    bool inOrder = true;
    for (int i = 0; i < kRounds; ++i)
    {
        request = (std::uint64_t) i;
        ping.signal();
        pong.wait();
        inOrder = inOrder && reply == (std::uint64_t) i * 2 + 1;
    }
    echo.join();
    REQUIRE (inOrder);
}
