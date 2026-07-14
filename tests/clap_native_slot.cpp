// Increment 3 foundation: the aux NativeClapSlot load → process → unload
// lifecycle. Gated on DUSKSTUDIO_TEST_CLAP=/path/to.clap (e.g. ~/.clap/DuskVerb.clap)
// so CI without a CLAP plugin stays green. See docs/native-clap-host-plan.md.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/clap/NativeClapSlot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

TEST_CASE ("NativeClapSlot loads, processes, and unloads cleanly", "[clap][slot]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-slot test");
        return;
    }

    constexpr int kBlock = 512;
    duskstudio::clap::NativeClapSlot slot;
    std::string err;

    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err));
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() != nullptr);

    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);


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

    SECTION ("MIDI-binding writes reach the parameter surface")
    {
        // queueParamBinding is the audio-thread half of a binding apply;
        // drainQueuedParamBindings is the engine timer's message-thread half.
        // The value lands plugin-side on the next process block (UI→RT ring).
        int targetIdx = -1;
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* p = slot.paramInfo (i);
            REQUIRE (p != nullptr);
            if ((p->flags & CLAP_PARAM_IS_STEPPED) == 0 && p->maxValue > p->minValue)
            { targetIdx = i; break; }
        }
        REQUIRE (targetIdx >= 0);
        const auto* target = slot.paramInfo (targetIdx);

        slot.queueParamBinding ((uint32_t) targetIdx, 1.0f);   // frac 1 → param max
        slot.drainQueuedParamBindings();
        std::fill (inL.begin(), inL.end(), 0.0f);
        std::fill (inR.begin(), inR.end(), 0.0f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);

        double v = 0.0;
        REQUIRE (slot.getParamValue (target->id, v));
        const double range = target->maxValue - target->minValue;
        REQUIRE_THAT (v, Catch::Matchers::WithinAbs (target->maxValue, 1.0e-4 * range + 1.0e-9));
    }

    SECTION ("loaded: signal passes through, output finite + non-silent")
    {
        double phase = 0.0;
        constexpr double kPi = 3.14159265358979323846;   // M_PI is non-standard
        const double dw = 2.0 * kPi * 1000.0 / 48000.0;
        float peak = 0.0f;
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < kBlock; ++i) { const float s = 0.25f * (float) std::sin (phase); phase += dw; inL[(size_t) i] = inR[(size_t) i] = s; }
            slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
                peak = std::max ({ peak, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
            }
        }
        REQUIRE (peak > 1.0e-3f);
    }

    SECTION ("unloaded: process clears the outputs and is a safe no-op")
    {
        slot.unload();
        REQUIRE_FALSE (slot.isLoaded());
        REQUIRE (slot.getInstance() == nullptr);

        std::fill (outL.begin(), outL.end(), 1.0f);   // dirty the buffers
        std::fill (outR.begin(), outR.end(), 1.0f);
        std::fill (inL.begin(),  inL.end(),  0.5f);
        std::fill (inR.begin(),  inR.end(),  0.5f);
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (outL[(size_t) i] == 0.0f);   // cleared, not leaked
            REQUIRE (outR[(size_t) i] == 0.0f);
        }
    }
}

TEST_CASE ("NativeClapSlot reactivate keeps the same instance (no destroy)", "[clap][slot][reactivate]")
{
    // Regression: changing the global oversampling factor (or device rate) re-prepares
    // the strip. The old code did a full reload, destroying the instance the editor's
    // GUI was attached to — plugin->destroy with a live GUI aborts u-he plugins
    // ("host forgot to destroy the gui" → terminate). reactivate must re-activate the
    // SAME instance so the GUI and parameter state survive.
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-reactivate test");
        return;
    }

    duskstudio::clap::NativeClapSlot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (path), 48000.0, 512, err));
    auto* before = slot.getInstance();
    REQUIRE (before != nullptr);

    // Re-activate at a different rate + block size (4× → 1× changes both for an
    // oversampled chain). The instance pointer must be identical afterwards.
    REQUIRE (slot.reactivate (96000.0, 256, err));
    REQUIRE (slot.isLoaded());
    REQUIRE (slot.getInstance() == before);   // not torn down + rebuilt

    constexpr int kBlock = 256;
    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);
    double phase = 0.0;
    constexpr double kPi = 3.14159265358979323846;
    const double dw = 2.0 * kPi * 1000.0 / 96000.0;
    float peak = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        for (int i = 0; i < kBlock; ++i) { const float s = 0.25f * (float) std::sin (phase); phase += dw; inL[(size_t) i] = inR[(size_t) i] = s; }
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (std::isfinite (outL[(size_t) i]));
            REQUIRE (std::isfinite (outR[(size_t) i]));
            peak = std::max ({ peak, std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i]) });
        }
    }
    REQUIRE (peak > 1.0e-3f);
}

TEST_CASE ("NativeClapSlot state round-trips into a fresh slot", "[clap][slot][state]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set — skipping live CLAP-state test");
        return;
    }

    constexpr int kBlock = 512;
    const std::filesystem::path bundle = std::filesystem::u8path (path);

    duskstudio::clap::NativeClapSlot a;
    std::string err;
    REQUIRE (a.load (bundle, 48000.0, kBlock, err));

    std::vector<uint8_t> blob;
    if (! a.saveState (blob))
    {
        SUCCEED ("plugin has no CLAP state extension — nothing to round-trip");
        return;
    }
    REQUIRE_FALSE (blob.empty());

    // Restore into a fresh slot and confirm it loads + processes finite audio.
    duskstudio::clap::NativeClapSlot b;
    REQUIRE (b.load (bundle, 48000.0, kBlock, err));
    REQUIRE (b.loadState (blob));

    std::vector<float> inL ((size_t) kBlock, 0.1f), inR ((size_t) kBlock, 0.1f),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);
    for (int blk = 0; blk < 8; ++blk)
        b.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE (std::isfinite (outL[(size_t) i]));
        REQUIRE (std::isfinite (outR[(size_t) i]));
    }
}
