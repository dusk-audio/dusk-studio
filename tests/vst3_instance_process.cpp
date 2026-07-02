// Instantiate a real VST3 effect and process audio offline THROUGH the
// InsertAdapter — the same generalized path the DSP call sites use — proving the
// host-agnostic foundation covers its third format. Gated on
// DUSKSTUDIO_TEST_VST3=/path/to.vst3.

#include <catch2/catch_test_macros.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/vst3/Vst3Bundle.h"
#include "engine/vst3/Vst3Instance.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

TEST_CASE ("Vst3Instance instantiates + processes a VST3 effect via InsertAdapter", "[vst3][instance]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_VST3");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_VST3 not set — skipping live VST3-process test");
        return;
    }

    using namespace duskstudio;
    vst3::Vst3Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));

    // Pick an audio effect; skip instruments (no audio input to insert on).
    std::string classId;
    for (const auto& d : bundle.plugins())
        if (! d.isInstrument) { classId = d.id; break; }
    if (classId.empty())
    {
        SUCCEED ("module advertises no audio effect — skipping");
        return;
    }

    vst3::Vst3Instance inst;
    REQUIRE (inst.create (bundle, classId, err));
    REQUIRE (inst.portLayout().mainOutChannels() > 0);

    constexpr int kBlock = 256;
    REQUIRE (inst.activate (48000.0, kBlock, err));
    REQUIRE (inst.isActive());
    REQUIRE (inst.getLatencySamples() >= 0);

    hosting::InsertAdapter adapter;
    adapter.prepare (inst.portLayout(), kBlock);

    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);

    SECTION ("silence in stays finite and quiet")
    {
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        float peak = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            adapter.process (inst, L.data(), R.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (L[(size_t) i]));
                REQUIRE (std::isfinite (R[(size_t) i]));
                peak = std::max ({ peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]) });
            }
        }
        REQUIRE (peak < 1.0e-2f);   // a default effect doesn't self-oscillate from silence
    }

    SECTION ("signal passes through the effect")
    {
        constexpr double kPi = 3.14159265358979323846;
        double phase = 0.0;
        const double dw = 2.0 * kPi * 440.0 / 48000.0;
        float peak = 0.0f;
        for (int b = 0; b < 32; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = 0.3f * (float) std::sin (phase);
                phase += dw;
                L[(size_t) i] = s;
                R[(size_t) i] = 0.7f * s;
            }
            adapter.process (inst, L.data(), R.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (L[(size_t) i]));
                REQUIRE (std::isfinite (R[(size_t) i]));
                peak = std::max ({ peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]) });
            }
        }
        REQUIRE (peak > 1.0e-4f);   // audio makes it through the plugin
    }

    SECTION ("reactivate at a new rate keeps processing")
    {
        REQUIRE (inst.reactivate (44100.0, kBlock, err));
        REQUIRE (inst.isActive());
        std::fill (L.begin(), L.end(), 0.1f);
        std::fill (R.begin(), R.end(), 0.1f);
        adapter.process (inst, L.data(), R.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            REQUIRE (std::isfinite (L[(size_t) i]));
    }
}
