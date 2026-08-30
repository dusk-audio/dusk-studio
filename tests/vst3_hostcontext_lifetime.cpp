#include <catch2/catch_test_macros.hpp>

#include "engine/vst3/Vst3HostContext.h"

TEST_CASE ("Vst3HostContext repeatedly releases its owned host object",
           "[vst3][hostcontext][regression][issue-382]")
{
    for (int iteration = 0; iteration < 64; ++iteration)
    {
        CAPTURE (iteration);
        duskstudio::vst3::Vst3HostContext context;
        REQUIRE (context.hostApplication() != nullptr);
        REQUIRE (context.componentHandler() != nullptr);
        REQUIRE (context.plugFrame() != nullptr);
    }
}
