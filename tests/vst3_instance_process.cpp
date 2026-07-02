// Instantiate a real VST3 effect and process audio offline THROUGH the
// InsertAdapter — the same generalized path the DSP call sites use — proving the
// host-agnostic foundation covers its third format. Gated on
// DUSKSTUDIO_TEST_VST3=/path/to.vst3.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/vst3/Vst3Bundle.h"
#include "engine/vst3/Vst3HostContext.h"
#include "engine/vst3/Vst3Instance.h"

#include <pluginterfaces/vst/ivsteditcontroller.h>

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

    SECTION ("state round-trips into a fresh instance")
    {
        std::vector<uint8_t> blob;
        REQUIRE (inst.saveState (blob));
        REQUIRE (blob.size() >= 12);   // magic + two length prefixes

        vst3::Vst3Instance inst2;
        REQUIRE (inst2.create (bundle, classId, err));
        REQUIRE (inst2.loadState (blob));

        // Truncated and foreign blobs must be rejected, not crash.
        std::vector<uint8_t> truncated (blob.begin(), blob.begin() + 6);
        REQUIRE_FALSE (inst2.loadState (truncated));
        std::vector<uint8_t> foreign = { 'n', 'o', 'p', 'e', 0, 0, 0, 0 };
        REQUIRE_FALSE (inst2.loadState (foreign));

        // The restored instance still activates and processes.
        REQUIRE (inst2.activate (48000.0, kBlock, err));
        hosting::InsertAdapter adapter2;
        adapter2.prepare (inst2.portLayout(), kBlock);
        std::fill (L.begin(), L.end(), 0.1f);
        std::fill (R.begin(), R.end(), 0.1f);
        adapter2.process (inst2, L.data(), R.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            REQUIRE (std::isfinite (L[(size_t) i]));
    }

    SECTION ("parameters enumerate and round-trip through the controller")
    {
        REQUIRE (inst.paramCount() > 0);

        // First continuous, automatable, writable parameter (stepped params
        // quantize the normalized value, breaking an exact round-trip check).
        const vst3::Vst3Instance::ParamInfo* target = nullptr;
        for (int i = 0; i < inst.paramCount(); ++i)
        {
            const auto* p = inst.paramInfo (i);
            REQUIRE (p != nullptr);
            REQUIRE_FALSE (p->name.empty());
            if (target == nullptr && p->stepCount == 0 && p->canAutomate && ! p->isReadOnly)
                target = p;
        }
        REQUIRE (target != nullptr);

        // Host set → controller read-back.
        inst.setParamValue (target->id, 0.25);
        double v = -1.0;
        REQUIRE (inst.getParamValue (target->id, v));
        REQUIRE_THAT (v, Catch::Matchers::WithinAbs (0.25, 1.0e-6));

        // The queued change reaches the processor without disturbing the audio path.
        std::fill (L.begin(), L.end(), 0.1f);
        std::fill (R.begin(), R.end(), 0.1f);
        adapter.process (inst, L.data(), R.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            REQUIRE (std::isfinite (L[(size_t) i]));

        std::string text;
        if (inst.paramValueToText (target->id, 0.25, text))
            REQUIRE_FALSE (text.empty());
    }

    SECTION ("restartComponent: latency flag consumes once, param-info refreshes")
    {
        // Drive the handler exactly as a plugin would.
        auto* handler = static_cast<Steinberg::Vst::IComponentHandler*> (
            inst.getHost().componentHandler());
        REQUIRE (handler != nullptr);

        REQUIRE_FALSE (inst.consumeLatencyChanged());
        handler->restartComponent (Steinberg::Vst::RestartFlags::kLatencyChanged);
        REQUIRE (inst.consumeLatencyChanged());
        REQUIRE_FALSE (inst.consumeLatencyChanged());

        const int before = inst.paramCount();
        REQUIRE (before > 0);
        handler->restartComponent (Steinberg::Vst::RestartFlags::kParamTitlesChanged);
        inst.refreshParamInfoIfChanged();
        REQUIRE (inst.paramCount() == before);   // rebuilt from the same controller
        REQUIRE (inst.paramInfo (0) != nullptr);
        REQUIRE_FALSE (inst.paramInfo (0)->name.empty());
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
