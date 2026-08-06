#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <string>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
// ctest runs the cases as parallel processes, so a unique name is not enough on
// its own - the directory has to actually be created, and the failure to create
// it has to stop the case rather than leave it writing into a shared parent.
struct ScopedTempDir
{
    juce::File dir;

    explicit ScopedTempDir (const juce::String& prefix)
        : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile (prefix + juce::Uuid().toDashedString()))
    {
        REQUIRE (dir.createDirectory().wasOk());
    }

    ~ScopedTempDir() { dir.deleteRecursively(); }
};
}

// A truncated / hand-edited session.json can lack whole section keys
// ("tracks", "buses", "aux_lanes"). Loading such a file over a populated
// session used to skip those sections entirely, leaving the previous
// session's regions, plugins and mixer state alive under the new session's
// name. load() now substitutes serialized defaults for any missing section.
TEST_CASE ("loading a session without section keys resets those sections",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-missing-sections-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    // Populate every ghostable surface.
    {
        AudioRegion r;
        r.file            = s.getAudioDirectory().getChildFile ("ghost.wav");
        r.lengthInSamples = 1000;
        s.track (3).regions.push_back (r);
        s.track (3).pluginLegacyDescriptionXml = "<PLUGIN/>";
        s.track (3).pluginStateBase64    = "ABCD";

        s.bus (1).strip.faderDb.store (-12.0f);
        s.bus (1).strip.compEnabled.store (true);

        s.auxLane (0).pluginLegacyDescriptionXml[0] = "<PLUGIN/>";
        s.auxLane (0).pluginStateBase64[0]    = "ABCD";
        s.auxLane (0).nativeClapPath[0]       = "/tmp/ghost.clap";
        s.auxLane (0).params.returnLevelDb.store (-6.0f);
    }

    const auto target = dir.getChildFile ("session.json");

    SECTION ("all section keys absent")
    {
        target.replaceWithText (R"({"version":3})");
    }
    SECTION ("section keys present but not arrays")
    {
        target.replaceWithText (R"({"version":3,"tracks":42,"buses":"x","aux_lanes":{}})");
    }
    SECTION ("section arrays shorter than the model")
    {
        target.replaceWithText (R"({"version":3,"tracks":[],"buses":[],"aux_lanes":[]})");
    }

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE (s.track (3).regions.empty());
    REQUIRE_FALSE (s.track (3).pluginDescriptor.has_value());
    REQUIRE (s.track (3).pluginLegacyDescriptionXml.isEmpty());
    REQUIRE (s.track (3).pluginStateBase64.isEmpty());

    REQUIRE_THAT (s.bus (1).strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_FALSE (s.bus (1).strip.compEnabled.load());

    REQUIRE_FALSE (s.auxLane (0).pluginDescriptor[0].has_value());
    REQUIRE (s.auxLane (0).pluginLegacyDescriptionXml[0].isEmpty());
    REQUIRE (s.auxLane (0).pluginStateBase64[0].isEmpty());
    REQUIRE (s.auxLane (0).nativeClapPath[0].isEmpty());
    REQUIRE_THAT (s.auxLane (0).params.returnLevelDb.load(), WithinAbs (0.0f, 1e-6f));
}

// A slot that is LISTED but isn't an object ("tracks": [null]) is the same
// ghost hazard as a missing slot: the restore path bails on a non-object, so
// the slot kept the previous session's regions / automation / mode.
TEST_CASE ("loading a session with a non-object track slot resets that slot",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-null-track-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    AudioRegion r;
    r.file            = s.getAudioDirectory().getChildFile ("ghost.wav");
    r.lengthInSamples = 1000;
    s.track (0).regions.push_back (r);
    s.track (0).automationLanes[(size_t) AutomationParam::FaderDb]
        .publishPoints ({ { 0, 0.25f }, { 48000, 0.75f } });
    s.track (0).automationMode.store (2, std::memory_order_release);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"tracks":[null]})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE (s.track (0).regions.empty());
    REQUIRE (s.track (0).automationLanes[(size_t) AutomationParam::FaderDb].pointsConst().empty());
    REQUIRE (s.track (0).automationMode.load (std::memory_order_acquire) == 0);
}

// Buses and aux lanes inherit any property their object omits, so a non-object
// entry has to restore from the serialized defaults - an empty object would
// leave the previous session's mixer state and plugins in the slot.
TEST_CASE ("loading a session with non-object bus / aux slots resets them",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-null-bus-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    s.bus (0).strip.faderDb.store (-12.0f);
    s.bus (0).strip.compEnabled.store (true);
    s.auxLane (0).params.returnLevelDb.store (-6.0f);
    s.auxLane (0).pluginStateBase64[0] = "ABCD";
    s.auxLane (0).nativeClapPath[0]    = "/tmp/ghost.clap";

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"buses":[null],"aux_lanes":[null]})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE_THAT (s.bus (0).strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_FALSE (s.bus (0).strip.compEnabled.load());
    REQUIRE_THAT (s.auxLane (0).params.returnLevelDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE (s.auxLane (0).pluginStateBase64[0].isEmpty());
    REQUIRE (s.auxLane (0).nativeClapPath[0].isEmpty());
}

// A slot that IS listed but only partially populated is the same ghost hazard
// as one that is missing: every mixer setter is conditional on its key being
// present, so a key the file leaves out would keep the previously loaded
// session's value. Each one the slot lacks is filled from the serialized model
// default instead.
TEST_CASE ("loading a session with a partial track slot resets its absent keys",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-partial-track-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    s.track (0).name = "ghost";
    s.track (0).strip.faderDb.store (-12.0f);
    s.track (0).strip.pan.store (0.75f);
    s.track (0).strip.mute.store (true);
    {
        auto ghostRouting = std::make_unique<HardwareInsertRouting>();
        ghostRouting->outputChL     = 6;
        ghostRouting->inputChL      = 7;
        ghostRouting->latencySamples = 512;
        s.track (0).hardwareInsert.routing.publish (std::move (ghostRouting));
    }

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"tracks":[{"name":"A"}]})");

    REQUIRE (SessionSerializer::load (s, target));

    // What the file specified survives; everything it omitted comes back at the
    // model default rather than the previous session's value.
    REQUIRE (s.track (0).name == "A");
    REQUIRE_THAT (s.track (0).strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (s.track (0).strip.pan.load(),     WithinAbs (0.0f, 1e-6f));
    REQUIRE_FALSE (s.track (0).strip.mute.load());

    const auto routing = s.track (0).hardwareInsert.routing.current();
    REQUIRE (routing.outputChL      == -1);
    REQUIRE (routing.inputChL       == -1);
    REQUIRE (routing.latencySamples == 0);
}

// RFC 7396 reads an explicit null as "delete this key", which would leave the
// key out of the merged slot, skip its setter and land the slot back on the
// live value - the exact failure the fill exists to prevent. Handing the null
// through to the setter instead is no better: the setter's own fallback is not
// always the model default. Here a null can only mean unspecified.
TEST_CASE ("a null member in a partial slot resolves to the model default",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-null-member-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);
    s.track (0).strip.mute.store (true);
    s.track (0).inputSource.store (5);
    s.bus (0).strip.mute.store (true);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":3,"tracks":[{"mute":null,"input_source":null}],)"
        R"("buses":[{"mute":null}]})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE_FALSE (s.track (0).strip.mute.load());
    REQUIRE_FALSE (s.bus (0).strip.mute.load());
    // The setter's fallback for a junk input_source is 0; the model default is
    // -2, so only a null that resolved to the default lands here.
    REQUIRE (s.track (0).inputSource.load() == -2);
}

// A section the file gives as a scalar reaches the restore path as the shared
// empty object, which passes the section's presence check and then skips every
// conditional setter inside it - the previously loaded session's routing and
// trims survive a slot that never described them. A type that contradicts the
// default is no more readable than an absent key, so the default section
// replaces it.
TEST_CASE ("a non-object where the default holds a section resets that section",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-type-mismatch-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    {
        auto ghostRouting = std::make_unique<HardwareInsertRouting>();
        ghostRouting->outputChL      = 6;
        ghostRouting->outputChR      = 7;
        ghostRouting->inputChL       = 8;
        ghostRouting->inputChR       = 9;
        ghostRouting->latencySamples = 512;
        ghostRouting->format         = 1;
        s.track (0).hardwareInsert.routing.publish (std::move (ghostRouting));
    }
    s.track (0).hardwareInsert.outputGainDb.store (-6.0f);
    s.track (0).hardwareInsert.inputGainDb .store ( 3.0f);
    s.track (0).hardwareInsert.dryWet      .store (0.25f);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"tracks":[{"hardware_insert":42}]})");

    REQUIRE (SessionSerializer::load (s, target));

    const HardwareInsertRouting defaults;
    const auto routing = s.track (0).hardwareInsert.routing.current();
    REQUIRE (routing.outputChL      == defaults.outputChL);
    REQUIRE (routing.outputChR      == defaults.outputChR);
    REQUIRE (routing.inputChL       == defaults.inputChL);
    REQUIRE (routing.inputChR       == defaults.inputChR);
    REQUIRE (routing.latencySamples == defaults.latencySamples);
    REQUIRE (routing.format         == defaults.format);

    REQUIRE_THAT (s.track (0).hardwareInsert.outputGainDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (s.track (0).hardwareInsert.inputGainDb .load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (s.track (0).hardwareInsert.dryWet      .load(), WithinAbs (1.0f, 1e-6f));
}

// The stable identifier wins when there is one, but an unrouted track
// serializes midi_input_id as "", so the fill hands every slot that key.
// Keying the identifier path on presence would swallow a session carrying only
// the legacy raw index.
TEST_CASE ("a legacy MIDI index survives the default fill", "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-legacy-midi-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":3,"tracks":[{"midi_input_idx":2,"midi_output_idx":3}]})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE (s.track (0).midiInputIndex.load (std::memory_order_acquire)  == 2);
    REQUIRE (s.track (0).midiOutputIndex.load (std::memory_order_acquire) == 3);
    REQUIRE (s.track (0).midiInputIdentifier.isEmpty());
}

// plugin_slots is an array, so it replaces the default wholesale. Stopping the
// restore at whatever the file's array carries therefore left a position the
// file does not describe holding the previous session's plugin. Every position
// is driven, each filled from the matching default.
TEST_CASE ("an aux lane's undescribed plugin slot drops the previous plugin",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-aux-slot-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    s.auxLane (0).pluginLegacyDescriptionXml[0] = "<PLUGIN/>";
    s.auxLane (0).pluginStateBase64[0]          = "ABCD";
    s.auxLane (0).nativeClapPath[0]             = "/tmp/ghost.clap";
    s.auxLane (0).nativeClapPluginId[0]         = "com.ghost.plugin";
    {
        auto ghostRouting = std::make_unique<HardwareInsertRouting>();
        ghostRouting->outputChL = 6;
        ghostRouting->inputChL  = 7;
        s.auxLane (0).hardwareInserts[0].routing.publish (std::move (ghostRouting));
        s.auxLane (0).hardwareInserts[0].enabled.store (true);
    }

    // The lane is described, but its plugin slot is not.
    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":3,"aux_lanes":[{"name":"A","plugin_slots":[]}]})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE (s.auxLane (0).name == "A");
    REQUIRE (s.auxLane (0).pluginLegacyDescriptionXml[0].isEmpty());
    REQUIRE (s.auxLane (0).pluginStateBase64[0].isEmpty());
    REQUIRE (s.auxLane (0).nativeClapPath[0].isEmpty());
    REQUIRE (s.auxLane (0).nativeClapPluginId[0].isEmpty());
    REQUIRE_FALSE (s.auxLane (0).hardwareInserts[0].enabled.load());

    const auto routing = s.auxLane (0).hardwareInserts[0].routing.current();
    REQUIRE (routing.outputChL == -1);
    REQUIRE (routing.inputChL  == -1);
}

// A hand-edited slot can nest itself deeper than any walk over it can recurse.
// Past the bound the slot is unusable rather than fatal: it restores from the
// model default, and the surrounding session still loads.
TEST_CASE ("a pathologically nested track slot falls back to the default",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-deep-slot-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);
    s.track (0).name = "ghost";
    s.track (0).strip.faderDb.store (-12.0f);

    constexpr int kNesting = 50000;
    std::string deep;
    deep.reserve ((size_t) kNesting * 6 + 1);
    for (int i = 0; i < kNesting; ++i) deep += R"({"a":)";
    deep += "1";
    deep.append ((size_t) kNesting, '}');

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"tracks":[{"name":"A","deep":)"
                             + juce::String (deep) + R"(}]})");

    REQUIRE (SessionSerializer::load (s, target));

    const Session defaults;
    REQUIRE (s.track (0).name == defaults.track (0).name);
    REQUIRE_THAT (s.track (0).strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
}

// The mastering chain was restored entirely inside a "mastering" presence
// check, so a file without the key left every field holding the previous
// session's chain - and the Mastering stage then processed the newly loaded
// mix through it.
TEST_CASE ("loading a session without a mastering section resets the chain",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-missing-mastering-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    auto& m = s.mastering();
    m.sourceFile = dir.getChildFile ("ghost-mix.wav");
    m.eqEnabled.store (true);
    for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
    {
        m.eqBandFreq[b]  .store (777.0f + (float) b);
        m.eqBandGainDb[b].store (6.0f + (float) b);
        m.eqBandQ[b]     .store (3.5f + (float) b);
    }
    m.eqLfBoost      .store (5.0f);
    m.eqHfBoost      .store (4.0f);
    m.eqHfAtten      .store (3.0f);
    m.eqTubeDrive    .store (0.9f);
    m.eqOutputGainDb .store (2.0f);
    m.compEnabled    .store (true);
    m.compThreshDb   .store (-18.0f);
    m.compRatio      .store (8.0f);
    m.compAttackMs   .store (1.0f);
    m.compReleaseMs  .store (500.0f);
    m.compReleaseAuto.store (false);
    m.compMakeupDb   .store (6.0f);
    m.limiterEnabled   .store (false);
    m.limiterDriveDb   .store (9.0f);
    m.limiterCeilingDb .store (-6.0f);
    m.limiterReleaseMs .store (400.0f);
    m.limiterMode      .store (2);
    m.limiterStereoLink.store (false);
    m.limiterLookaheadMs.store (8.0f);
    m.targetPresetIndex.store (3);

    const auto target = dir.getChildFile ("session.json");

    SECTION ("mastering key absent")
    {
        target.replaceWithText (R"({"version":3})");
    }
    SECTION ("mastering key present but not an object")
    {
        target.replaceWithText (R"({"version":3,"mastering":42})");
    }

    REQUIRE (SessionSerializer::load (s, target));

    const MasteringParams def;
    REQUIRE (m.sourceFile == juce::File());
    REQUIRE (m.eqEnabled.load() == def.eqEnabled.load());
    for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
    {
        REQUIRE_THAT (m.eqBandFreq[b]  .load(), WithinAbs (def.eqBandFreq[b]  .load(), 1e-6f));
        REQUIRE_THAT (m.eqBandGainDb[b].load(), WithinAbs (def.eqBandGainDb[b].load(), 1e-6f));
        REQUIRE_THAT (m.eqBandQ[b]     .load(), WithinAbs (def.eqBandQ[b]     .load(), 1e-6f));
    }
    REQUIRE_THAT (m.eqLfBoost     .load(), WithinAbs (def.eqLfBoost     .load(), 1e-6f));
    REQUIRE_THAT (m.eqHfBoost     .load(), WithinAbs (def.eqHfBoost     .load(), 1e-6f));
    REQUIRE_THAT (m.eqHfAtten     .load(), WithinAbs (def.eqHfAtten     .load(), 1e-6f));
    REQUIRE_THAT (m.eqTubeDrive   .load(), WithinAbs (def.eqTubeDrive   .load(), 1e-6f));
    REQUIRE_THAT (m.eqOutputGainDb.load(), WithinAbs (def.eqOutputGainDb.load(), 1e-6f));
    REQUIRE (m.compEnabled.load() == def.compEnabled.load());
    REQUIRE_THAT (m.compThreshDb  .load(), WithinAbs (def.compThreshDb  .load(), 1e-6f));
    REQUIRE_THAT (m.compRatio     .load(), WithinAbs (def.compRatio     .load(), 1e-6f));
    REQUIRE_THAT (m.compAttackMs  .load(), WithinAbs (def.compAttackMs  .load(), 1e-6f));
    REQUIRE_THAT (m.compReleaseMs .load(), WithinAbs (def.compReleaseMs .load(), 1e-6f));
    REQUIRE (m.compReleaseAuto.load() == def.compReleaseAuto.load());
    REQUIRE_THAT (m.compMakeupDb  .load(), WithinAbs (def.compMakeupDb  .load(), 1e-6f));
    REQUIRE (m.limiterEnabled.load() == def.limiterEnabled.load());
    REQUIRE_THAT (m.limiterDriveDb    .load(), WithinAbs (def.limiterDriveDb    .load(), 1e-6f));
    REQUIRE_THAT (m.limiterCeilingDb  .load(), WithinAbs (def.limiterCeilingDb  .load(), 1e-6f));
    REQUIRE_THAT (m.limiterReleaseMs  .load(), WithinAbs (def.limiterReleaseMs  .load(), 1e-6f));
    REQUIRE_THAT (m.limiterLookaheadMs.load(), WithinAbs (def.limiterLookaheadMs.load(), 1e-6f));
    REQUIRE (m.limiterMode.load()       == def.limiterMode.load());
    REQUIRE (m.limiterStereoLink.load() == def.limiterStereoLink.load());
    REQUIRE (m.targetPresetIndex.load() == def.targetPresetIndex.load());
}

// Mastering follows the master strip rather than the per-track fill: a section
// that IS there describes the chain, and a key it omits keeps the live value.
// The limiter is the exception - it postdates the section, so a file written
// before it must not inherit one.
TEST_CASE ("a partial mastering section retains the keys it omits",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-partial-mastering-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    auto& m = s.mastering();
    m.eqEnabled       .store (true);
    m.eqLfBoost       .store (5.0f);
    m.eqOutputGainDb  .store (2.0f);
    m.compRatio       .store (8.0f);
    m.limiterCeilingDb.store (-6.0f);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":3,"mastering":{"comp_ratio":6.0,"eq_output_gain_db":1e40}})");

    REQUIRE (SessionSerializer::load (s, target));

    const MasteringParams def;
    REQUIRE_THAT (m.compRatio.load(), WithinAbs (6.0f, 1e-6f));
    // Past float range: unusable, so the model default rather than the file's
    // value or the live one.
    REQUIRE_THAT (m.eqOutputGainDb.load(), WithinAbs (def.eqOutputGainDb.load(), 1e-6f));
    REQUIRE (m.eqEnabled.load());
    REQUIRE_THAT (m.eqLfBoost.load(), WithinAbs (5.0f, 1e-6f));
    REQUIRE_THAT (m.limiterCeilingDb.load(), WithinAbs (def.limiterCeilingDb.load(), 1e-6f));
}

// Transport had the same presence check around it, with a wider blast radius:
// loop and punch ranges, tempo and the whole tempo map, sync / MCU identifiers
// and the MIDI bindings the audio thread reads all survived a file that never
// described them.
TEST_CASE ("loading a session without a transport section resets transport state",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-missing-transport-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);

    s.savedLoopEnabled  = true;
    s.savedLoopStart    = 48000;
    s.savedLoopEnd      = 96000;
    s.savedPunchEnabled = true;
    s.savedPunchIn      = 24000;
    s.savedPunchOut     = 72000;
    s.snapToGrid        = true;
    s.audioEditorSnap   = true;
    s.midiEditorSnap    = false;
    s.pianoRollKeySnap  = false;
    s.snapResolution    = SnapResolution::CDFrames;
    s.tempoBpm.store (90.0f);
    s.tempoMap.setPoints ({ { 0, 100.0f }, { 48000, 140.0f } });
    s.uiStage.store (2);
    s.syncSourceInputIdentifier = "ghost-in";
    s.externalSyncFollowsTempo   .store (false);
    s.externalSyncChasesTransport.store (true);
    s.syncOutputIdentifier = "ghost-out";
    s.syncOutputEmitClock.store (true);
    s.externalTimeCodeChasesTransport.store (true,  std::memory_order_relaxed);
    s.syncOutputEmitTimeCode         .store (true,  std::memory_order_relaxed);
    s.syncOutputTimeCodeFrameRate    .store (0,     std::memory_order_relaxed);
    s.mcu.inputIdentifier  = "ghost-mcu-in";
    s.mcu.outputIdentifier = "ghost-mcu-out";
    s.mcu.assignMode.store (5, std::memory_order_relaxed);
    s.beatsPerBar.store (7);
    s.beatUnit   .store (8);
    s.metronomeEnabled           .store (true);
    s.metronomeVolDb             .store (6.0f);
    s.metronomeClickWhileRecording.store (false);
    s.metronomeClickWhilePlaying .store (true);
    s.metronomeOnlyDuringCountIn .store (true);
    s.metronomePolyphonic        .store (true);
    s.countInEnabled  .store (true);
    s.timeDisplayMode .store (1);
    s.lastRecordPointSamples.store (123456);
    s.preRollSeconds .store (4.0f);
    s.postRollSeconds.store (5.0f);
    s.preRollEnabled .store (false);
    s.postRollEnabled.store (false);
    s.oversamplingFactor.store (4, std::memory_order_relaxed);
    {
        MidiBinding b;
        b.channel     = 1;
        b.dataNumber  = 7;
        b.trigger     = MidiBindingTrigger::CC;
        b.target      = MidiBindingTarget::TransportPlay;
        auto ghost = std::make_unique<std::vector<MidiBinding>>();
        ghost->push_back (b);
        s.midiBindings.publish (std::move (ghost));
    }

    // The track carries a legacy MIDI region (no recorded_at_bpm), whose anchor
    // is stamped from the tempo peeked off the transport section.
    const auto target = dir.getChildFile ("session.json");
    const juce::String legacyMidiTrack =
        R"(,"tracks":[{"midi_regions":[{"timeline_start":0,"length_samples":48000,"length_ticks":1920}]}])";

    SECTION ("transport key absent")
    {
        target.replaceWithText (R"({"version":3)" + legacyMidiTrack + "}");
    }
    SECTION ("transport key present but not an object")
    {
        target.replaceWithText (R"({"version":3,"transport":42)" + legacyMidiTrack + "}");
    }

    REQUIRE (SessionSerializer::load (s, target));

    const Session def;
    REQUIRE (s.savedLoopEnabled  == def.savedLoopEnabled);
    REQUIRE (s.savedLoopStart    == def.savedLoopStart);
    REQUIRE (s.savedLoopEnd      == def.savedLoopEnd);
    REQUIRE (s.savedPunchEnabled == def.savedPunchEnabled);
    REQUIRE (s.savedPunchIn      == def.savedPunchIn);
    REQUIRE (s.savedPunchOut     == def.savedPunchOut);
    REQUIRE (s.snapToGrid       == def.snapToGrid);
    REQUIRE (s.audioEditorSnap  == def.audioEditorSnap);
    REQUIRE (s.midiEditorSnap   == def.midiEditorSnap);
    REQUIRE (s.pianoRollKeySnap == def.pianoRollKeySnap);
    REQUIRE (s.snapResolution   == def.snapResolution);
    REQUIRE_THAT (s.tempoBpm.load(), WithinAbs (def.tempoBpm.load(), 1e-6f));
    REQUIRE (s.tempoMap.empty());
    REQUIRE (s.uiStage.load() == def.uiStage.load());
    REQUIRE (s.syncSourceInputIdentifier.isEmpty());
    REQUIRE (s.externalSyncFollowsTempo.load()    == def.externalSyncFollowsTempo.load());
    REQUIRE (s.externalSyncChasesTransport.load() == def.externalSyncChasesTransport.load());
    REQUIRE (s.syncOutputIdentifier.isEmpty());
    REQUIRE (s.syncOutputEmitClock.load() == def.syncOutputEmitClock.load());
    REQUIRE (s.externalTimeCodeChasesTransport.load (std::memory_order_relaxed)
               == def.externalTimeCodeChasesTransport.load (std::memory_order_relaxed));
    REQUIRE (s.syncOutputEmitTimeCode.load (std::memory_order_relaxed)
               == def.syncOutputEmitTimeCode.load (std::memory_order_relaxed));
    REQUIRE (s.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed)
               == def.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed));
    REQUIRE (s.mcu.inputIdentifier.isEmpty());
    REQUIRE (s.mcu.outputIdentifier.isEmpty());
    REQUIRE (s.mcu.assignMode.load (std::memory_order_relaxed)
               == def.mcu.assignMode.load (std::memory_order_relaxed));
    REQUIRE (s.beatsPerBar.load() == def.beatsPerBar.load());
    REQUIRE (s.beatUnit   .load() == def.beatUnit   .load());
    REQUIRE (s.metronomeEnabled.load() == def.metronomeEnabled.load());
    REQUIRE_THAT (s.metronomeVolDb.load(), WithinAbs (def.metronomeVolDb.load(), 1e-6f));
    REQUIRE (s.metronomeClickWhileRecording.load() == def.metronomeClickWhileRecording.load());
    REQUIRE (s.metronomeClickWhilePlaying  .load() == def.metronomeClickWhilePlaying  .load());
    REQUIRE (s.metronomeOnlyDuringCountIn  .load() == def.metronomeOnlyDuringCountIn  .load());
    REQUIRE (s.metronomePolyphonic         .load() == def.metronomePolyphonic         .load());
    REQUIRE (s.countInEnabled .load() == def.countInEnabled .load());
    REQUIRE (s.timeDisplayMode.load() == def.timeDisplayMode.load());
    REQUIRE (s.lastRecordPointSamples.load() == def.lastRecordPointSamples.load());
    REQUIRE_THAT (s.preRollSeconds .load(), WithinAbs (def.preRollSeconds .load(), 1e-6f));
    REQUIRE_THAT (s.postRollSeconds.load(), WithinAbs (def.postRollSeconds.load(), 1e-6f));
    REQUIRE (s.preRollEnabled .load() == def.preRollEnabled .load());
    REQUIRE (s.postRollEnabled.load() == def.postRollEnabled.load());
    REQUIRE (s.midiBindings.current().empty());
    REQUIRE (s.oversamplingFactor.load (std::memory_order_relaxed)
               == def.oversamplingFactor.load (std::memory_order_relaxed));

    // The anchor the legacy region was stamped with has to be the tempo the
    // session actually ends up at. Anchoring it to the seeded 90 while the
    // transport resets to 120 mis-retimes the region on the first tempo change.
    const auto& midi = s.track (0).midiRegions.current();
    REQUIRE (midi.size() == 1);
    REQUIRE_THAT (midi[0].recordedAtBPM, WithinAbs ((double) def.tempoBpm.load(), 1e-6));
}

// The other half of the transport contract: a section that IS there is the
// session's description of its transport, and a key it leaves out keeps the
// live value rather than snapping to the model default.
TEST_CASE ("a partial transport section retains the keys it omits",
           "[session][serializer]")
{
    ScopedTempDir tmp { "dusk-partial-transport-" };
    const auto& dir = tmp.dir;

    Session s;
    s.setSessionDirectory (dir);
    s.beatsPerBar.store (7);
    s.uiStage.store (2);
    s.savedLoopEnabled = true;

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"transport":{"beats_per_bar":3}})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE (s.beatsPerBar.load() == 3);
    REQUIRE (s.uiStage.load() == 2);
    REQUIRE (s.savedLoopEnabled);
}
