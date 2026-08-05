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
