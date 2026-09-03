// Increment 3 foundation: the aux NativeClapSlot load -> process -> unload
// lifecycle. DUSKSTUDIO_TEST_CLAP selects a local plugin for the broad smoke
// test; the state regression below defaults to the bundled fixture.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/clap/NativeClapSlot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::string readSourceFile (const char* relativePath)
{
    std::ifstream input (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relativePath);
    return { std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>() };
}

bool nearestGateIncludes (const std::string& source, const char* marker,
                          const char* requiredMacro)
{
    const auto markerPos = source.find (marker);
    if (markerPos == std::string::npos) return false;
    const auto gatePos = source.rfind ("#if", markerPos);
    if (gatePos == std::string::npos) return false;
    return source.substr (gatePos, markerPos - gatePos).find (requiredMacro)
        != std::string::npos;
}

struct TestClapBundle
{
    std::filesystem::path path;
    bool isBundledFixture = false;
};

TestClapBundle testClapBundle()
{
    const char* overridePath = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (overridePath != nullptr && *overridePath != '\0')
        return { std::filesystem::u8path (overridePath), false };
    return { std::filesystem::u8path (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH), true };
}
} // namespace

TEST_CASE ("NativeClapSlot loads, processes, and unloads cleanly", "[clap][slot]")
{
    const char* path = std::getenv ("DUSKSTUDIO_TEST_CLAP");
    if (path == nullptr || *path == '\0')
    {
        SUCCEED ("DUSKSTUDIO_TEST_CLAP not set - skipping live CLAP-slot test");
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

    SECTION ("loaded: processing keeps every output sample finite")
    {
        double phase = 0.0;
        constexpr double kPi = 3.14159265358979323846;   // M_PI is non-standard
        const double dw = 2.0 * kPi * 1000.0 / 48000.0;
        for (int b = 0; b < 40; ++b)
        {
            for (int i = 0; i < kBlock; ++i) { const float s = 0.25f * (float) std::sin (phase); phase += dw; inL[(size_t) i] = inR[(size_t) i] = s; }
            slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                REQUIRE (std::isfinite (outL[(size_t) i]));
                REQUIRE (std::isfinite (outR[(size_t) i]));
            }
        }
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

TEST_CASE ("NativeClapSlot reactivate keeps the same instance and signal path",
           "[clap][slot][reactivate][regression][issue-397]")
{
    // Regression: changing the global oversampling factor (or device rate) re-prepares
    // the strip. The old code did a full reload, destroying the instance the editor's
    // GUI was attached to — plugin->destroy with a live GUI aborts u-he plugins
    // ("host forgot to destroy the gui" → terminate). reactivate must re-activate the
    // SAME instance so the GUI and parameter state survive.
    duskstudio::clap::NativeClapSlot slot;
    std::string err;
    REQUIRE (slot.load (std::filesystem::u8path (DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH),
                        48000.0, 512, err));
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
    float peakOut = 0.0f;
    float maxError = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        for (int i = 0; i < kBlock; ++i) { const float s = 0.25f * (float) std::sin (phase); phase += dw; inL[(size_t) i] = inR[(size_t) i] = s; }
        slot.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            REQUIRE (std::isfinite (outL[(size_t) i]));
            REQUIRE (std::isfinite (outR[(size_t) i]));
            peakOut = std::max ({ peakOut, std::abs (outL[(size_t) i]),
                                  std::abs (outR[(size_t) i]) });
            maxError = std::max ({ maxError,
                                   std::abs (outL[(size_t) i] - inL[(size_t) i]),
                                   std::abs (outR[(size_t) i] - inR[(size_t) i]) });
        }
    }
    REQUIRE (peakOut > 0.2f);
    REQUIRE (maxError < 1.0e-7f);
}

TEST_CASE ("NativeClapSlot state round-trips into a fresh slot",
           "[clap][slot][state][regression][issue-355]")
{
    const auto testBundle = testClapBundle();
    constexpr int kBlock = 512;

    duskstudio::clap::NativeClapSlot a;
    std::string err;
    REQUIRE (a.load (testBundle.path, 48000.0, kBlock, err));

    int targetIdx = -1;
    double changedValue = 0.0;
    for (int i = 0; i < a.paramCount(); ++i)
    {
        const auto* p = a.paramInfo (i);
        double current = 0.0;
        if (p != nullptr && (p->flags & CLAP_PARAM_IS_READONLY) == 0
            && p->maxValue > p->minValue && a.getParamValue (p->id, current))
        {
            targetIdx = i;
            changedValue = std::abs (current - p->minValue)
                               > 0.5 * (p->maxValue - p->minValue)
                             ? p->minValue : p->maxValue;
            a.setParamValue (p->id, changedValue);
            break;
        }
    }
    if (targetIdx < 0 && ! testBundle.isBundledFixture)
    {
        SUCCEED ("plugin exposes no writable ranged parameter - skipping state round-trip");
        return;
    }

    std::vector<float> inL ((size_t) kBlock), inR ((size_t) kBlock),
                       outL ((size_t) kBlock), outR ((size_t) kBlock);
    std::fill (inL.begin(), inL.end(), 0.1f);
    std::fill (inR.begin(), inR.end(), 0.1f);
    a.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);

    const auto* target = targetIdx >= 0 ? a.paramInfo (targetIdx) : nullptr;
    double savedValue = 0.0;
    double tolerance = 0.0;
    if (target != nullptr)
    {
        REQUIRE (a.getParamValue (target->id, savedValue));
        tolerance = 1.0e-5 * (target->maxValue - target->minValue) + 1.0e-9;
        REQUIRE_THAT (savedValue, Catch::Matchers::WithinAbs (changedValue, tolerance));
    }

    std::vector<uint8_t> blob;
    if (! a.saveState (blob))
    {
        SUCCEED ("plugin has no CLAP state extension — nothing to round-trip");
        return;
    }
    REQUIRE_FALSE (blob.empty());

    // Restore into a fresh slot and confirm it loads + processes finite audio.
    duskstudio::clap::NativeClapSlot b;
    REQUIRE (b.load (testBundle.path, 48000.0, kBlock, err));
    REQUIRE (b.loadState (blob));

    if (target != nullptr)
    {
        double restoredValue = 0.0;
        REQUIRE (b.getParamValue (target->id, restoredValue));
        REQUIRE_THAT (restoredValue, Catch::Matchers::WithinAbs (savedValue, tolerance));
    }

    if (testBundle.isBundledFixture)
    {
        std::vector<uint8_t> restoredState;
        REQUIRE (b.saveState (restoredState));
        REQUIRE (restoredState == blob);
    }

    for (int blk = 0; blk < 8; ++blk)
        b.processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        REQUIRE (std::isfinite (outL[(size_t) i]));
        REQUIRE (std::isfinite (outR[(size_t) i]));
    }
}

TEST_CASE ("Linux track clone keeps native CLAP identity and state",
           "[clap][slot][state][regression][issue-462]")
{
    const auto cloneSource = readSourceFile ("src/session/RegionEditActions.cpp");
    REQUIRE_FALSE (cloneSource.empty());
    REQUIRE (nearestGateIncludes (cloneSource, "bool liveNativeCaptured",
                                  "DUSKSTUDIO_HAS_NATIVE_CLAP"));
    REQUIRE (nearestGateIncludes (cloneSource,
                                  "Replay exactly one native owner",
                                  "DUSKSTUDIO_HAS_NATIVE_CLAP"));
    REQUIRE (cloneSource.find ("if (primed && msStateRestored)")
             != std::string::npos);

    constexpr int kBlock = 64;
    const auto bundle = std::filesystem::u8path (
        DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH);
    duskstudio::clap::NativeClapSlot source;
    std::string error;
    REQUIRE (source.load (bundle, 48000.0, kBlock, error));

    std::vector<float> left ((size_t) kBlock, 0.0f);
    std::vector<float> right ((size_t) kBlock, 0.0f);
    for (int block = 0; block < 3; ++block)
        source.processStereo (left.data(), right.data(), left.data(), right.data(), kBlock);

    const auto capturedPath = source.getPath();
    const auto capturedId = source.getPluginId();
    std::vector<uint8_t> capturedState;
    REQUIRE (source.saveState (capturedState));
    REQUIRE_FALSE (capturedState.empty());

    duskstudio::clap::NativeClapSlot destination;
    REQUIRE (destination.load (bundle, 48000.0, kBlock, error));
    destination.processStereo (left.data(), right.data(), left.data(), right.data(), kBlock);
    std::vector<uint8_t> staleState;
    REQUIRE (destination.saveState (staleState));
    REQUIRE (staleState != capturedState);

    REQUIRE (destination.load (std::filesystem::u8path (capturedPath), 48000.0,
                               kBlock, error, capturedId));
    REQUIRE (destination.loadState (capturedState));
    REQUIRE (destination.getPath() == capturedPath);
    REQUIRE (destination.getPluginId() == capturedId);

    std::vector<uint8_t> replayedState;
    REQUIRE (destination.saveState (replayedState));
    REQUIRE (replayedState == capturedState);
}
