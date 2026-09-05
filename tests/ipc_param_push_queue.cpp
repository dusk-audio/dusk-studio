#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/ParamPushQueue.h"

#include <cstdint>

using duskstudio::ipc::ParamPushQueue;
using duskstudio::ipc::ParamPushRecord;

namespace
{
ParamPushRecord entry (std::uint32_t index, std::uint32_t generation = 0)
{
    return { { index, (float) index * 0.001f, index + 1 }, generation };
}
} // namespace

TEST_CASE ("ParamPushQueue hands entries over in order", "[ipc]")
{
    ParamPushQueue queue;
    ParamPushRecord out {};

    REQUIRE_FALSE (queue.pop (out));

    for (std::uint32_t i = 0; i < 8; ++i)
        queue.push (entry (i));

    for (std::uint32_t i = 0; i < 8; ++i)
    {
        REQUIRE (queue.pop (out));
        REQUIRE (out.payload.paramIndex == i);
        REQUIRE (out.payload.sequenceNumber == i + 1);
    }

    REQUIRE_FALSE (queue.pop (out));
}

TEST_CASE ("ParamPushQueue survives more pushes than a drain interval holds", "[ipc]")
{
    ParamPushQueue queue;
    ParamPushRecord out {};

    constexpr std::uint32_t overshoot = 10;
    const auto pushed = (std::uint32_t) ParamPushQueue::kCapacity + overshoot;
    for (std::uint32_t i = 0; i < pushed; ++i)
        queue.push (entry (i));

    // A full queue drops its oldest entries, not its newest: the parent is
    // mirroring a control position, so the last value written is the one that
    // has to arrive.
    for (std::uint32_t i = overshoot; i < pushed; ++i)
    {
        REQUIRE (queue.pop (out));
        REQUIRE (out.payload.paramIndex == i);
    }

    REQUIRE_FALSE (queue.pop (out));
}

TEST_CASE ("ParamPushQueue keeps FIFO order as the ring wraps", "[ipc]")
{
    ParamPushQueue queue;
    ParamPushRecord out {};

    std::uint32_t next = 0;

    for (std::uint32_t round = 0; round < 4 * (std::uint32_t) ParamPushQueue::kCapacity; ++round)
    {
        queue.push (entry (next));
        queue.push (entry (next + 1));

        REQUIRE (queue.pop (out));
        REQUIRE (out.payload.paramIndex == next);
        REQUIRE (queue.pop (out));
        REQUIRE (out.payload.paramIndex == next + 1);

        next += 2;
    }

    REQUIRE_FALSE (queue.pop (out));
}

TEST_CASE ("ParamPushQueue carries the producer's plugin generation", "[ipc]")
{
    ParamPushQueue queue;
    ParamPushRecord out {};

    queue.push (entry (3, 7));
    queue.push (entry (4, 8));

    REQUIRE (queue.pop (out));
    REQUIRE (out.generation == 7);
    REQUIRE (queue.pop (out));
    REQUIRE (out.generation == 8);
}

TEST_CASE ("ParamPushQueue never lets a second producer into a slot still being written", "[ipc]")
{
    ParamPushQueue queue;
    ParamPushRecord out {};
    constexpr auto capacity = (std::uint32_t) ParamPushQueue::kCapacity;

    std::uint64_t paused = 0;
    REQUIRE (queue.claim (paused));
    REQUIRE (paused == 0);

    for (std::uint32_t i = 1; i < capacity; ++i)
        REQUIRE (queue.push (entry (i)));

    // A full lap later the same slot comes round while its first producer is
    // still inside it: the push is dropped without reserving a ticket.
    REQUIRE_FALSE (queue.push (entry (capacity)));

    // The reader holds at the reserved ticket instead of skipping past it.
    REQUIRE_FALSE (queue.pop (out));

    queue.commit (paused, entry (0));
    for (std::uint32_t i = 0; i < capacity; ++i)
    {
        REQUIRE (queue.pop (out));
        REQUIRE (out.payload.paramIndex == i);
    }
    REQUIRE_FALSE (queue.pop (out));

    // Once published the slot is ordinary again: a whole further lap goes
    // through, and the lap after that overwrites it.
    for (std::uint32_t i = capacity; i <= 2 * capacity; ++i)
        REQUIRE (queue.push (entry (i)));
    for (std::uint32_t i = capacity + 1; i <= 2 * capacity; ++i)
    {
        REQUIRE (queue.pop (out));
        REQUIRE (out.payload.paramIndex == i);
    }
    REQUIRE_FALSE (queue.pop (out));
}

TEST_CASE ("ParamPushQueue reader waits on a slot whose producer is mid-write", "[ipc]")
{
    ParamPushQueue queue;
    ParamPushRecord out {};

    REQUIRE (queue.push (entry (0)));
    std::uint64_t inFlight = 0;
    REQUIRE (queue.claim (inFlight));
    REQUIRE (queue.push (entry (2)));

    REQUIRE (queue.pop (out));
    REQUIRE (out.payload.paramIndex == 0);
    REQUIRE_FALSE (queue.pop (out));

    queue.commit (inFlight, entry (1));
    REQUIRE (queue.pop (out));
    REQUIRE (out.payload.paramIndex == 1);
    REQUIRE (queue.pop (out));
    REQUIRE (out.payload.paramIndex == 2);
    REQUIRE_FALSE (queue.pop (out));
}
