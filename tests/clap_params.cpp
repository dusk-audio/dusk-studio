// Increment 4: CLAP parameter enumeration + read + set-via-events. Gated on
// DUSKSTUDIO_TEST_CLAP=/path/to.clap (e.g. ~/.clap/DuskVerb.clap) so CI without a
// CLAP plugin stays green. See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>

#include "engine/clap/NativeClapSlot.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>

using duskstudio::clap::NativeClapSlot;
using ParamInfo = duskstudio::clap::ClapInstance::ParamInfo;

TEST_CASE ("CLAP params enumerate, read, and round-trip a change through process()", "[clap][params]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-params test");
        return;
    }

    constexpr int kBlock = 512;
    NativeClapSlot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err));

    const int n = slot.paramCount();
    // A valid effect can legitimately expose zero parameters — skip rather than fail.
    if (n == 0)
    {
        SUCCEED ("plugin exposes no parameters — nothing to enumerate");
        return;
    }

    SECTION ("every param info is well-formed")
    {
        for (int i = 0; i < n; ++i)
        {
            const auto* pi = slot.paramInfo (i);
            REQUIRE (pi != nullptr);
            REQUIRE_FALSE (pi->name.empty());
            REQUIRE (std::isfinite (pi->minValue));
            REQUIRE (std::isfinite (pi->maxValue));
            REQUIRE (std::isfinite (pi->defaultValue));
            REQUIRE (pi->maxValue >= pi->minValue);
            REQUIRE (pi->defaultValue >= pi->minValue);
            REQUIRE (pi->defaultValue <= pi->maxValue);

            double v = 0.0;
            REQUIRE (slot.getParamValue (pi->id, v));   // live read works for every param
            REQUIRE (std::isfinite (v));
        }
    }

    SECTION ("setParamValue is applied to the plugin by the next process() block")
    {
        // Any automatable, writable param with a real range. We don't assume it's
        // continuous (some choice params expose a 0..1 range and quantise), so we
        // drive to the far extreme and assert the value moved toward it.
        const ParamInfo* target = nullptr;
        for (int i = 0; i < n; ++i)
        {
            const auto* pi = slot.paramInfo (i);
            const bool automatable = (pi->flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
            const bool readonly    = (pi->flags & CLAP_PARAM_IS_READONLY) != 0;
            if (automatable && ! readonly && pi->maxValue > pi->minValue)
            { target = pi; break; }
        }
        if (target == nullptr)
        {
            SUCCEED ("plugin exposes no automatable writable param — nothing to drive");
            return;
        }

        double cur = 0.0;
        REQUIRE (slot.getParamValue (target->id, cur));

        // Push toward whichever extreme is farther from the current value, so even a
        // coarsely-quantised param is guaranteed to land on a different value.
        const double mid  = target->minValue + 0.5 * (target->maxValue - target->minValue);
        const double want = (cur <= mid) ? target->maxValue : target->minValue;

        slot.setParamValue (target->id, want);

        // Drive several blocks so the ring drains and the plugin consumes the event.
        std::vector<float> inL ((size_t) kBlock, 0.0f), inR ((size_t) kBlock, 0.0f),
                           outL ((size_t) kBlock), outR ((size_t) kBlock);
        for (int b = 0; b < 8; ++b)
            slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);

        double after = 0.0;
        REQUIRE (slot.getParamValue (target->id, after));
        // The event drove the param from cur toward want (applied, not ignored).
        REQUIRE (std::abs (after - want) < std::abs (cur - want));
    }
}
