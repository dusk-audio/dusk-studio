#include <catch2/catch_test_macros.hpp>

#include "engine/vst3/Vst3HostContext.h"

TEST_CASE ("Vst3HostContext repeatedly releases its owned host object",
           "[vst3][hostcontext][regression][issue-382]")
{
    using HostContext = duskstudio::vst3::Vst3HostContext;
    HostContext::resetHostObjectLifetimeCounts();

    for (int iteration = 0; iteration < 64; ++iteration)
    {
        CAPTURE (iteration);
        HostContext context;
        REQUIRE (context.hostApplication() != nullptr);
        REQUIRE (context.componentHandler() != nullptr);
        REQUIRE (context.plugFrame() != nullptr);
    }

    const auto counts = HostContext::hostObjectLifetimeCounts();
    REQUIRE (counts.constructed == 64);
    REQUIRE (counts.destroyed == counts.constructed);
}
