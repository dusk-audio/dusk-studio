#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/OutputPairRouting.h"

#include <array>
#include <vector>

using namespace duskstudio::outputpair;
using Catch::Matchers::WithinAbs;

TEST_CASE ("output pair encode/decode round-trips")
{
    for (int l = 0; l < 32; ++l)
        for (int r = 0; r < 32; ++r)
        {
            const int id = encodePair (l, r);
            REQUIRE (id > 0);                 // never collides with "unrouted"
            REQUIRE (decodePairL (id) == l);
            REQUIRE (decodePairR (id) == r);
        }
}

namespace
{
// 4 device outputs, each a small block, plus a null channel to prove the
// guard. The tap writes via a float* const* exactly like the audio callback.
struct OutBuffers
{
    std::array<std::vector<float>, 4> bufs;
    std::array<float*, 5> ptrs {};   // [4] deliberately left null

    OutBuffers (int numSamples)
    {
        for (int c = 0; c < 4; ++c) { bufs[(size_t) c].assign ((size_t) numSamples, 0.0f); ptrs[(size_t) c] = bufs[(size_t) c].data(); }
        ptrs[4] = nullptr;
    }
};
}

TEST_CASE ("tapStereoPairInto accumulates onto the routed pair")
{
    constexpr int n = 8;
    std::vector<float> L ((size_t) n, 0.5f);
    std::vector<float> R ((size_t) n, -0.25f);

    SECTION ("unrouted (-1 / 0) is a no-op")
    {
        OutBuffers out (n);
        tapStereoPairInto (out.ptrs.data(), 4, L.data(), R.data(), n, -1);
        tapStereoPairInto (out.ptrs.data(), 4, L.data(), R.data(), n, 0);
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < n; ++i) REQUIRE_THAT (out.bufs[(size_t) c][(size_t) i], WithinAbs (0.0f, 1e-6f));
    }

    SECTION ("valid pair receives L/R, others untouched")
    {
        OutBuffers out (n);
        tapStereoPairInto (out.ptrs.data(), 4, L.data(), R.data(), n, encodePair (2, 3));
        for (int i = 0; i < n; ++i)
        {
            REQUIRE_THAT (out.bufs[2][(size_t) i], WithinAbs (0.5f,   1e-6f));
            REQUIRE_THAT (out.bufs[3][(size_t) i], WithinAbs (-0.25f, 1e-6f));
            REQUIRE_THAT (out.bufs[0][(size_t) i], WithinAbs (0.0f,   1e-6f));
            REQUIRE_THAT (out.bufs[1][(size_t) i], WithinAbs (0.0f,   1e-6f));
        }
    }

    SECTION ("two taps to the same pair sum")
    {
        OutBuffers out (n);
        tapStereoPairInto (out.ptrs.data(), 4, L.data(), R.data(), n, encodePair (0, 1));
        tapStereoPairInto (out.ptrs.data(), 4, L.data(), R.data(), n, encodePair (0, 1));
        for (int i = 0; i < n; ++i)
        {
            REQUIRE_THAT (out.bufs[0][(size_t) i], WithinAbs (1.0f,  1e-6f));
            REQUIRE_THAT (out.bufs[1][(size_t) i], WithinAbs (-0.5f, 1e-6f));
        }
    }

    SECTION ("pair beyond the device's open outputs is a no-op (no OOB)")
    {
        OutBuffers out (n);
        // Channel 5 doesn't exist (numOuts = 4) and channel 4 is null.
        tapStereoPairInto (out.ptrs.data(), 4, L.data(), R.data(), n, encodePair (4, 5));
        tapStereoPairInto (out.ptrs.data(), 5, L.data(), R.data(), n, encodePair (4, 0));   // ch4 null
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < n; ++i) REQUIRE_THAT (out.bufs[(size_t) c][(size_t) i], WithinAbs (0.0f, 1e-6f));
    }
}
