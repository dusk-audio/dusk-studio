// NativeVst3Slot lifecycle: load a VST3 effect, process audio through the slot
// (which routes InsertAdapter → processBlock), unload, reactivate. Gated on
// DUSKSTUDIO_TEST_VST3=/path/to.vst3 so CI without a VST3 plugin stays green.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/vst3/NativeVst3Slot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
float driveTone (duskstudio::vst3::NativeVst3Slot& slot, std::vector<float>& L,
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

TEST_CASE ("NativeVst3Slot loads, processes, and unloads cleanly", "[vst3][slot]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_VST3");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_VST3 not set — skipping live VST3-slot test");
        return;
    }

    using Catch::Matchers::WithinAbs;
    duskstudio::vst3::NativeVst3Slot slot;
    std::string err;
    constexpr int kBlock = 256;

    if (! slot.load (juce::File (juce::String (path)), 48000.0, kBlock, err))
    {
        SUCCEED ("module has no loadable audio effect (" + err + ") — skipping");
        return;
    }
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() != nullptr);

    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);

    SECTION ("signal in produces finite non-silent output")
    {
        REQUIRE (driveTone (slot, L, R, kBlock, 32) > 1.0e-4f);
    }

    SECTION ("bypass passes audio through untouched")
    {
        slot.setBypassed (true);
        std::fill (L.begin(), L.end(), 0.25f);
        std::fill (R.begin(), R.end(), -0.25f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE_THAT (L[(size_t) i], WithinAbs (0.25, 1.0e-9));
            REQUIRE_THAT (R[(size_t) i], WithinAbs (-0.25, 1.0e-9));
        }
        slot.setBypassed (false);
    }

    SECTION ("state round-trips through the slot")
    {
        std::vector<uint8_t> blob;
        REQUIRE (slot.saveState (blob));
        REQUIRE_FALSE (blob.empty());
        REQUIRE (slot.loadState (blob));
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
        REQUIRE (slot.reactivate (44100.0, kBlock, err));
        REQUIRE (slot.isLoaded());
        REQUIRE (driveTone (slot, L, R, kBlock, 16) > 1.0e-4f);
    }

    SECTION ("MIDI-binding writes reach the parameter surface")
    {
        // queueParamBinding is the audio-thread half of a binding apply;
        // drainQueuedParamBindings is the engine timer's message-thread half.
        int targetIdx = -1;
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* p = slot.paramInfo (i);
            REQUIRE (p != nullptr);
            if (p->stepCount == 0 && p->canAutomate && ! p->isReadOnly)
            { targetIdx = i; break; }
        }
        REQUIRE (targetIdx >= 0);
        const auto id = slot.paramInfo (targetIdx)->id;

        slot.queueParamBinding ((uint32_t) targetIdx, 0.75f);
        slot.drainQueuedParamBindings();

        double v = -1.0;
        REQUIRE (slot.getParamValue (id, v));
        REQUIRE_THAT (v, WithinAbs (0.75, 1.0e-6));
    }
}
