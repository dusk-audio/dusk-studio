// Native LV2 state revisions and Save-time capture of queued patch messages,
// driven through the state-probe fixture: a plug-in whose saved level changes
// only when run() receives a patch:Set.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/hosting/InsertAdapter.h"
#include "engine/lv2/Lv2Bundle.h"
#include "engine/lv2/Lv2Instance.h"
#include "engine/lv2/NativeLv2Slot.h"
#include "fixtures/StateProbeLv2.h"

#include <lv2/atom/forge.h>
#include <lv2/core/lv2.h>
#include <lv2/patch/patch.h>
#include <lv2/urid/urid.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace
{
namespace Probe = duskstudio::test::stateprobelv2;
using Catch::Matchers::WithinAbs;

constexpr int kFrames = 32;

struct Lv2Probe
{
    Lv2Probe()
    {
        REQUIRE (bundle.load (DUSKSTUDIO_STATE_PROBE_LV2_FIXTURE_PATH, error));
        REQUIRE (instance.create (bundle, Probe::kPluginUri, error));
        REQUIRE (instance.activate (48000.0, kFrames, error));
        adapter.prepare (instance.portLayout(), kFrames);
        map = static_cast<LV2_URID_Map*> (
            static_cast<const LV2_Feature*> (instance.uridMapFeature())->data);
        for (int i = 0; i < instance.paramCount(); ++i)
            if (instance.paramInfo (i)->isPatchProperty) levelId = instance.paramInfo (i)->id;
        REQUIRE (levelId != 0);
    }

    void process() { adapter.process (instance, left.data(), right.data(), kFrames); }
    std::uint64_t revision() const { return instance.stateRevision(); }
    double level() const
    {
        double value = -1.0;
        REQUIRE (instance.getParamValue (levelId, value));
        return value;
    }
    LV2_URID urid (const char* uri) const { return map->map (map->handle, uri); }

    std::vector<std::uint8_t> object (LV2_URID type, const std::function<void (LV2_Atom_Forge&)>& body = {}) const
    {
        std::array<std::uint8_t, 256> buffer {};
        LV2_Atom_Forge forge;
        lv2_atom_forge_init (&forge, map);
        lv2_atom_forge_set_buffer (&forge, buffer.data(), buffer.size());
        LV2_Atom_Forge_Frame frame;
        lv2_atom_forge_object (&forge, &frame, 0, type);
        if (body) body (forge);
        lv2_atom_forge_pop (&forge, &frame);
        const auto* atom = reinterpret_cast<const LV2_Atom*> (buffer.data());
        return { buffer.begin(), buffer.begin() + (std::ptrdiff_t) (sizeof (LV2_Atom) + atom->size) };
    }
    std::vector<std::uint8_t> patchSet (float value) const
    {
        return object (urid (LV2_PATCH__Set), [&] (LV2_Atom_Forge& forge)
        {
            lv2_atom_forge_key (&forge, urid (LV2_PATCH__property));
            lv2_atom_forge_urid (&forge, urid (Probe::kLevelUri));
            lv2_atom_forge_key (&forge, urid (LV2_PATCH__value));
            lv2_atom_forge_float (&forge, value);
        });
    }
    std::vector<std::uint8_t> announce (Probe::Announcement what) const
    {
        return object (urid (Probe::kAnnounceUri), [&] (LV2_Atom_Forge& forge)
        {
            lv2_atom_forge_key (&forge, urid (Probe::kWhatUri));
            lv2_atom_forge_int (&forge, what);
        });
    }
    void send (const std::vector<std::uint8_t>& atom)
    {
        instance.forwardUiAtomEvent (atom.data(), (std::uint32_t) atom.size());
    }

    duskstudio::lv2::Lv2Bundle bundle;
    duskstudio::lv2::Lv2Instance instance;
    duskstudio::hosting::InsertAdapter adapter;
    std::string error;
    LV2_URID_Map* map = nullptr;
    std::uint32_t levelId = 0;
    std::array<float, kFrames> left {}, right {};
};

// The level a saved blob carries, read back from a fresh plug-in.
double restoredLevel (const std::vector<std::uint8_t>& blob)
{
    Lv2Probe restored;
    REQUIRE (restored.instance.loadState (blob));
    restored.instance.requestPatchParameterValuesForUi();
    restored.process();
    restored.instance.drainPatchFeedback();
    return restored.level();
}
} // namespace

TEST_CASE ("Lv2Instance revisions follow persistent edits from the host, the UI and the plug-in",
           "[lv2][instance][state]")
{
    Lv2Probe probe;
    probe.process();
    probe.instance.drainPatchFeedback();

    SECTION ("input control writes count when they change the value")
    {
        const auto before = probe.revision();
        probe.instance.setControlPortValueFromUi (Probe::kMeterPort, 1.0f);
        probe.instance.setControlPortValueFromUi (Probe::kGainPort, Probe::kDefaultGain);
        probe.instance.setParamValue (Probe::kGainPort, Probe::kDefaultGain);
        CHECK (probe.revision() == before);
        probe.instance.setControlPortValueFromUi (Probe::kGainPort, 0.5f);
        const auto fromUi = probe.revision();
        CHECK (fromUi > before);
        probe.instance.setParamValue (Probe::kGainPort, 0.5);
        CHECK (probe.revision() == fromUi);
        probe.instance.setParamValue (Probe::kGainPort, 0.75);
        const auto fromHost = probe.revision();
        CHECK (fromHost > fromUi);
        probe.process();
        CHECK (probe.revision() == fromHost);
        double gain = -1.0;
        REQUIRE (probe.instance.getParamValue (Probe::kGainPort, gain));
        CHECK_THAT (gain, WithinAbs (0.75, 1e-7));
    }

    SECTION ("patch writes count; requests, echoes and repeated values do not")
    {
        const auto before = probe.revision();
        probe.instance.setParamValue (probe.levelId, 0.0);
        CHECK (probe.revision() == before);
        probe.instance.setParamValue (probe.levelId, 0.3);
        const auto written = probe.revision();
        CHECK (written > before);
        probe.process();
        probe.instance.drainPatchFeedback();
        const auto applied = probe.revision();
        CHECK (applied >= written);
        probe.send (probe.object (probe.urid (LV2_PATCH__Get)));
        probe.send (probe.patchSet (0.3f));
        CHECK (probe.revision() == applied);
        probe.process();
        probe.instance.drainPatchFeedback();
        CHECK (probe.revision() == applied);
        probe.send (probe.patchSet (0.4f));
        const auto fromUi = probe.revision();
        CHECK (fromUi > applied);
        CHECK_THAT (probe.level(), WithinAbs (0.4, 1e-7));
    }

    SECTION ("the plug-in's own announcements count")
    {
        const auto before = probe.revision();
        probe.send (probe.announce (Probe::StateChanged));
        CHECK (probe.revision() == before);
        probe.process();
        const auto announced = probe.revision();
        CHECK (announced > before);
        probe.instance.drainPatchFeedback();
        probe.send (probe.announce (Probe::RepeatLevel));
        probe.process();
        probe.instance.drainPatchFeedback();
        CHECK (probe.revision() == announced);
        probe.send (probe.announce (Probe::ChangeLevel));
        probe.process();
        probe.instance.drainPatchFeedback();
        CHECK (probe.revision() > announced);
        CHECK_THAT (probe.level(), WithinAbs (Probe::kAnnouncedLevel, 1e-7));
    }

    SECTION ("capture and reactivation keep the revision; a restore counts once")
    {
        probe.instance.setParamValue (probe.levelId, 0.6);
        probe.process();
        probe.instance.drainPatchFeedback();
        std::vector<std::uint8_t> saved;
        const auto before = probe.revision();
        REQUIRE (probe.instance.saveState (saved));
        CHECK (probe.revision() == before);
        REQUIRE (probe.instance.reactivate (96000.0, kFrames, probe.error));
        probe.process();
        probe.instance.drainPatchFeedback();
        CHECK (probe.revision() == before);

        Lv2Probe restored;
        const auto fresh = restored.revision();
        CHECK_FALSE (restored.instance.loadState ({}));
        CHECK (restored.revision() == fresh);
        REQUIRE (restored.instance.loadState (saved));
        const auto loaded = restored.revision();
        CHECK (loaded > fresh);
        restored.process();
        restored.instance.drainPatchFeedback();
        CHECK (restored.revision() == loaded);
        CHECK_THAT (restored.level(), WithinAbs (0.6, 1e-7));
    }
}

TEST_CASE ("Lv2Instance flushes queued patch messages into the state Save captures",
           "[lv2][instance][state][regression]")
{
    Lv2Probe probe;
    probe.process();
    probe.instance.drainPatchFeedback();

    SECTION ("a host patch write")
    {
        probe.instance.setParamValue (probe.levelId, 0.7);
        const auto queued = probe.revision();
        REQUIRE (probe.instance.flushPendingWrites());
        const auto flushed = probe.revision();
        CHECK (flushed >= queued);
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK (probe.revision() == flushed);
        CHECK_THAT (restoredLevel (saved), WithinAbs (0.7, 1e-7));
        probe.process();
        probe.instance.drainPatchFeedback();
        CHECK (probe.revision() == flushed);
    }

    SECTION ("a patch message from the plug-in's editor")
    {
        probe.send (probe.patchSet (0.4f));
        REQUIRE (probe.instance.flushPendingWrites());
        std::vector<std::uint8_t> saved;
        REQUIRE (probe.instance.saveState (saved));
        CHECK_THAT (restoredLevel (saved), WithinAbs (0.4, 1e-7));
    }

    SECTION ("a change the plug-in announces during the flush is counted before the capture")
    {
        probe.send (probe.announce (Probe::ChangeLevel));
        const auto before = probe.revision();
        REQUIRE (probe.instance.flushPendingWrites());
        const auto flushed = probe.revision();
        CHECK (flushed > before);
        CHECK_THAT (probe.level(), WithinAbs (Probe::kAnnouncedLevel, 1e-7));
        probe.process();
        probe.instance.drainPatchFeedback();
        CHECK (probe.revision() == flushed);
    }

    SECTION ("nothing queued")
    {
        const auto before = probe.revision();
        REQUIRE (probe.instance.flushPendingWrites());
        CHECK (probe.revision() == before);
    }
}

TEST_CASE ("NativeLv2Slot flushes bindings and bypassed edits before a capture",
           "[lv2][slot][state][regression]")
{
    duskstudio::lv2::NativeLv2Slot slot;
    std::string error;
    REQUIRE (slot.load (std::filesystem::u8path (DUSKSTUDIO_STATE_PROBE_LV2_FIXTURE_PATH),
                        48000.0, kFrames, error));
    int levelIndex = -1;
    for (int i = 0; i < slot.paramCount(); ++i)
        if (slot.paramInfo (i)->isPatchProperty) levelIndex = i;
    REQUIRE (levelIndex >= 0);

    SECTION ("a MIDI-binding write still in the slot's queue")
    {
        slot.queueParamBinding ((std::uint32_t) levelIndex, 0.5f);
        const auto before = slot.stateRevision();
        REQUIRE (slot.flushPendingWrites());
        CHECK (slot.stateRevision() > before);
        std::vector<std::uint8_t> saved;
        REQUIRE (slot.saveState (saved));
        CHECK_THAT (restoredLevel (saved), WithinAbs (0.5, 1e-6));
    }

    SECTION ("an edit made while the slot is bypassed")
    {
        slot.setBypassed (true);
        slot.setParamValue (slot.paramInfo (levelIndex)->id, 0.2);
        std::array<float, kFrames> left {}, right {};
        slot.processStereo (left.data(), right.data(), left.data(), right.data(), kFrames);
        REQUIRE (slot.flushPendingWrites());
        std::vector<std::uint8_t> saved;
        REQUIRE (slot.saveState (saved));
        CHECK_THAT (restoredLevel (saved), WithinAbs (0.2, 1e-7));
    }
}
