// Native VST3 state revisions and Save-time parameter capture, driven through
// the state-probe fixture: a processor whose saved state changes only through
// parameter changes delivered to process().

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/vst3/NativeVst3Slot.h"
#include "engine/vst3/Vst3Bundle.h"
#include "engine/vst3/Vst3HostContext.h"
#include "engine/vst3/Vst3Instance.h"
#include "engine/vst3/Vst3ParameterChanges.h"
#include "fixtures/StateProbeVst3.h"
#include "support/AllocationObserver.h"

#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <pluginterfaces/vst/ivsteditcontroller.h>
#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
namespace Probe = duskstudio::test::stateprobevst3;
using Catch::Matchers::WithinAbs;

constexpr int kFrames = 64;

struct ProbeInstance
{
    ProbeInstance()
    {
        REQUIRE (bundle.load (DUSKSTUDIO_STATE_PROBE_VST3_FIXTURE_PATH, error));
        REQUIRE (bundle.plugins().size() == 1);
        REQUIRE (instance.create (bundle, bundle.plugins().front().id, error));
        REQUIRE (instance.activate (48000.0, kFrames, error));
        adapter.prepare (instance.portLayout(), kFrames);
    }
    void process (float trigger = 0.0f)
    {
        left.fill (0.0f);
        right.fill (0.0f);
        left[0] = trigger;
        adapter.process (instance, left.data(), right.data(), kFrames);
    }
    double value (std::uint32_t id) const
    {
        double result = -1.0;
        REQUIRE (instance.getParamValue (id, result));
        return result;
    }
    long counter (std::uint32_t id) const { return std::lround (value (id) * Probe::kCounterScale); }
    Steinberg::Vst::IComponentHandler& handler()
    {
        return *static_cast<Steinberg::Vst::IComponentHandler*> (instance.getHost().componentHandler());
    }

    duskstudio::vst3::Vst3Bundle bundle;
    duskstudio::vst3::Vst3Instance instance;
    duskstudio::hosting::InsertAdapter adapter;
    std::string error;
    std::array<float, kFrames> left {}, right {};
};

// The gain a saved blob carries, read back through a fresh instance.
double restoredGain (const std::vector<std::uint8_t>& blob)
{
    ProbeInstance restored;
    REQUIRE (restored.instance.loadState (blob));
    return restored.value (Probe::kGain);
}
} // namespace

TEST_CASE ("VST3 output parameter changes give the plug-in bounded writable queues",
           "[vst3][allocation][state]")
{
    using namespace Steinberg;
    duskstudio::vst3::detail::OutputParameterChanges changes;
    changes.prepare (3);

    SECTION ("queues follow the SDK collector's indexing and point ordering")
    {
        changes.beginBlock();
        int32 first = -1, again = -1, second = -1;
        auto* gain = changes.addParameterData (7, first);
        REQUIRE (gain != nullptr);
        CHECK (first == 0);
        CHECK (changes.addParameterData (7, again) == gain);
        CHECK (again == 0);
        auto* other = changes.addParameterData (9, second);
        REQUIRE (other != nullptr);
        CHECK (second == 1);
        CHECK (changes.getParameterCount() == 2);
        CHECK (changes.getParameterData (0) == gain);
        CHECK (changes.getParameterData (1) == other);
        CHECK (changes.getParameterData (2) == nullptr);
        CHECK (changes.getParameterData (-1) == nullptr);
        CHECK (gain->getParameterId() == 7);

        int32 index = -1;
        REQUIRE (gain->addPoint (20, 0.2, index) == kResultTrue);
        CHECK (index == 0);
        REQUIRE (gain->addPoint (5, 0.05, index) == kResultTrue);
        CHECK (index == 0);
        REQUIRE (gain->addPoint (20, 0.25, index) == kResultTrue);
        CHECK (index == 1);
        REQUIRE (gain->addPoint (63, 0.63, index) == kResultTrue);
        CHECK (index == 2);
        REQUIRE (gain->getPointCount() == 3);
        const std::array<std::pair<int32, double>, 3> expected { { { 5, 0.05 }, { 20, 0.25 }, { 63, 0.63 } } };
        for (int32 i = 0; i < 3; ++i)
        {
            int32 offset = -1;
            double value = -1.0;
            REQUIRE (gain->getPoint (i, offset, value) == kResultTrue);
            CHECK (offset == expected[(std::size_t) i].first);
            CHECK_THAT (value, WithinAbs (expected[(std::size_t) i].second, 1e-12));
        }
        int32 offset = -1;
        double value = -1.0;
        CHECK (gain->getPoint (3, offset, value) == kResultFalse);
        CHECK (other->getPointCount() == 0);

        changes.beginBlock();
        CHECK (changes.getParameterCount() == 0);
        auto* reused = changes.addParameterData (9, index);
        REQUIRE (reused != nullptr);
        CHECK (index == 0);
        CHECK (reused->getPointCount() == 0);
    }

    SECTION ("a full pool refuses instead of growing")
    {
        changes.beginBlock();
        int32 index = -1;
        for (Vst::ParamID id = 1; id <= 3; ++id)
            REQUIRE (changes.addParameterData (id, index) != nullptr);
        CHECK (changes.addParameterData (4, index) == nullptr);
        CHECK (index == -1);
        CHECK (changes.addParameterData (Vst::kNoParamId, index) == nullptr);
        auto* queue = changes.addParameterData (2, index);
        REQUIRE (queue != nullptr);
        CHECK (index == 1);
        for (int32 point = 0; point < duskstudio::vst3::detail::OutputParameterChanges::kPointsPerQueue; ++point)
            REQUIRE (queue->addPoint (point, 0.5, index) == kResultTrue);
        CHECK (queue->addPoint (1000, 0.5, index) == kResultFalse);
        CHECK (index == -1);
        CHECK (queue->addPoint (3, 0.75, index) == kResultTrue);
        CHECK (index == 3);
    }

    SECTION ("a block of writes allocates nothing")
    {
        changes.prepare (512);
        for (int pass = 0; pass < 2; ++pass)
        {
            std::size_t allocations = 0;
            bool accepted = true;
            {
                duskstudio::test::AllocationObserver observer;
                changes.beginBlock();
                for (Vst::ParamID id = 0; id < 512; ++id)
                {
                    int32 index = -1;
                    auto* queue = changes.addParameterData (id * 977u + 1u, index);
                    accepted = queue != nullptr && accepted;
                    if (queue != nullptr)
                        for (int32 point = 0; point < 4; ++point)
                            accepted = queue->addPoint (point * 16, 0.5, index) == kResultTrue && accepted;
                }
                allocations = observer.count();
            }
            CHECK (accepted);
            CHECK (allocations == 0);
            CHECK (changes.getParameterCount() == 512);
        }
    }
}

TEST_CASE ("Vst3Instance revisions follow the plug-in's persistent edits only", "[vst3][instance][state]")
{
    using namespace Steinberg;
    ProbeInstance probe;
    auto& handler = probe.handler();
    probe.process();
    REQUIRE (probe.counter (Probe::kQueueErrors) == 0);

    SECTION ("editor edits count; gestures and read-only parameters do not")
    {
        const auto before = probe.instance.stateRevision();
        handler.beginEdit (Probe::kGain);
        handler.endEdit (Probe::kGain);
        handler.performEdit (Probe::kMeter, 0.5);
        CHECK (probe.instance.stateRevision() == before);
        handler.performEdit (Probe::kGain, 0.6);
        const auto edited = probe.instance.stateRevision();
        CHECK (edited > before);
        // An id missing from the snapshot may be one the controller announced
        // since, so an edit to it is not dismissed as read-only.
        handler.performEdit (Probe::kUndeclared, 0.5);
        CHECK (probe.instance.stateRevision() > edited);
        probe.process();
        CHECK (probe.counter (Probe::kQueueErrors) == 0);
    }

    SECTION ("the plug-in can mark itself dirty through IComponentHandler2")
    {
        FUnknownPtr<Vst::IComponentHandler2> handler2 (&handler);
        REQUIRE (handler2);
        const auto before = probe.instance.stateRevision();
        CHECK (handler2->setDirty (false) == kResultOk);
        CHECK (handler2->startGroupEdit() == kResultOk);
        CHECK (handler2->finishGroupEdit() == kResultOk);
        CHECK (probe.instance.stateRevision() == before);
        CHECK (handler2->setDirty (true) == kResultOk);
        CHECK (probe.instance.stateRevision() > before);
    }

    SECTION ("restarts count only when they announce changed values or assignments")
    {
        const auto before = probe.instance.stateRevision();
        for (const auto flags : { Vst::kLatencyChanged, Vst::kIoChanged, Vst::kParamTitlesChanged,
                                  Vst::kIoTitlesChanged, Vst::kNoteExpressionChanged,
                                  Vst::kRoutingInfoChanged, Vst::kKeyswitchChanged })
            handler.restartComponent (flags);
        CHECK (probe.instance.stateRevision() == before);
        (void) probe.instance.consumeLatencyChanged();
        (void) probe.instance.consumeIoChanged();
        handler.restartComponent (Vst::kParamValuesChanged);
        const auto afterValues = probe.instance.stateRevision();
        CHECK (afterValues > before);
        handler.restartComponent (Vst::kMidiCCAssignmentChanged);
        CHECK (probe.instance.stateRevision() > afterValues);
    }

    SECTION ("processor reports count only for writable declared parameters")
    {
        const auto before = probe.instance.stateRevision();
        for (int block = 0; block < 4; ++block) probe.process();
        probe.process (Probe::kReportUndeclared);
        probe.process (Probe::kReportEmptyQueue);
        CHECK (probe.instance.stateRevision() == before);
        probe.process (Probe::kReportGain);
        CHECK (probe.instance.stateRevision() > before);
        CHECK (probe.counter (Probe::kQueueErrors) == 0);
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (Probe::kReportedGain, 1e-12));
    }

    SECTION ("host writes count when accepted and again when applied")
    {
        const auto before = probe.instance.stateRevision();
        probe.instance.setParamValue (Probe::kMeter, 1.0);
        CHECK (probe.instance.stateRevision() == before);
        probe.instance.setParamValue (Probe::kGain, 0.25);
        const auto queued = probe.instance.stateRevision();
        CHECK (queued > before);
        probe.process();
        CHECK (probe.instance.stateRevision() > queued);
    }

    SECTION ("capture, restore and reactivation keep the revision monotonic")
    {
        std::vector<std::uint8_t> saved;
        const auto before = probe.instance.stateRevision();
        REQUIRE (probe.instance.saveState (saved));
        CHECK (probe.instance.stateRevision() == before);
        auto corrupt = saved;
        corrupt.front() = 'X';
        CHECK_FALSE (probe.instance.loadState (corrupt));
        CHECK (probe.instance.stateRevision() == before);
        REQUIRE (probe.instance.loadState (saved));
        const auto restored = probe.instance.stateRevision();
        CHECK (restored > before);
        REQUIRE (probe.instance.reactivate (96000.0, kFrames, probe.error));
        probe.process();
        CHECK (probe.instance.stateRevision() == restored);
    }
}

TEST_CASE ("Vst3Instance flushes queued parameter changes into the state Save captures",
           "[vst3][instance][state][regression]")
{
    ProbeInstance probe;
    probe.process();

    SECTION ("an active instance hands them over in a zero-sample process call")
    {
        CHECK (probe.instance.flushPendingWrites());
        CHECK (probe.counter (Probe::kFlushes) == 0);
        probe.instance.setParamValue (Probe::kGain, 0.8);
        const auto queued = probe.instance.stateRevision();
        REQUIRE (probe.instance.flushPendingWrites());
        CHECK (probe.counter (Probe::kFlushes) == 1);
        const auto flushed = probe.instance.stateRevision();
        CHECK (flushed > queued);
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK (probe.instance.stateRevision() == flushed);
        CHECK_THAT (restoredGain (saved), WithinAbs (0.8, 1e-12));
        CHECK_THAT (probe.value (Probe::kGain), WithinAbs (0.8, 1e-12));
        CHECK (probe.instance.isActive());
        probe.process();
        CHECK (probe.instance.stateRevision() == flushed);
        CHECK (probe.counter (Probe::kQueueErrors) == 0);
    }

    SECTION ("an editor edit reaches the processor before the capture")
    {
        probe.handler().performEdit (Probe::kGain, 0.3);
        REQUIRE (probe.instance.flushPendingWrites());
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (0.3, 1e-12));
    }

    SECTION ("an inactive instance is activated only for the flush")
    {
        const auto activations = probe.counter (Probe::kActivations);
        probe.instance.deactivate();
        probe.instance.setParamValue (Probe::kGain, 0.7);
        REQUIRE (probe.instance.flushPendingWrites());
        CHECK_FALSE (probe.instance.isActive());
        CHECK (probe.counter (Probe::kActivations) == activations + 1);
        CHECK (probe.counter (Probe::kFlushes) == 1);
        CHECK (probe.counter (Probe::kQueueErrors) == 0);
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (0.7, 1e-12));
        REQUIRE (probe.instance.activate (48000.0, kFrames, probe.error));
        CHECK (probe.counter (Probe::kActivations) == activations + 2);
    }

    SECTION ("a plug-in refusing the flush gets the changes with its next block")
    {
        probe.instance.setParamValue (Probe::kRefuseFlushes, 1.0);
        probe.instance.setParamValue (Probe::kGain, 0.6);
        CHECK_FALSE (probe.instance.flushPendingWrites());
        CHECK (probe.counter (Probe::kFlushes) == 1);
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (Probe::kDefaultGain, 1e-12));
        const auto before = probe.instance.stateRevision();
        probe.process();
        CHECK (probe.instance.stateRevision() > before);
        REQUIRE (probe.instance.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (0.6, 1e-12));
    }

    SECTION ("an instance that was never set up keeps its changes queued")
    {
        duskstudio::vst3::Vst3Instance fresh;
        REQUIRE (fresh.create (probe.bundle, probe.bundle.plugins().front().id, probe.error));
        fresh.setParamValue (Probe::kGain, 0.9);
        CHECK_FALSE (fresh.flushPendingWrites());
        REQUIRE (fresh.activate (48000.0, kFrames, probe.error));
        REQUIRE (fresh.flushPendingWrites());
        std::vector<std::uint8_t> saved;
        REQUIRE (fresh.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (0.9, 1e-12));
    }
}

TEST_CASE ("NativeVst3Slot flushes bindings and queued edits before a capture",
           "[vst3][slot][state][regression]")
{
    duskstudio::vst3::NativeVst3Slot slot;
    std::string error;
    REQUIRE (slot.load (std::filesystem::u8path (DUSKSTUDIO_STATE_PROBE_VST3_FIXTURE_PATH),
                        48000.0, kFrames, error));
    int gainIndex = -1;
    for (int i = 0; i < slot.paramCount(); ++i)
        if (slot.paramInfo (i)->id == Probe::kGain) gainIndex = i;
    REQUIRE (gainIndex >= 0);

    SECTION ("a MIDI-binding write still in the slot's queue")
    {
        slot.queueParamBinding ((std::uint32_t) gainIndex, 0.375f);
        const auto before = slot.stateRevision();
        REQUIRE (slot.flushPendingWrites());
        CHECK (slot.stateRevision() > before);
        std::vector<std::uint8_t> saved;
        REQUIRE (slot.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (0.375, 1e-6));
    }

    SECTION ("an edit made while the slot is bypassed")
    {
        slot.setBypassed (true);
        slot.setParamValue (Probe::kGain, 0.125);
        std::array<float, kFrames> left {}, right {};
        slot.processStereo (left.data(), right.data(), left.data(), right.data(), kFrames);
        REQUIRE (slot.flushPendingWrites());
        std::vector<std::uint8_t> saved;
        REQUIRE (slot.saveState (saved));
        CHECK_THAT (restoredGain (saved), WithinAbs (0.125, 1e-12));
    }

    SECTION ("an empty slot has nothing to flush")
    {
        slot.unload();
        CHECK (slot.flushPendingWrites());
        CHECK (slot.stateRevision() == 0);
    }
}
