// Instantiate a real VST3 effect and process audio offline THROUGH the
// InsertAdapter — the same generalized path the DSP call sites use — proving the
// host-agnostic foundation covers its third format. Gated on
// DUSKSTUDIO_TEST_VST3=/path/to.vst3.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/vst3/Vst3Bundle.h"
#include "engine/vst3/Vst3BusPlan.h"
#include "engine/vst3/Vst3HostContext.h"
#include "engine/vst3/Vst3Instance.h"
#include "engine/vst3/Vst3MidiNoteTracker.h"
#include "support/AllocationObserver.h"

#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

TEST_CASE ("VST3 SDK parameter queues grow beyond their retained capacity", "[vst3][allocation]")
{
    using namespace Steinberg;
    using duskstudio::test::AllocationObserver;
    SECTION ("Allocation observation covers scalar and aligned storage")
    {
        std::size_t observed = 0;
        {
            AllocationObserver observer;
            auto* scalar = ::operator new (17);
            auto* array = ::operator new[] (17);
            auto* aligned = ::operator new (64, std::align_val_t { 64 });
            auto* alignedArray = ::operator new[] (64, std::align_val_t { 64 });
            auto* scalarNoThrow = ::operator new (17, std::nothrow);
            auto* arrayNoThrow = ::operator new[] (17, std::nothrow);
            auto* alignedNoThrow = ::operator new (64, std::align_val_t { 64 }, std::nothrow);
            auto* alignedArrayNoThrow = ::operator new[] (64, std::align_val_t { 64 }, std::nothrow);
            ::operator delete (scalar);
            ::operator delete[] (array);
            ::operator delete (aligned, std::align_val_t { 64 });
            ::operator delete[] (alignedArray, std::align_val_t { 64 });
            ::operator delete (scalarNoThrow, std::nothrow);
            ::operator delete[] (arrayNoThrow, std::nothrow);
            ::operator delete (alignedNoThrow, std::align_val_t { 64 }, std::nothrow);
            ::operator delete[] (alignedArrayNoThrow, std::align_val_t { 64 }, std::nothrow);
            observed = observer.count();
        }
        CHECK (observed == 8);
    }
    SECTION ("A parameter queue grows only past its actual capacity")
    {
        struct Queue final : Vst::ParameterValueQueue
        {
            Queue() : ParameterValueQueue (7) {}
            std::size_t capacity() const noexcept { return values.capacity(); }
        } queue;
        const auto initialCapacity = queue.capacity();
        std::size_t growth = 0, reuse = 0;
        bool accepted = true;
        {
            AllocationObserver observer;
            for (std::size_t i = 0; i <= initialCapacity; ++i)
            {
                int32 index = -1;
                accepted = (queue.addPoint (static_cast<int32> (i), 0.5, index) == kResultTrue) && accepted;
            }
            growth = observer.count();
        }
        REQUIRE (accepted);
        CHECK (growth > 0);
        const auto retainedCapacity = queue.capacity();
        queue.clear();
        {
            AllocationObserver observer;
            for (std::size_t i = 0; i < retainedCapacity; ++i)
            {
                int32 index = -1;
                accepted = (queue.addPoint (static_cast<int32> (i), 0.5, index) == kResultTrue) && accepted;
            }
            int32 index = -1;
            accepted = (queue.addPoint (0, 0.25, index) == kResultTrue) && accepted;
            reuse = observer.count();
        }
        CHECK (accepted);
        CHECK (reuse == 0);
        CHECK (queue.getPointCount() == static_cast<int32> (retainedCapacity));
        int32 offset = -1;
        Vst::ParamValue value = 0.0;
        REQUIRE (queue.getPoint (0, offset, value) == kResultTrue);
        CHECK (offset == 0);
        CHECK_THAT (value, Catch::Matchers::WithinAbs (0.25, 1e-12));
    }
    SECTION ("A queue-count hint does not bound parameter growth")
    {
        Vst::ParameterChanges changes (1);
        std::size_t growth = 0;
        bool accepted = true;
        {
            AllocationObserver observer;
            for (Vst::ParamID id = 0; id < 2; ++id)
            {
                int32 index = -1;
                accepted = (changes.addParameterData (id, index) != nullptr) && accepted;
            }
            growth = observer.count();
        }
        CHECK (accepted);
        CHECK (growth > 0);
        CHECK (changes.getParameterCount() == 2);
    }
}

TEST_CASE ("VST3 panic releases every sounding note when its CC is unmapped",
           "[vst3][midi][regression][issue-460]")
{
    duskstudio::vst3::detail::MidiNoteTracker tracker;
    tracker.noteOn (2, 48);
    tracker.noteOn (2, 72);
    tracker.noteOn (2, 72);
    tracker.noteOn (3, 60);
    tracker.noteOff (2, 48);
    tracker.noteOff (2, 72);

    std::vector<int> released;
    SECTION ("a refused release is retained and stops the traversal immediately")
    {
        tracker.noteOn (2, 72);
        tracker.noteOn (2, 90);
        int attempts = 0;
        const bool complete = tracker.releaseChannel (2, [&] (int key)
        {
            ++attempts;
            if (! released.empty()) return false;
            released.push_back (key);
            return true;
        });
        CHECK_FALSE (complete);
        CHECK (attempts == 2);
        CHECK (tracker.soundingOnChannel (2) == 2);
        CHECK (tracker.isSounding (2, 72));
        CHECK (tracker.isSounding (2, 90));
        REQUIRE (tracker.releaseChannel (2, [&] (int key)
        {
            released.push_back (key);
            return true;
        }));
        CHECK (released == std::vector<int> { 72, 72, 90 });
        CHECK (tracker.soundingOnChannel (2) == 0);
        CHECK (tracker.isSounding (3, 60));
        return;
    }
    SECTION ("a complete release clears its channel") {}
    REQUIRE (tracker.releaseChannel (2, [&] (int key)
    {
        released.push_back (key);
        return true;
    }));

    REQUIRE (released == std::vector<int> { 72 });
    REQUIRE_FALSE (tracker.isSounding (2, 72));
    REQUIRE (tracker.isSounding (3, 60));
}

TEST_CASE ("VST3 audio bus plan keeps activation and scratch shape consistent",
           "[vst3][bus][regression][issue-361][issue-390][issue-455]")
{
    using namespace duskstudio::vst3::detail;

    AudioBusPlan plan (3, 4);
    plan.setBus (AudioBusDirection::Input, 0, 2, true);    // main
    plan.setBus (AudioBusDirection::Input, 1, 2, false);   // inactive optional
    plan.setBus (AudioBusDirection::Input, 2, 1, true);    // sidechain
    plan.setBus (AudioBusDirection::Output, 0, 2, true);   // main
    plan.setBus (AudioBusDirection::Output, 1, 0, true);   // zero-channel optional
    plan.setBus (AudioBusDirection::Output, 2, 8, true);   // modular aux
    plan.setBus (AudioBusDirection::Output, 3, 4, false);  // inactive optional

    std::vector<std::tuple<AudioBusDirection, int, bool>> activation;
    applyAudioBusPlan (plan, [&] (AudioBusDirection direction, int bus, bool active)
    {
        activation.emplace_back (direction, bus, active);
        return ! (direction == AudioBusDirection::Output && bus == 3 && ! active);
    });
    REQUIRE (activation == std::vector<std::tuple<AudioBusDirection, int, bool>> {
        { AudioBusDirection::Input, 0, true },
        { AudioBusDirection::Input, 1, false },
        { AudioBusDirection::Input, 2, true },
        { AudioBusDirection::Output, 0, true },
        { AudioBusDirection::Output, 1, false },
        { AudioBusDirection::Output, 2, true },
        { AudioBusDirection::Output, 3, false }
    });

    const int inputChannels[] = { 2, 2, 1 };
    const int outputChannels[] = { 2, 0, 8, 4 };
    REQUIRE (matchesAudioBusShape (
        plan, 3, 4,
        [&] (int bus) { return inputChannels[bus]; },
        [&] (int bus) { return outputChannels[bus]; }));
    REQUIRE_FALSE (matchesAudioBusShape (
        plan, 3, 4,
        [] (int bus) { return bus == 1 ? 0 : (bus == 2 ? 1 : 2); },
        [&] (int bus) { return outputChannels[bus]; }));

    std::vector<std::tuple<AudioBusDirection, int, int>> processChannels;
    applyAudioBufferPlan (plan, [&] (AudioBusDirection direction, int bus, int channels)
    {
        processChannels.emplace_back (direction, bus, channels);
    });
    REQUIRE (processChannels == std::vector<std::tuple<AudioBusDirection, int, int>> {
        { AudioBusDirection::Input, 0, 2 },
        { AudioBusDirection::Input, 1, 0 },
        { AudioBusDirection::Input, 2, 1 },
        { AudioBusDirection::Output, 0, 2 },
        { AudioBusDirection::Output, 1, 0 },
        { AudioBusDirection::Output, 2, 8 },
        { AudioBusDirection::Output, 3, 4 }
    });

    const auto shape = planScratch (plan);
    REQUIRE (shape.inputChannels == 3);
    REQUIRE (shape.outputChannels == 14);
    REQUIRE (shape.widestBus == 8);

    constexpr int frames = 64;
    std::vector<float> scratch (shape.outputChannels * (std::size_t) frames);
    for (std::size_t channel = 0; channel < shape.outputChannels; ++channel)
    {
        const auto offset = scratchChannelOffset (channel, frames);
        REQUIRE (offset + frames <= scratch.size());
        if (channel > 0)
            REQUIRE (offset != scratchChannelOffset (channel - 1, frames));
    }
}

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

        REQUIRE_FALSE (inst.ioChangePending());
        handler->restartComponent (Steinberg::Vst::RestartFlags::kIoChanged);
        REQUIRE (inst.ioChangePending());
        REQUIRE (inst.consumeIoChanged());
        REQUIRE_FALSE (inst.ioChangePending());

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
