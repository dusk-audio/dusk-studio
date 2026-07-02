// A/B null-test: the generalized processBlock (driven through the InsertAdapter,
// as the production slot runs it) must produce the same audio as the legacy
// processStereo for a stereo plugin. Fresh instances of the same plugin are fed
// identical input; their outputs must match within the plugin's own instance-to-
// instance variance. Gated on DUSKSTUDIO_TEST_CLAP=/path/to.clap so CI without a
// plugin stays green.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/clap/ClapBundle.h"
#include "engine/clap/ClapInstance.h"
#include "engine/hosting/InsertAdapter.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

TEST_CASE ("ClapInstance: processBlock nulls against processStereo", "[clap][hosting][ab]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping CLAP A/B null-test");
        return;
    }

    using namespace duskstudio;
    clap::ClapBundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE_FALSE (bundle.plugins().empty());
    const auto id = bundle.plugins().front().id;

    constexpr double kSR = 48000.0;
    constexpr int    kBlock = 256;

    // Three fresh instances of the same plugin: ref1 + ref2 both driven by the
    // legacy processStereo (their sample-by-sample difference is the plugin's own
    // instance-to-instance non-determinism floor — tape/analog emulations are not
    // bit-identical across instances), and gen driven by the generalized
    // InsertAdapter → processBlock. The generalized path is correct iff gen is no
    // further from ref1 than ref2 is (plus an FP epsilon). For a deterministic
    // plugin the floor is 0, so this collapses to an exact null.
    clap::ClapInstance ref1, ref2, gen;
    if (! ref1.create (bundle, id, err))
    {
        // create() rejects anything but stereo-in/stereo-out (e.g. an instrument
        // like Diva). Not a valid stereo insert → nothing for this A/B to compare.
        SUCCEED ("plugin is not a stereo insert (" + err + ") — skipping A/B null-test");
        return;
    }
    REQUIRE (ref2.create (bundle, id, err));
    REQUIRE (gen .create (bundle, id, err));
    REQUIRE (ref1.activate (kSR, kBlock, err));
    REQUIRE (ref2.activate (kSR, kBlock, err));
    REQUIRE (gen .activate (kSR, kBlock, err));

    hosting::InsertAdapter adapter;
    adapter.prepare (gen.portLayout(), kBlock);

    std::vector<float> r1L ((size_t) kBlock), r1R ((size_t) kBlock),
                       r2L ((size_t) kBlock), r2R ((size_t) kBlock),
                       gL  ((size_t) kBlock), gR  ((size_t) kBlock);

    constexpr double kPi = 3.14159265358979323846;
    constexpr float  kEps = 1.0e-6f;
    double phase = 0.0;
    const double dw = 2.0 * kPi * 440.0 / kSR;
    float worstFloor = 0.0f;   // ref2-vs-ref1: the plugin's own instance-to-instance variance
    float worstGen   = 0.0f;   // gen-vs-ref1:  the generalized processBlock path's divergence

    for (int b = 0; b < 32; ++b)   // enough blocks for any internal tail to develop
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const float s = 0.3f * (float) std::sin (phase);
            phase += dw;
            r1L[(size_t) i] = r2L[(size_t) i] = gL[(size_t) i] = s;
            r1R[(size_t) i] = r2R[(size_t) i] = gR[(size_t) i] = 0.7f * s;   // asymmetric L/R
        }

        ref1.processStereo (r1L.data(), r1R.data(), r1L.data(), r1R.data(), kBlock);   // in-place, matches the call site
        ref2.processStereo (r2L.data(), r2R.data(), r2L.data(), r2R.data(), kBlock);
        adapter.process (gen, gL.data(), gR.data(), kBlock);                           // InsertAdapter → processBlock

        for (int i = 0; i < kBlock; ++i)
        {
            const auto n = (size_t) i;
            worstFloor = std::max ({ worstFloor, std::abs (r2L[n] - r1L[n]), std::abs (r2R[n] - r1R[n]) });
            worstGen   = std::max ({ worstGen,   std::abs (gL[n]  - r1L[n]), std::abs (gR[n]  - r1R[n]) });
        }
    }

    // The generalized path is correct iff it stays within the plugin's own
    // instance-to-instance non-determinism. For a deterministic plugin the floor
    // is 0 and this is an exact null; a tape/analog emulation whose two reference
    // instances already differ gets that same envelope (×4 headroom for a third
    // instance's independent variance), which still catches a real plumbing bug
    // (that would push gen orders of magnitude past the floor).
    WARN ("A/B: worst instance floor = " << worstFloor << ", worst processBlock divergence = " << worstGen);
    REQUIRE (worstGen <= std::max (kEps, 4.0f * worstFloor));
}
