#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/multisample/DuskMultisampleProcessor.h"

#include <array>
#include <utility>

using Catch::Matchers::WithinAbs;

namespace
{
using Dispatch = std::pair<int, float>;

struct DispatchLog
{
    std::array<Dispatch, 4> values {};
    std::size_t size { 0 };
};

void recordDispatch (void* context, int cc, float value)
{
    auto& log = *static_cast<DispatchLog*> (context);
    if (log.size < log.values.size())
        log.values[log.size++] = { cc, value };
}

void processOneBlock (duskstudio::DuskMultisampleProcessor& processor)
{
    constexpr int blockSize = 16;
    float left[blockSize] {};
    float right[blockSize] {};
    float* outputs[] { left, right };

    duskstudio::hosting::PortBuffers io;
    io.mainOut = outputs;
    io.mainOutChannels = 2;
    io.numFrames = blockSize;
    processor.processBlock (io);
}

void activate (duskstudio::DuskMultisampleProcessor& processor)
{
    std::string error;
    REQUIRE (processor.activate (48000.0, 16, error));
}
} // namespace

TEST_CASE ("Multisample HDCC coalescing dispatches only the latest value",
           "[multisample][hdcc]")
{
    duskstudio::DuskMultisampleProcessor processor;
    activate (processor);

    DispatchLog dispatches;
    processor.setHdccDispatchObserverForTest (&dispatches, recordDispatch);

    processor.setHDCC (17, 0.1f);
    processor.setHDCC (17, 0.4f);
    processor.setHDCC (17, 0.9f);
    processOneBlock (processor);

    REQUIRE (dispatches.size == 1);
    CHECK (dispatches.values[0].first == 17);
    CHECK_THAT (dispatches.values[0].second, WithinAbs (0.9f, 1.0e-6f));

    processOneBlock (processor);
    CHECK (dispatches.size == 1);
}

TEST_CASE ("Multisample HDCC coalescing dispatches different controllers independently",
           "[multisample][hdcc]")
{
    duskstudio::DuskMultisampleProcessor processor;
    activate (processor);

    DispatchLog dispatches;
    processor.setHdccDispatchObserverForTest (&dispatches, recordDispatch);

    processor.setHDCC (3, 0.25f);
    processor.setHDCC (300, 0.75f);
    processOneBlock (processor);

    REQUIRE (dispatches.size == 2);
    CHECK (dispatches.values[0].first == 3);
    CHECK_THAT (dispatches.values[0].second, WithinAbs (0.25f, 1.0e-6f));
    CHECK (dispatches.values[1].first == 300);
    CHECK_THAT (dispatches.values[1].second, WithinAbs (0.75f, 1.0e-6f));
}
