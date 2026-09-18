#include <catch2/catch_test_macros.hpp>

#include "../src/dsp/MasterBus.h"
#include "../src/session/Session.h"

#include <algorithm>
#include <cmath>
#include <vector>

using duskstudio::MasterBus;
using duskstudio::MasterBusParams;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int    kBlock      = 256;
constexpr int    kBlocks     = 8;

struct Arrival
{
    int   index = -1;
    float value = 0.0f;
};

// Feeds a single unit impulse at index 0 and reports where the output first
// leaves silence.
Arrival impulseArrival (MasterBus& master)
{
    std::vector<float> l ((size_t) kBlock, 0.0f);
    std::vector<float> r ((size_t) kBlock, 0.0f);
    for (int b = 0; b < kBlocks; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        if (b == 0) l[0] = r[0] = 1.0f;
        master.processInPlace (l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            if (std::abs (l[(size_t) i]) > 1.0e-6f)
                return { b * kBlock + i, l[(size_t) i] };
    }
    return {};
}

void quietTape (MasterBusParams& params)
{
    params.tape.autoCal.store (false);
    params.tape.autoComp.store (false);
    params.tape.noiseAmount.store (0.0f);
    params.tape.wow.store (0.0f);
    params.tape.flutter.store (0.0f);
}
}

TEST_CASE ("the master tape's output arrives where its reported latency says", "[master][tape][latency]")
{
    MasterBusParams params;
    quietTape (params);

    MasterBus master;
    master.bind (params);
    master.prepare (kSampleRate, kBlock, 1);

    const int reported = master.getTapeLatencySamples();
    REQUIRE (reported > 0);

    SECTION ("disengaged, the master still holds the compensation")
    {
        params.tapeEnabled.store (false);
        const auto arrival = impulseArrival (master);
        REQUIRE (arrival.index == reported);
        REQUIRE (arrival.value == 1.0f);
    }

    SECTION ("a passthrough path does not arrive early")
    {
        // Thru is sample-exact inside the core, which reports no latency for
        // it. The master keeps its own alignment instead, so the mix does not
        // jump forward by the compensation the bounce trim removes.
        params.tapeEnabled.store (true);
        params.tape.signalPath.store (3);
        REQUIRE (master.getTapeLatencySamples() == reported);
        const auto arrival = impulseArrival (master);
        REQUIRE (arrival.index == reported);
        REQUIRE (arrival.value == 1.0f);
    }

    SECTION ("a processing path does not arrive early either")
    {
        // The tape smears an impulse, so only the leading edge is meaningful:
        // nothing may appear before the compensated point.
        params.tapeEnabled.store (true);
        params.tape.signalPath.store (0);
        const auto arrival = impulseArrival (master);
        REQUIRE (arrival.index >= reported);
        REQUIRE (arrival.index < reported + 16);
    }
}
