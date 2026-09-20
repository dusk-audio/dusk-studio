#include <catch2/catch_test_macros.hpp>

#include "session/MidiBindings.h"

#include <algorithm>
#include <vector>

using namespace duskstudio;

namespace
{
MidiBinding binding (MidiBindingTrigger trigger, int channel, int data,
                     MidiBindingTarget target, int index = 0)
{
    MidiBinding b;
    b.trigger = trigger;
    b.channel = channel;
    b.dataNumber = data;
    b.target = target;
    b.targetIndex = index;
    return b;
}

// Every target the binding menus offer. A target that exports but does not
// import is a binding the user silently loses on the way back in.
const std::vector<MidiBindingTarget>& allTargets()
{
    static const std::vector<MidiBindingTarget> targets {
        MidiBindingTarget::TransportPlay,
        MidiBindingTarget::TransportStop,
        MidiBindingTarget::TransportRecord,
        MidiBindingTarget::TransportToggle,
        MidiBindingTarget::TrackFader,
        MidiBindingTarget::TrackPan,
        MidiBindingTarget::TrackMute,
        MidiBindingTarget::TrackSolo,
        MidiBindingTarget::TrackArm,
        MidiBindingTarget::TrackAuxSend,
        MidiBindingTarget::TrackHpfFreq,
        MidiBindingTarget::TrackEqGain,
        MidiBindingTarget::TrackEqFreq,
        MidiBindingTarget::TrackEqQ,
        MidiBindingTarget::TrackCompThresh,
        MidiBindingTarget::TrackCompMakeup,
        MidiBindingTarget::TrackPluginParam,
        MidiBindingTarget::TrackFaderBank,
        MidiBindingTarget::TrackPanBank,
        MidiBindingTarget::TrackMuteBank,
        MidiBindingTarget::TrackSoloBank,
        MidiBindingTarget::TrackArmBank,
        MidiBindingTarget::TrackAuxSendBank,
        MidiBindingTarget::TrackHpfFreqBank,
        MidiBindingTarget::TrackEqGainBank,
        MidiBindingTarget::TrackEqFreqBank,
        MidiBindingTarget::TrackEqQBank,
        MidiBindingTarget::TrackCompThreshBank,
        MidiBindingTarget::TrackCompMakeupBank,
        MidiBindingTarget::TrackPluginParamBank,
        MidiBindingTarget::BusFader,
        MidiBindingTarget::BusPan,
        MidiBindingTarget::BusMute,
        MidiBindingTarget::BusSolo,
        MidiBindingTarget::AuxLaneFader,
        MidiBindingTarget::AuxLaneMute,
        MidiBindingTarget::AuxPluginParam,
        MidiBindingTarget::MasterFader,
        MidiBindingTarget::TrackEqEnabled,
        MidiBindingTarget::TrackCompEnabled,
        MidiBindingTarget::TrackInsertBypass,
        MidiBindingTarget::TrackAuxSendPrePost,
        MidiBindingTarget::BusEqGain,
        MidiBindingTarget::MasterEqLfBoost,
        MidiBindingTarget::MasterEqHfBoost,
        MidiBindingTarget::MasterCompThresh,
        MidiBindingTarget::MasterCompMakeup,
        MidiBindingTarget::MasterCompRatio,
    };
    return targets;
}
} // namespace

// Export writes the panel's bindings to a .json preset and import reads them
// back, so a controller setup can move between sessions intact.
TEST_CASE ("MIDI bindings preset round-trips every field", "[midi][bindings]")
{
    std::vector<MidiBinding> binds {
        binding (MidiBindingTrigger::CC, 3, 23, MidiBindingTarget::TrackFader, 7),
        binding (MidiBindingTrigger::Note, 0, 60, MidiBindingTarget::TrackMute, 2),
        binding (MidiBindingTrigger::PitchBend, 5, 0, MidiBindingTarget::MasterFader),
        binding (MidiBindingTrigger::MmcCommand, 0, 2, MidiBindingTarget::TransportPlay),
    };
    binds[1].buttonMode = MidiButtonMode::Toggle;
    binds[0].paramIndex = 4;

    const auto restored = deserializeBindingsPreset (serializeBindingsPreset (binds));
    REQUIRE (restored.has_value());
    REQUIRE (restored->size() == binds.size());
    for (size_t i = 0; i < binds.size(); ++i)
    {
        CHECK (restored->at (i).trigger == binds[i].trigger);
        CHECK (restored->at (i).channel == binds[i].channel);
        CHECK (restored->at (i).dataNumber == binds[i].dataNumber);
        CHECK (restored->at (i).target == binds[i].target);
        CHECK (restored->at (i).targetIndex == binds[i].targetIndex);
        CHECK (restored->at (i).paramIndex == binds[i].paramIndex);
        CHECK (restored->at (i).buttonMode == binds[i].buttonMode);
    }
}

// The manual says every target in the list can be bound. One that import drops
// would take the user's binding with it.
TEST_CASE ("MIDI bindings preset round-trips every available target", "[midi][bindings]")
{
    std::vector<MidiBinding> binds;
    for (auto target : allTargets())
        binds.push_back (binding (MidiBindingTrigger::CC, 1, 20, target, 1));

    const auto restored = deserializeBindingsPreset (serializeBindingsPreset (binds));
    REQUIRE (restored.has_value());
    REQUIRE (restored->size() == binds.size());
    for (size_t i = 0; i < binds.size(); ++i)
        CHECK (restored->at (i).target == binds[i].target);

    // And every one of them names itself for the panel.
    for (auto target : allTargets())
    {
        const auto* name = nameForTarget (target);
        REQUIRE (name != nullptr);
        CHECK (juce::String (name).isNotEmpty());
    }
}

// An unreadable import is reported rather than applied, and the report has to
// tell an empty preset (clear all) apart from a broken file.
TEST_CASE ("MIDI bindings import separates a broken preset from an empty one",
           "[midi][bindings]")
{
    SECTION ("not JSON at all")
    {
        CHECK_FALSE (deserializeBindingsPreset ("not json").has_value());
    }
    SECTION ("JSON, but not an object")
    {
        CHECK_FALSE (deserializeBindingsPreset ("[1, 2, 3]").has_value());
    }
    SECTION ("an object from some other schema")
    {
        CHECK_FALSE (deserializeBindingsPreset ("{\"format_version\":1}").has_value());
        CHECK_FALSE (
            deserializeBindingsPreset ("{\"bindings\":\"nope\"}").has_value());
    }
    SECTION ("a well-formed preset with no bindings clears them all")
    {
        const auto empty = deserializeBindingsPreset (
            serializeBindingsPreset (std::vector<MidiBinding> {}));
        REQUIRE (empty.has_value());
        CHECK (empty->empty());
    }
}

// A preset written by a later version can name a target this build has never
// heard of. That entry is dropped; the rest of the file still loads.
TEST_CASE ("MIDI bindings import keeps the entries it understands", "[midi][bindings]")
{
    const juce::String json = R"({
      "format_version": 1,
      "bindings": [
        {"channel": 1, "data": 20, "trigger": 0, "target": 100, "target_idx": 3},
        {"channel": 1, "data": 21, "trigger": 0, "target": 9999, "target_idx": 0},
        "a string where an object should be",
        {"channel": 1, "data": 22, "trigger": 0, "target": 0, "target_idx": 0},
        {"channel": 1, "data": 23, "trigger": 0, "target": 200, "target_idx": 0}
      ]
    })";

    const auto restored = deserializeBindingsPreset (json);
    REQUIRE (restored.has_value());
    // The unknown target, the non-object and the None target are all dropped.
    REQUIRE (restored->size() == 2);
    CHECK (restored->at (0).target == MidiBindingTarget::TrackFader);
    CHECK (restored->at (0).targetIndex == 3);
    CHECK (restored->at (1).target == MidiBindingTarget::MasterFader);
}

// Out-of-range numbers in a hand-edited file are clamped, not trusted.
TEST_CASE ("MIDI bindings import clamps a hand-edited preset", "[midi][bindings]")
{
    const juce::String json = R"({
      "bindings": [
        {"channel": 99, "data": 300, "trigger": 77, "target": 100,
         "target_idx": 1, "button_mode": 42}
      ]
    })";

    const auto restored = deserializeBindingsPreset (json);
    REQUIRE (restored.has_value());
    REQUIRE (restored->size() == 1);
    const auto& b = restored->front();
    CHECK (b.channel == 16);
    CHECK (b.dataNumber == 127);
    CHECK (b.trigger == MidiBindingTrigger::CC);
    CHECK (b.buttonMode == MidiButtonMode::Toggle);
    CHECK (b.isValid());
}
