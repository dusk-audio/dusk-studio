#include <catch2/catch_test_macros.hpp>

#include "engine/PdcMath.h"

#include <array>

using duskstudio::pdc::computeCompensations;

TEST_CASE ("PDC: every track compensates to the deepest latency", "[pdc]")
{
    // Track 1 is the deepest (512). Each track delays by deepest - own so all
    // line up; the deepest track itself adds zero.
    const std::array<int, 4> latency { 0, 512, 128, 64 };
    std::array<int, 4> comp {};
    const int deepest = computeCompensations (latency.data(), comp.data(), 4);

    REQUIRE (deepest == 512);
    REQUIRE (comp[0] == 512);   // no latency → delayed by the full amount
    REQUIRE (comp[1] == 0);     // the deepest track is the reference
    REQUIRE (comp[2] == 384);
    REQUIRE (comp[3] == 448);

    // Post-condition: own latency + compensation is identical for every track.
    for (int i = 0; i < 4; ++i)
        REQUIRE (latency[(size_t) i] + comp[(size_t) i] == deepest);
}

TEST_CASE ("PDC: all-zero latency yields no compensation", "[pdc]")
{
    const std::array<int, 3> latency { 0, 0, 0 };
    std::array<int, 3> comp { -1, -1, -1 };
    const int deepest = computeCompensations (latency.data(), comp.data(), 3);

    REQUIRE (deepest == 0);
    for (int c : comp) REQUIRE (c == 0);
}

TEST_CASE ("PDC: a single latency source delays everyone else", "[pdc]")
{
    std::array<int, 5> latency { 0, 0, 300, 0, 0 };
    std::array<int, 5> comp {};
    const int deepest = computeCompensations (latency.data(), comp.data(), 5);

    REQUIRE (deepest == 300);
    REQUIRE (comp[2] == 0);
    for (int i : { 0, 1, 3, 4 }) REQUIRE (comp[(size_t) i] == 300);
}
