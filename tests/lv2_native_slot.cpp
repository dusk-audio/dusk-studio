// NativeLv2Slot lifecycle: load an LV2 effect, process audio through the slot
// (which routes InsertAdapter → processBlock), unload, reactivate. Gated on
// DUSKSTUDIO_TEST_LV2=/path/to/bundle.lv2 so CI without an LV2 plugin stays green.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/lv2/NativeLv2Slot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
float driveTone (duskstudio::lv2::NativeLv2Slot& slot, std::vector<float>& L,
                 std::vector<float>& R, int block, int blocks)
{
    constexpr double kPi = 3.14159265358979323846;
    double phase = 0.0;
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const float s = 0.3f * (float) std::sin (phase);
            phase += 2.0 * kPi * 440.0 / 48000.0;
            L[(size_t) i] = s;
            R[(size_t) i] = 0.7f * s;
        }
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), block);   // in-place
        for (int i = 0; i < block; ++i)
        {
            REQUIRE (std::isfinite (L[(size_t) i]));
            REQUIRE (std::isfinite (R[(size_t) i]));
            peak = std::max ({ peak, std::abs (L[(size_t) i]), std::abs (R[(size_t) i]) });
        }
    }
    return peak;
}
} // namespace

TEST_CASE ("NativeLv2Slot loads, processes, and unloads cleanly", "[lv2][slot]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_LV2");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_LV2 not set — skipping live LV2-slot test");
        return;
    }

    using Catch::Matchers::WithinAbs;
    duskstudio::lv2::NativeLv2Slot slot;
    std::string err;
    constexpr int kBlock = 256;

    if (! slot.load (juce::File (juce::String (path)), 48000.0, kBlock, err))
    {
        SUCCEED ("bundle has no loadable audio effect (" + err + ") — skipping");
        return;
    }
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() != nullptr);

    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);

    SECTION ("signal in produces finite non-silent output")
    {
        REQUIRE (driveTone (slot, L, R, kBlock, 32) > 1.0e-4f);
    }

    SECTION ("unload clears outputs, doesn't leak stale audio")
    {
        slot.unload();
        REQUIRE_FALSE (slot.isLoaded());
        REQUIRE (slot.getInstance() == nullptr);

        std::fill (L.begin(), L.end(), 9.0f);
        std::fill (R.begin(), R.end(), 9.0f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE_THAT (L[(size_t) i], WithinAbs (0.0, 1.0e-9));
            REQUIRE_THAT (R[(size_t) i], WithinAbs (0.0, 1.0e-9));
        }
    }

    SECTION ("reactivate keeps processing")
    {
        REQUIRE (slot.reactivate (48000.0, kBlock, err));
        REQUIRE (slot.isLoaded());
        REQUIRE (driveTone (slot, L, R, kBlock, 16) > 1.0e-4f);
    }

    SECTION ("MIDI-binding writes reach the parameter surface")
    {
        // queueParamBinding is the audio-thread half of a binding apply;
        // drainQueuedParamBindings is the engine timer's message-thread half.
        // The value lands port-side on the next process block (UI→RT ring).
        int targetIdx = -1;
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* p = slot.paramInfo (i);
            REQUIRE (p != nullptr);
            REQUIRE_FALSE (p->name.empty());
            if (! p->stepped && p->maxValue > p->minValue)
            { targetIdx = i; break; }
        }
        if (targetIdx < 0)
        {
            // JUCE-wrapped LV2s carry their real parameters as atom patch
            // messages, not control ports — nothing to bind through this surface.
            SUCCEED ("plugin exposes no continuous control-port parameters — skipping");
            return;
        }
        const auto* target = slot.paramInfo (targetIdx);

        slot.queueParamBinding ((uint32_t) targetIdx, 1.0f);   // frac 1 → port max
        slot.drainQueuedParamBindings();
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);

        double v = 0.0;
        REQUIRE (slot.getParamValue (target->id, v));
        const double range = (double) target->maxValue - (double) target->minValue;
        REQUIRE_THAT (v, WithinAbs ((double) target->maxValue, 1.0e-4 * range + 1.0e-9));
    }
}
