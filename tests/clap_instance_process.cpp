// Increment 1 of the native CLAP host: load a real .clap, create + activate the
// instance, and process audio offline. Gated on DUSKSTUDIO_TEST_CLAP=/path/to.clap
// (e.g. ~/.clap/DuskVerb.clap) so CI without a CLAP plugin stays green.
// See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/ClapBundle.h"
#include "engine/clap/ClapInstance.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

TEST_CASE ("ClapInstance loads + processes a real CLAP plugin", "[clap][instance]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        // Conditional fixture: pass (don't fail the suite) when no .clap is
        // provided. Point DUSKSTUDIO_TEST_CLAP at a plugin to actually exercise it.
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-plugin process test");
        return;
    }

    duskstudio::clap::ClapBundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE_FALSE (bundle.plugins().empty());

    duskstudio::clap::ClapInstance inst;
    // Single-plugin fixture (e.g. DuskVerb), so front() is deterministic.
    REQUIRE (inst.create (bundle, bundle.plugins().front().id, err));
    REQUIRE (inst.activate (48000.0, 512, err));
    REQUIRE (inst.outputChannels() >= 1);

    constexpr int kBlock = 512;
    constexpr int kBlocks = 40;   // drive enough blocks for any tail to settle
    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);

    SECTION ("silence in stays finite and silent")
    {
        std::fill (inL.begin(), inL.end(), 0.0f);
        std::fill (inR.begin(), inR.end(), 0.0f);

        float peak = 0.0f;
        for (int b = 0; b < kBlocks; ++b)
        {
            inst.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
                peak = std::max ({ peak, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
            }
        }
        REQUIRE (peak < 1.0e-3f);   // silence in → silence out
    }

    SECTION ("signal in produces finite, non-silent output")
    {
        double phase = 0.0;
        const double dw = 2.0 * M_PI * 1000.0 / 48000.0;
        float peakOut = 0.0f;
        for (int b = 0; b < kBlocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = 0.25f * (float) std::sin (phase);
                phase += dw;
                inL[(size_t) i] = inR[(size_t) i] = s;
            }
            inst.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
                peakOut = std::max ({ peakOut, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
            }
        }
        REQUIRE (peakOut > 1.0e-3f);   // signal passes through the plugin
    }
}
