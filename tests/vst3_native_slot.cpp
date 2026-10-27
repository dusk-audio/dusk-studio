// NativeVst3Slot lifecycle: load a VST3 effect, process audio through the slot
// (which routes InsertAdapter → processBlock), unload, reactivate. Gated on
// DUSKSTUDIO_TEST_VST3=/path/to.vst3 so CI without a VST3 plugin stays green.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/vst3/NativeVst3Slot.h"
#include "engine/hosting/NativeRestorePolicy.h"
#include "engine/vst3/Vst3HostContext.h"

#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
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

    if (! slot.load (std::filesystem::u8path (path), 48000.0, kBlock, err))
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

TEST_CASE ("NativeVst3Slot rebuilds a runtime output-bus layout",
           "[vst3][slot][regression][issue-455]")
{
    using Catch::Matchers::WithinAbs;
    duskstudio::vst3::NativeVst3Slot slot;
    std::string err;
    constexpr int kBlock = 64;

    const bool loaded = slot.load (
        std::filesystem::u8path (DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH),
        48000.0, kBlock, err);
    INFO ("fixture load error: " << err);
    REQUIRE (loaded);
    auto* instance = slot.getInstance();
    REQUIRE (instance != nullptr);
    REQUIRE (instance->portLayout().outputs.size() == 2);
    REQUIRE (instance->portLayout().outputs[1].active);

    std::vector<float> L ((size_t) kBlock, 0.25f);
    std::vector<float> R ((size_t) kBlock, -0.25f);
    slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
    REQUIRE_THAT (L.front(), WithinAbs (0.25, 1.0e-9));
    REQUIRE_THAT (R.front(), WithinAbs (-0.25, 1.0e-9));

    const duskstudio::vst3::Vst3Instance::ParamInfo* expand = nullptr;
    for (int i = 0; i < slot.paramCount(); ++i)
        if (slot.paramInfo (i)->name == "Expand Outputs")
            expand = slot.paramInfo (i);
    REQUIRE (expand != nullptr);
    slot.setParamValue (expand->id, 1.0);
    REQUIRE (instance->ioChangePending());

    std::fill (L.begin(), L.end(), 0.5f);
    std::fill (R.begin(), R.end(), -0.5f);
    slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
    REQUIRE_THAT (L.front(), WithinAbs (0.0, 1.0e-9));
    REQUIRE_THAT (R.front(), WithinAbs (0.0, 1.0e-9));

    REQUIRE (instance->consumeIoChanged());
    REQUIRE (slot.reactivate (48000.0, kBlock, err));
    REQUIRE (instance->portLayout().outputs.size() == 3);
    REQUIRE (instance->portLayout().outputs[1].active);
    REQUIRE (instance->portLayout().outputs[2].active);

    std::fill (L.begin(), L.end(), 0.75f);
    std::fill (R.begin(), R.end(), -0.75f);
    slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
    REQUIRE_THAT (L.front(), WithinAbs (0.75, 1.0e-9));
    REQUIRE_THAT (R.front(), WithinAbs (-0.75, 1.0e-9));
}

TEST_CASE ("NativeVst3Slot refreshes latency after restoring state",
           "[vst3][slot][regression][issue-456]")
{
    duskstudio::vst3::NativeVst3Slot slot;
    std::string err;
    constexpr int kBlock = 64;

    const bool loaded = slot.load (
        std::filesystem::u8path (DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH),
        48000.0, kBlock, err);
    INFO ("fixture load error: " << err);
    REQUIRE (loaded);
    REQUIRE (slot.getLatencySamples() == 0);

    const duskstudio::vst3::Vst3Instance::ParamInfo* latencyMode = nullptr;
    for (int i = 0; i < slot.paramCount(); ++i)
        if (slot.paramInfo (i)->name == "Latency Mode")
            latencyMode = slot.paramInfo (i);
    REQUIRE (latencyMode != nullptr);

    slot.setParamValue (latencyMode->id, 1.0);
    std::vector<uint8_t> highLatencyState;
    REQUIRE (slot.saveState (highLatencyState));
    REQUIRE_FALSE (highLatencyState.empty());

    slot.setParamValue (latencyMode->id, 0.0);
    REQUIRE_FALSE (slot.consumeLatencyChanged());
    REQUIRE (slot.loadState (highLatencyState));

    // Waiting for the restart that reads it, the plug-in plays on at its old latency.
    std::vector<float> L ((size_t) kBlock, 0.5f), R ((size_t) kBlock, -0.5f);
    slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
    REQUIRE (std::abs (L.front() - 0.5f) < 1.0e-6f);
    REQUIRE (std::abs (R.front() + 0.5f) < 1.0e-6f);
    REQUIRE (slot.getLatencySamples() == 0);
    REQUIRE (slot.consumeLatencyChanged());

    REQUIRE (slot.reactivate (48000.0, kBlock, err));
    REQUIRE (slot.getLatencySamples() == 64);
}

TEST_CASE ("NativeVst3Slot supplies the current transport on every processed block",
           "[vst3][slot][transport]")
{
    using Catch::Matchers::WithinAbs;
    using Context = Steinberg::Vst::ProcessContext;
    duskstudio::vst3::NativeVst3Slot slot;
    std::string error;
    constexpr int kFrames = 64;
    REQUIRE (slot.load (
        std::filesystem::u8path (DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH),
        48000.0, kFrames, error));
    const auto enableReport = [&]
    {
        for (int index = 0; index < slot.paramCount(); ++index)
            if (slot.paramInfo (index)->name == "Report Transport")
            {
                slot.setParamValue (slot.paramInfo (index)->id, 1.0);
                return true;
            }
        return false;
    };
    REQUIRE (enableReport());
    std::vector<float> left (kFrames), right (kFrames);
    const auto process = [&] (const dusk::TransportPosition* position)
    {
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);
        slot.processStereo (left.data(), right.data(), left.data(), right.data(),
                            kFrames, nullptr, position);
        for (int index = 0; index < 11; ++index)
            CHECK_THAT (right[(size_t) index], WithinAbs (left[(size_t) index], 1.0e-6f));
    };
    const auto flags = [&] { return (Steinberg::uint32) std::lround (left[8]); };
    const auto valid = Context::kProjectTimeMusicValid | Context::kBarPositionValid
                     | Context::kTempoValid | Context::kTimeSigValid;
    dusk::TransportPosition position;
    position.isPlaying = true;
    position.timeInSamples = 123456;
    position.timeInSeconds = 123456.0 / 48000.0;
    position.ppqPosition = 17.25;
    position.bpm = 93.0;
    position.timeSignatureNumerator = 6;
    position.timeSignatureDenominator = 8;
    process (&position);
    CHECK_THAT (left[0], WithinAbs (1.0f, 1.0e-6f));
    CHECK_THAT (left[1], WithinAbs (48000.0f, 1.0e-6f));
    CHECK_THAT (left[2], WithinAbs (123456.0f, 1.0e-6f));
    CHECK_THAT (left[3], WithinAbs (17.25f, 1.0e-6f));
    CHECK_THAT (left[4], WithinAbs (15.0f, 1.0e-6f));
    CHECK_THAT (left[5], WithinAbs (93.0f, 1.0e-6f));
    CHECK_THAT (left[6], WithinAbs (6.0f, 1.0e-6f));
    CHECK_THAT (left[7], WithinAbs (8.0f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) (valid | Context::kPlaying));

    position.isRecording = true;
    position.timeInSamples = 123520;
    position.ppqPosition = 17.375;
    position.bpm = 137.0;
    process (&position);
    CHECK_THAT (left[2], WithinAbs (123520.0f, 1.0e-6f));
    CHECK_THAT (left[3], WithinAbs (17.375f, 1.0e-6f));
    CHECK_THAT (left[5], WithinAbs (137.0f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) (valid | Context::kPlaying | Context::kRecording));

    position.isPlaying = false;
    position.isRecording = false;
    position.timeInSamples = -24000;
    position.ppqPosition = -0.75;
    process (&position);
    CHECK_THAT (left[2], WithinAbs (-24000.0f, 1.0e-6f));
    CHECK_THAT (left[3], WithinAbs (-0.75f, 1.0e-6f));
    CHECK_THAT (left[4], WithinAbs (-3.0f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) valid);

    position.isLooping = true;
    position.loopStartSamples = 6000;
    position.loopEndSamples = 46800;
    position.ppqLoopStart = 1.25;
    position.ppqLoopEnd = 9.75;
    process (&position);
    CHECK_THAT (left[9], WithinAbs (1.25f, 1.0e-6f));
    CHECK_THAT (left[10], WithinAbs (9.75f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) (valid | Context::kCycleValid | Context::kCycleActive));
    position.isLooping = false;
    process (&position);
    CHECK_THAT (left[9], WithinAbs (1.25f, 1.0e-6f));
    CHECK_THAT (left[10], WithinAbs (9.75f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) (valid | Context::kCycleValid));
    position.isLooping = true;
    position.ppqLoopEnd = 0.5;
    process (&position);
    CHECK_THAT (left[9], WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (left[10], WithinAbs (0.0f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) valid);
    position.isLooping = false;
    position.ppqLoopStart = position.ppqLoopEnd = 0.0;

    process (nullptr);
    for (int index = 0; index < 11; ++index)
        CHECK_THAT (left[(size_t) index], WithinAbs (0.0f, 1.0e-6f));

    REQUIRE (slot.reactivate (96000.0, kFrames, error));
    REQUIRE (enableReport());
    position.timeInSamples = 96000;
    position.ppqPosition = 2.5;
    position.timeSignatureNumerator = 7;
    position.timeSignatureDenominator = 8;
    position.isPlaying = true;
    process (&position);
    CHECK_THAT (left[1], WithinAbs (96000.0f, 1.0e-6f));
    CHECK_THAT (left[2], WithinAbs (96000.0f, 1.0e-6f));
    CHECK_THAT (left[3], WithinAbs (2.5f, 1.0e-6f));
    CHECK_THAT (left[4], WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (left[6], WithinAbs (7.0f, 1.0e-6f));
    CHECK_THAT (left[7], WithinAbs (8.0f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) (valid | Context::kPlaying));

    position.ppqPosition = std::numeric_limits<double>::quiet_NaN();
    position.bpm = std::numeric_limits<double>::infinity();
    position.timeSignatureNumerator = 0;
    process (&position);
    CHECK_THAT (left[3], WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (left[4], WithinAbs (0.0f, 1.0e-6f));
    CHECK_THAT (left[5], WithinAbs (0.0f, 1.0e-6f));
    CHECK (flags() == (Steinberg::uint32) Context::kPlaying);
}

TEST_CASE ("NativeVst3Slot keeps a bus-layout change from the plug-in's own activation only if it moved the buses",
           "[vst3][slot][regression][issue-769]")
{
    using Catch::Matchers::WithinAbs;
    duskstudio::vst3::NativeVst3Slot slot;
    std::string err;
    static constexpr int kBlock = 64;

    const bool loaded = slot.load (
        std::filesystem::u8path (DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH),
        48000.0, kBlock, err);
    INFO ("fixture load error: " << err);
    REQUIRE (loaded);
    auto* instance = slot.getInstance();
    REQUIRE (instance != nullptr);

    const auto setParam = [&slot] (const char* name, double value)
    {
        for (int i = 0; i < slot.paramCount(); ++i)
            if (slot.paramInfo (i)->name == name)
            {
                slot.setParamValue (slot.paramInfo (i)->id, value);
                return true;
            }
        return false;
    };
    std::vector<float> L ((size_t) kBlock), R ((size_t) kBlock);
    const auto passes = [&]
    {
        std::fill (L.begin(), L.end(), 0.75f);
        std::fill (R.begin(), R.end(), -0.75f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
        return std::abs (L.front() - 0.75f) < 1.0e-6f && std::abs (R.front() + 0.75f) < 1.0e-6f;
    };

    SECTION ("a repeat that leaves the buses as they were is dropped")
    {
        REQUIRE (setParam ("Repeat IO On Activate", 1.0));
        REQUIRE (setParam ("Expand Outputs", 1.0));
        REQUIRE (instance->consumeIoChanged());
        REQUIRE (slot.reactivate (48000.0, kBlock, err));
        CHECK (instance->portLayout().outputs.size() == 3);
        CHECK_FALSE (instance->ioChangePending());
        CHECK (passes());
    }

    SECTION ("a change the activation makes after the buses were read is kept")
    {
        REQUIRE (setParam ("Flip Outputs On Activate", 1.0));
        REQUIRE (slot.reactivate (48000.0, kBlock, err));
        CHECK (instance->ioChangePending());
        std::fill (L.begin(), L.end(), 0.75f);
        std::fill (R.begin(), R.end(), -0.75f);
        slot.processStereo (L.data(), R.data(), L.data(), R.data(), kBlock);
        CHECK_THAT (L.front(), WithinAbs (0.0, 1.0e-9));
        CHECK_THAT (R.front(), WithinAbs (0.0, 1.0e-9));
    }

    SECTION ("any reactivation answers a change announced before it")
    {
        REQUIRE (setParam ("Expand Outputs", 1.0));
        REQUIRE (instance->ioChangePending());
        REQUIRE (slot.reactivate (48000.0, kBlock, err));
        CHECK (instance->portLayout().outputs.size() == 3);
        CHECK_FALSE (instance->ioChangePending());
        CHECK (passes());
    }
}

TEST_CASE ("Vst3Instance detaches host interfaces before plugin termination",
           "[vst3][instance][regression][issue-455][issue-457]")
{
    {
        duskstudio::vst3::Vst3Instance neverCreated;
        std::string reactivateError;
        REQUIRE_FALSE (neverCreated.reactivate (48000.0, 64, reactivateError));
        REQUIRE (reactivateError == "not created");
    }

    duskstudio::vst3::Vst3Bundle bundle;
    std::string err;
    REQUIRE (bundle.load (DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH, err));
    REQUIRE (bundle.plugins().size() == 1);

    Steinberg::IPtr<Steinberg::IPlugView> view { nullptr };
    {
        duskstudio::vst3::Vst3Instance instance;
        REQUIRE (instance.create (bundle, bundle.plugins().front().id, err));
        auto* controller = static_cast<Steinberg::Vst::IEditController*> (
            instance.editController());
        REQUIRE (controller != nullptr);
        view = Steinberg::owned (
            controller->createView (Steinberg::Vst::ViewType::kEditor));
        REQUIRE (view != nullptr);
        REQUIRE (view->setFrame (static_cast<Steinberg::IPlugFrame*> (
                     instance.getHost().plugFrame())) == Steinberg::kResultTrue);
        instance.setActiveEditorView (view.get());
    }

    Steinberg::ViewRect teardownState;
    REQUIRE (view->getSize (&teardownState) == Steinberg::kResultTrue);
    REQUIRE (teardownState.getWidth() == 1);
    REQUIRE (teardownState.getHeight() == 1);
}

TEST_CASE ("VST3 rejected controller state keeps the saved state offline",
           "[vst3][slot][restore]")
{
    duskstudio::vst3::NativeVst3Slot slot;
    std::string error;
    REQUIRE (slot.load (
        std::filesystem::u8path (DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH),
        48000.0, 64, error));
    std::vector<uint8_t> saved;
    REQUIRE (slot.saveState (saved));
    REQUIRE (saved.size() == 14);
    REQUIRE (saved.back() == 0);

    SECTION ("accepted controller state round-trips independently of the component")
    {
        saved.back() = 1;
        REQUIRE (slot.loadState (saved));
        std::vector<uint8_t> captured;
        REQUIRE (slot.saveState (captured));
        CHECK (captured == saved);
        CHECK (slot.getLatencySamples() == 0);
    }

    SECTION ("component-only saved state remains supported")
    {
        saved.pop_back();
        saved[9] = 0;
        REQUIRE (slot.loadState (saved));
        CHECK (slot.isLoaded());
    }

    SECTION ("a nonempty rejected controller stream prevents recapturing replacement state")
    {
        saved.back() = 0xff;
        const auto original = saved;
        const bool accepted = slot.loadState (saved);
        CHECK_FALSE (accepted);
        const auto reason = duskstudio::hosting::enforceRestorePolicy (
            true, true, accepted, {}, saved.size(), [&] { slot.unload(); });
        CHECK_FALSE (slot.isLoaded());
        CHECK (reason == "saved state was rejected (14 bytes); slot left offline "
                         "to preserve the saved state");
        if (slot.isLoaded())
            slot.saveState (saved);
        CHECK (saved == original);
    }
}
