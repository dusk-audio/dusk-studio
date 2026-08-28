// NativeLv2Slot lifecycle: load an LV2 effect, process audio through the slot
// (which routes InsertAdapter → processBlock), unload, reactivate. Gated on
// DUSKSTUDIO_TEST_LV2=/path/to/bundle.lv2 so CI without an LV2 plugin stays green.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/lv2/NativeLv2Slot.h"
#include "foundation/Base64.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
// The app runs the plugin and then drains its patch:Set replies on a message-thread
// timer (AudioEngine's parameter poll). Parameters that ride atom messages only reach
// the host's value surface through that pair, so a state test has to do both.
void settleParamSurface (duskstudio::lv2::NativeLv2Slot& slot, int block, int blocks)
{
    std::vector<float> L ((size_t) block, 0.0f), R ((size_t) block, 0.0f);
    for (int b = 0; b < blocks; ++b)
    {
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), block);
        if (auto* instance = slot.getInstance())
            instance->drainPatchFeedback();
    }
}

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

    if (! slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err))
    {
        SUCCEED ("bundle has no loadable audio effect (" + err + ") — skipping");
        return;
    }
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() != nullptr);

    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);

    SECTION ("signal in produces finite non-silent output")
    {
        const float peak = driveTone (slot, L, R, kBlock, 32);
        if (! slot.isLoadedInstrument())
            REQUIRE (peak > 1.0e-4f);
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
        const float peak = driveTone (slot, L, R, kBlock, 16);
        if (! slot.isLoadedInstrument())
            REQUIRE (peak > 1.0e-4f);
    }


    SECTION ("explicit plugin id: round-trips, bogus id refuses to load")
    {
        const std::string pickedId = slot.getPluginId();
        REQUIRE (! pickedId.empty());

        slot.unload();
        REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err, pickedId));
        REQUIRE (slot.getPluginId() == pickedId);

        slot.unload();
        REQUIRE_FALSE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err,
                                  "urn:duskstudio:no-such-plugin"));
        REQUIRE_FALSE (slot.isLoaded());
    }

    SECTION ("patch-property writes reach the plugin, not just the shadow")
    {
        // The shadow read-back in getParamValue can't prove the patch:Set atom
        // was injected into the control atom port — the plugin's own saved
        // state can: it changes only if the DSP side heard the event.
        auto* inst = slot.getInstance();
        int patchIdx = -1;
        for (int i = 0; i < inst->paramCount(); ++i)
        {
            const auto* p = inst->paramInfo (i);
            if (p->isPatchProperty && ! p->stepped && p->maxValue > p->minValue)
            { patchIdx = i; break; }
        }
        if (patchIdx < 0)
        {
            SUCCEED ("plugin exposes no continuous patch properties — skipping");
            return;
        }
        const auto* target = inst->paramInfo (patchIdx);

        std::vector<uint8_t> before, after;
        REQUIRE (slot.saveState (before));

        inst->setParamValue (target->id, (double) target->maxValue);
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        for (int b = 0; b < 4; ++b)
            slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);

        REQUIRE (slot.saveState (after));
        REQUIRE (before != after);
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
            SUCCEED ("plugin exposes no continuous parameters — skipping");
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

TEST_CASE ("NativeLv2Slot state round-trips into a fresh slot",
           "[lv2][slot][state][regression][issue-355]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_LV2");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_LV2 not set — skipping live LV2-state test");
        return;
    }

    constexpr int kBlock = 256;
    const std::filesystem::path bundle = std::filesystem::u8path (path);
    duskstudio::lv2::NativeLv2Slot source;
    std::string err;
    REQUIRE (source.load (bundle, 48000.0, kBlock, err));

    int targetIdx = -1;
    double changedValue = 0.0;
    for (int i = 0; i < source.paramCount(); ++i)
    {
        const auto* p = source.paramInfo (i);
        double current = 0.0;
        if (p != nullptr && p->maxValue > p->minValue
            && source.getParamValue (p->id, current))
        {
            targetIdx = i;
            changedValue = std::abs (current - p->minValue)
                               > 0.5 * ((double) p->maxValue - (double) p->minValue)
                             ? p->minValue : p->maxValue;
            source.setParamValue (p->id, changedValue);
            break;
        }
    }
    REQUIRE (targetIdx >= 0);

    std::vector<float> left ((size_t) kBlock), right ((size_t) kBlock);
    settleParamSurface (source, kBlock, 4);
    const auto* target = source.paramInfo (targetIdx);
    REQUIRE (target != nullptr);
    double savedValue = 0.0;
    REQUIRE (source.getParamValue (target->id, savedValue));
    const double tolerance = 1.0e-5 * ((double) target->maxValue - (double) target->minValue)
                             + 1.0e-9;
    REQUIRE_THAT (savedValue, Catch::Matchers::WithinAbs (changedValue, tolerance));

    std::vector<uint8_t> blob;
    REQUIRE (source.saveState (blob));
    REQUIRE_FALSE (blob.empty());

    duskstudio::lv2::NativeLv2Slot restored;
    REQUIRE (restored.load (bundle, 48000.0, kBlock, err, source.getPluginId()));
    REQUIRE (restored.loadState (blob));
    settleParamSurface (restored, kBlock, 4);
    // Patch-property ids embed a runtime URID, which is assigned per instance and
    // is NOT comparable across two of them. Index is the stable identity - the
    // same one MIDI bindings persist.
    const auto* restoredParam = restored.paramInfo (targetIdx);
    REQUIRE (restoredParam != nullptr);
    REQUIRE (restoredParam->name == target->name);
    double restoredValue = 0.0;
    REQUIRE (restored.getParamValue (restoredParam->id, restoredValue));
    INFO ("param '" << target->name << "' patchProperty=" << target->isPatchProperty
          << " range " << target->minValue << ".." << target->maxValue);
    REQUIRE_THAT (restoredValue, Catch::Matchers::WithinAbs (savedValue, tolerance));
}

TEST_CASE ("NativeLv2Slot state round-trips through a state directory",
           "[lv2][slot][state][regression][issue-355]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_LV2");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_LV2 not set - skipping live LV2-state test");
        return;
    }

    // The engine always hands a track's LV2 slot a state directory, which selects
    // a completely different save path to the blob-only one above.
    duskstudio::test::TempDirectory temp ("dusk-lv2-statedir-");
    const auto& stateDir = temp.path();

    constexpr int kBlock = 256;
    const std::filesystem::path bundle = std::filesystem::u8path (path);
    duskstudio::lv2::NativeLv2Slot source;
    std::string err;
    REQUIRE (source.load (bundle, 48000.0, kBlock, err));
    source.setStateDirectory (stateDir);

    int targetIdx = -1;
    double changedValue = 0.0;
    for (int i = 0; i < source.paramCount(); ++i)
    {
        const auto* p = source.paramInfo (i);
        double current = 0.0;
        if (p != nullptr && p->maxValue > p->minValue
            && source.getParamValue (p->id, current))
        {
            targetIdx = i;
            changedValue = std::abs (current - p->minValue)
                               > 0.5 * ((double) p->maxValue - (double) p->minValue)
                             ? p->minValue : p->maxValue;
            source.setParamValue (p->id, changedValue);
            break;
        }
    }
    REQUIRE (targetIdx >= 0);

    settleParamSurface (source, kBlock, 4);
    const auto* target = source.paramInfo (targetIdx);
    REQUIRE (target != nullptr);
    double savedValue = 0.0;
    REQUIRE (source.getParamValue (target->id, savedValue));

    std::vector<uint8_t> blob;
    REQUIRE (source.saveState (blob));
    REQUIRE_FALSE (blob.empty());

    duskstudio::lv2::NativeLv2Slot restored;
    REQUIRE (restored.load (bundle, 48000.0, kBlock, err, source.getPluginId()));
    restored.setStateDirectory (stateDir);
    // The session carries the blob as base64, so round-trip it the way a save and
    // reopen does rather than handing the bytes straight back.
    const auto encoded = dusk::base64::encode (blob.data(), blob.size());
    const auto decoded = dusk::base64::decode (encoded.data(), encoded.size());
    REQUIRE (decoded == blob);

    REQUIRE (restored.loadState (decoded));
    settleParamSurface (restored, kBlock, 4);
    const auto* restoredParam = restored.paramInfo (targetIdx);   // see the id note above
    REQUIRE (restoredParam != nullptr);
    double restoredValue = 0.0;
    REQUIRE (restored.getParamValue (restoredParam->id, restoredValue));
    const double tolerance = 1.0e-5 * ((double) target->maxValue - (double) target->minValue)
                             + 1.0e-9;
    INFO ("param '" << target->name << "' patchProperty=" << target->isPatchProperty);
    REQUIRE_THAT (restoredValue, Catch::Matchers::WithinAbs (savedValue, tolerance));

}
