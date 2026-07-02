// Instantiate a real LV2 effect and process audio offline THROUGH the InsertAdapter
// — the same generalized path the DSP call sites use — which is the real test of
// whether the host-agnostic foundation (INativeInstance / InsertAdapter / PortBuffers)
// generalizes past CLAP. Gated on DUSKSTUDIO_TEST_LV2=/path/to/bundle.lv2.

#include <catch2/catch_test_macros.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/lv2/Lv2Bundle.h"
#include "engine/lv2/Lv2Instance.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

TEST_CASE ("Lv2Instance instantiates + processes an LV2 effect via InsertAdapter", "[lv2][instance]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_LV2");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_LV2 not set — skipping live LV2-process test");
        return;
    }

    using namespace duskstudio;
    lv2::Lv2Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (path, err));
    REQUIRE_FALSE (bundle.plugins().empty());

    // Pick an audio effect (audio in + out); skip instruments / MIDI-only utilities.
    std::string uri;
    for (const auto& d : bundle.plugins())
        if (d.audioInputs > 0 && d.audioOutputs > 0) { uri = d.uri; break; }
    if (uri.empty())
    {
        SUCCEED ("bundle advertises no audio effect — skipping");
        return;
    }

    lv2::Lv2Instance inst;
    REQUIRE (inst.create (bundle, uri, err));
    REQUIRE (inst.portLayout().mainOutChannels() > 0);

    constexpr int kBlock = 256;
    REQUIRE (inst.activate (48000.0, kBlock, err));
    REQUIRE (inst.isActive());

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

    SECTION ("state round-trips through saveState/loadState")
    {
        std::vector<uint8_t> blob;
        REQUIRE (inst.saveState (blob));
        REQUIRE_FALSE (blob.empty());

        // A fresh instance fed the saved state must accept it; the blob is
        // Turtle, so it should at least mention the plugin's state subject.
        lv2::Lv2Instance other;
        REQUIRE (other.create (bundle, uri, err));
        REQUIRE (other.activate (48000.0, kBlock, err));
        REQUIRE (other.loadState (blob));

        // And the restored instance's own save must be non-empty again.
        std::vector<uint8_t> blob2;
        REQUIRE (other.saveState (blob2));
        REQUIRE_FALSE (blob2.empty());
    }

    SECTION ("reactivate keeps state (fresh save matches post-reactivate save)")
    {
        // Same rate, different block size: a rate change may legitimately alter a
        // plugin's state:interface blob (cached coefficients, sample-domain values),
        // so byte-equality is only a fair assertion across a block-size change.
        std::vector<uint8_t> before;
        REQUIRE (inst.saveState (before));
        REQUIRE (inst.reactivate (48000.0, kBlock * 2, err));
        REQUIRE (inst.isActive());
        std::vector<uint8_t> after;
        REQUIRE (inst.saveState (after));
        REQUIRE (before == after);
    }
}
