#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

// A truncated / hand-edited session.json can lack whole section keys
// ("tracks", "buses", "aux_lanes"). Loading such a file over a populated
// session used to skip those sections entirely, leaving the previous
// session's regions, plugins and mixer state alive under the new session's
// name. load() now substitutes serialized defaults for any missing section.
TEST_CASE ("loading a session without section keys resets those sections",
           "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-missing-sections-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-null-track-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-null-bus-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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

// #145 closed the case of a slot that is missing or not an object. A slot that
// IS listed but only partially populated has the same hole: every mixer setter
// is conditional on its key being present, so each key the file leaves out kept
// the previously loaded session's value. The file's slot is now overlaid on the
// serialized model default, so an absent key resolves to the default instead.
TEST_CASE ("loading a session with a partial track slot resets its absent keys",
           "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-partial-track-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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

// RFC 7396 reads an explicit null as "delete this key", which would drop the
// key out of the overlay and land the slot back on the live value - the exact
// failure the overlay exists to prevent. Here a null can only mean unspecified.
TEST_CASE ("a null member in a partial slot resolves to the model default",
           "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-null-member-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    Session s;
    s.setSessionDirectory (dir);
    s.track (0).strip.faderDb.store (-12.0f);
    s.bus (0).strip.faderDb.store (-9.0f);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (
        R"({"version":3,"tracks":[{"fader_db":null}],"buses":[{"fader_db":null}]})");

    REQUIRE (SessionSerializer::load (s, target));

    REQUIRE_THAT (s.track (0).strip.faderDb.load(), WithinAbs (0.0f, 1e-6f));
    REQUIRE_THAT (s.bus (0).strip.faderDb.load(),   WithinAbs (0.0f, 1e-6f));
}

// The overlay must not merge arrays element-wise: a shorter bus_assign has to
// replace the default outright, or a slot that assigns nothing would inherit
// assignments from the default's longer array.
TEST_CASE ("a partial slot's arrays replace rather than merge", "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-array-replace-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

    Session s;
    s.setSessionDirectory (dir);

    AudioRegion r;
    r.file            = s.getAudioDirectory().getChildFile ("ghost.wav");
    r.lengthInSamples = 1000;
    s.track (0).regions.push_back (r);

    const auto target = dir.getChildFile ("session.json");
    target.replaceWithText (R"({"version":3,"tracks":[{"regions":[]}]})");

    REQUIRE (SessionSerializer::load (s, target));
    REQUIRE (s.track (0).regions.empty());
}

// The stable identifier wins when there is one, but an unrouted track
// serializes midi_input_id as "", so the overlay hands every slot that key.
// Keying the identifier path on presence would swallow a session carrying only
// the legacy raw index.
TEST_CASE ("a legacy MIDI index survives the default overlay", "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-legacy-midi-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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
// is driven, each overlaid on the matching default.
TEST_CASE ("an aux lane's undescribed plugin slot drops the previous plugin",
           "[session][serializer]")
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-aux-slot-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };

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
