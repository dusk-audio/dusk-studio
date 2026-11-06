#include <catch2/catch_test_macros.hpp>

#include "ui/NativeEmbeddedParent.h"

using duskstudio::platform::EmbeddedChildLatch;
using duskstudio::platform::NativeEmbeddedParent;

namespace
{
NativeEmbeddedParent peer (std::uintptr_t handle, std::uint32_t id)
{
    return { NativeEmbeddedParent::Backend::platform, handle, id };
}
} // namespace

TEST_CASE ("a native child closed by its own failure stays closed only on the parent it failed on",
           "[ui][embed]")
{
    const auto main = peer (0x1000, 7);
    EmbeddedChildLatch latch;
    latch.opening (main);

    SECTION ("a close the owner asked for never latches")
    {
        latch.closed (true, true);
        CHECK_FALSE (latch.blocks (main));
    }

    SECTION ("an unrequested close with the parent still alive is a failure")
    {
        latch.closed (false, true);
        CHECK (latch.blocks (main));
        CHECK (latch.blocks (main));
    }

    SECTION ("losing the parent is not a failure")
    {
        latch.closed (false, false);
        CHECK_FALSE (latch.blocks (peer (0x2000, 8)));
        CHECK_FALSE (latch.blocks (main));
    }

    SECTION ("a recreated parent clears the latch, even at the same address")
    {
        latch.closed (false, true);
        CHECK_FALSE (latch.blocks (peer (0x1000, 8)));
        CHECK_FALSE (latch.isLatched());
    }

    SECTION ("a parent that goes away clears the latch")
    {
        latch.closed (false, true);
        CHECK_FALSE (latch.blocks ({}));
        CHECK_FALSE (latch.blocks (main));
    }

    SECTION ("a failed open holds until the parent changes")
    {
        latch.openFailed();
        CHECK (latch.blocks (main));
        latch.opening (peer (0x2000, 8));
        CHECK_FALSE (latch.blocks (peer (0x2000, 8)));
    }
}
