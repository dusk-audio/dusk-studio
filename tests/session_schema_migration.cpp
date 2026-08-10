// H1 schema migration test. Exercises SessionSerializer::migrateSession
// directly (forward-declared below — non-static in the .cpp + lives in
// namespace duskstudio so tests can reach it without touching the
// header) AND end-to-end via SessionSerializer::load on a v1-tagged
// session JSON. Confirms:
//   1. migrateSession returns false + does not advance when asked to
//      migrate from an unknown lower version (safety branch).
//   2. migrateSession advances v1 → the current kFormatVersion on a
//      well-formed root object, and the "version" property on `root` is
//      bumped to match.
//   3. End-to-end: a v1-tagged session.json on disk loads cleanly,
//      Session deserialises, the round-trip save writes the current
//      kFormatVersion back out.
//
// These tests are the regression net for future kFormatVersion bumps:
// every migrator case added must keep test (2) green for its specific
// from-version and test (3) green for round-trip.

#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <memory>

namespace duskstudio
{
// Forward-declare the in-cpp non-static migrator. The cpp drops `static`
// from this symbol specifically so tests can reach it without a header
// change (SessionSerializer.h stays untouched for source-compat).
bool migrateSession (nlohmann::json& root, int from);
} // namespace duskstudio

namespace
{
juce::File makeTempMigrationDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-migration-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

void writeRaw (const juce::File& target, const juce::String& contents)
{
    target.deleteFile();
    target.create();
    target.replaceWithText (contents);
}
} // namespace

TEST_CASE ("migrateSession safety branch: refuses unknown lower version",
           "[session][serializer][migration]")
{
    // Mock pre-versioning (v0) — no migrator case registered. The loop's
    // default branch must return false rather than spin forever.
    nlohmann::json root = { { "tempo", 120.0 } };

    const bool ok = duskstudio::migrateSession (root, 0);
    REQUIRE_FALSE (ok);

    // Root must be untouched on the safety-branch path — caller relies
    // on this to know the document is unsalvageable.
    REQUIRE (root.is_object());
    REQUIRE_FALSE (root.contains ("version"));
}

TEST_CASE ("migrateSession advances a mock v1 root to the current schema",
           "[session][serializer][migration]")
{
    // Mock v1 root with a stable field the migrator must preserve.
    nlohmann::json root = { { "version", 1 }, { "tempo", 98.5 } };

    const bool ok = duskstudio::migrateSession (root, 1);
    REQUIRE (ok);

    // version field must now match the current build's kFormatVersion.
    // We don't reach kFormatVersion symbolically from the test (it's
    // in an anonymous namespace inside the .cpp), so we check the
    // Take provenance owns version 5; the original payload must survive every step.
    REQUIRE (root.is_object());
    REQUIRE (root.contains ("version"));
    REQUIRE (root["version"].get<int>() == 5);
    REQUIRE (root.contains ("tempo"));
    REQUIRE (root["tempo"].get<double>() == 98.5);
}

TEST_CASE ("SessionSerializer loads a v1-tagged session file end-to-end",
           "[session][serializer][migration]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempMigrationDir();
    const auto target = dir.getChildFile ("session.json");

    // Hand-written v1 session — minimum surface to exercise the load +
    // migrate path. The loader sees fileVersion < kFormatVersion and
    // routes through migrateSession before Session deserialisation.
    writeRaw (target, R"({"version":1,"tempo":124.0,"tracks":[{"name":"v1-track"}]})");

    Session s;
    REQUIRE (SessionSerializer::load (s, target));

    // Save back + verify the file is now tagged with the current
    // kFormatVersion.
    REQUIRE (SessionSerializer::save (s, target));
    auto root = nlohmann::json::parse (target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE (root.is_object());
    REQUIRE (root.contains ("version"));
    REQUIRE (root["version"].get<int>() == 5);

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer migrates a v3 legacy plugin reference to a v5 save",
           "[session][serializer][migration][plugin-descriptor]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempMigrationDir();
    const auto target = dir.getChildFile ("session.json");
    const std::string legacyXml =
        R"(<PLUGIN name="Legacy Synth" descriptiveName="Legacy Synth" format="VST3" file="/plugins/Legacy.vst3" uid="1234" manufacturer="Dusk" version="1.0" isInstrument="1"/>)";
    nlohmann::json root {
        { "version", 3 },
        { "tracks", nlohmann::json::array ({
            { { "name", "Legacy" },
              { "plugin_desc_xml", legacyXml },
              { "plugin_state", "bGVnYWN5LXN0YXRl" } }
        }) }
    };
    writeRaw (target, root.dump());

    auto session = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*session, target));
    CHECK_FALSE (session->track (0).pluginDescriptor.has_value());
    CHECK (session->track (0).pluginLegacyDescriptionXml.toStdString() == legacyXml);
    CHECK (session->track (0).pluginStateBase64 == "bGVnYWN5LXN0YXRl");

    REQUIRE (SessionSerializer::save (*session, target));
    const auto saved = nlohmann::json::parse (
        target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE (saved.is_object());
    CHECK (saved["version"].get<int>() == 5);
    CHECK (saved["tracks"][0]["plugin_desc_xml"].get<std::string>() == legacyXml);
    CHECK (saved["tracks"][0]["plugin_state"].get<std::string>()
           == "bGVnYWN5LXN0YXRl");

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer round-trips active and historical take provenance",
           "[session][serializer][migration][take-provenance]")
{
    using duskstudio::AudioRegion;
    using duskstudio::MidiRegion;
    using duskstudio::MidiTakeRef;
    using duskstudio::Session;
    using duskstudio::SessionSerializer;
    using duskstudio::TakeRef;

    const auto dir = makeTempMigrationDir();
    const auto target = dir.getChildFile ("session.json");
    auto source = std::make_unique<Session>();

    AudioRegion audio;
    audio.lengthInSamples = 4096;
    audio.sourceOffset = 32;
    audio.provenance = { 101, 2, true };
    TakeRef priorAudio;
    priorAudio.lengthInSamples = 2048;
    priorAudio.sourceOffset = 64;
    priorAudio.provenance = { 102, 3, false };
    audio.previousTakes.push_back (priorAudio);
    source->track (0).regions.push_back (audio);

    MidiRegion midi;
    midi.lengthInSamples = 8192;
    midi.lengthInTicks = 1920;
    midi.provenance = { 103, 4, false };
    MidiTakeRef priorMidi;
    priorMidi.lengthInTicks = 960;
    priorMidi.provenance = { 104, 5, true };
    midi.previousTakes.push_back (priorMidi);
    source->track (1).midiRegions.mutate (
        [&] (auto& regions) { regions.push_back (midi); });

    REQUIRE (SessionSerializer::save (*source, target));
    const auto saved = nlohmann::json::parse (
        target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE (saved["version"].get<int>() == 5);
    const auto& savedAudio = saved["tracks"][0]["regions"][0];
    CHECK (savedAudio["take_provenance"]["captured_at_ms"].get<std::int64_t>() == 101);
    CHECK (savedAudio["take_provenance"]["loop_pass"].get<int>() == 2);
    CHECK (savedAudio["take_provenance"]["partial"].get<bool>());
    const auto& savedPriorAudio = savedAudio["previous_takes"][0];
    CHECK (savedPriorAudio["take_provenance"]["captured_at_ms"].get<std::int64_t>() == 102);
    CHECK (savedPriorAudio["take_provenance"]["loop_pass"].get<int>() == 3);
    CHECK_FALSE (savedPriorAudio["take_provenance"].contains ("partial"));
    const auto& savedMidi = saved["tracks"][1]["midi_regions"][0];
    CHECK (savedMidi["take_provenance"]["captured_at_ms"].get<std::int64_t>() == 103);
    CHECK (savedMidi["take_provenance"]["loop_pass"].get<int>() == 4);
    CHECK_FALSE (savedMidi["take_provenance"].contains ("partial"));
    const auto& savedPriorMidi = savedMidi["previous_takes"][0];
    CHECK (savedPriorMidi["take_provenance"]["captured_at_ms"].get<std::int64_t>() == 104);
    CHECK (savedPriorMidi["take_provenance"]["loop_pass"].get<int>() == 5);
    CHECK (savedPriorMidi["take_provenance"]["partial"].get<bool>());

    auto loaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*loaded, target));
    const auto& loadedAudio = loaded->track (0).regions[0];
    CHECK (loadedAudio.provenance.capturedAtMs == 101);
    CHECK (loadedAudio.provenance.loopPassOrdinal == 2);
    CHECK (loadedAudio.provenance.partialPass);
    REQUIRE (loadedAudio.previousTakes.size() == 1);
    CHECK (loadedAudio.previousTakes[0].provenance.capturedAtMs == 102);
    CHECK (loadedAudio.previousTakes[0].provenance.loopPassOrdinal == 3);
    CHECK_FALSE (loadedAudio.previousTakes[0].provenance.partialPass);
    const auto& loadedMidi = loaded->track (1).midiRegions.current()[0];
    CHECK (loadedMidi.provenance.capturedAtMs == 103);
    CHECK (loadedMidi.provenance.loopPassOrdinal == 4);
    CHECK_FALSE (loadedMidi.provenance.partialPass);
    REQUIRE (loadedMidi.previousTakes.size() == 1);
    CHECK (loadedMidi.previousTakes[0].provenance.capturedAtMs == 104);
    CHECK (loadedMidi.previousTakes[0].provenance.loopPassOrdinal == 5);
    CHECK (loadedMidi.previousTakes[0].provenance.partialPass);

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer gives legacy v4 takes default provenance",
           "[session][serializer][migration][take-provenance]")
{
    using duskstudio::AudioRegion;
    using duskstudio::MidiRegion;
    using duskstudio::MidiTakeRef;
    using duskstudio::Session;
    using duskstudio::SessionSerializer;
    using duskstudio::TakeRef;

    const auto dir = makeTempMigrationDir();
    const auto target = dir.getChildFile ("session.json");
    auto source = std::make_unique<Session>();

    AudioRegion audio;
    audio.lengthInSamples = 256;
    audio.previousTakes.push_back (TakeRef {});
    source->track (0).regions.push_back (audio);
    MidiRegion midi;
    midi.lengthInSamples = 512;
    midi.lengthInTicks = 120;
    midi.previousTakes.push_back (MidiTakeRef {});
    source->track (1).midiRegions.mutate (
        [&] (auto& regions) { regions.push_back (midi); });

    REQUIRE (SessionSerializer::save (*source, target));
    auto legacy = nlohmann::json::parse (
        target.loadFileAsString().toStdString(), nullptr, false);
    REQUIRE_FALSE (legacy["tracks"][0]["regions"][0].contains ("take_provenance"));
    REQUIRE_FALSE (legacy["tracks"][0]["regions"][0]["previous_takes"][0]
                       .contains ("take_provenance"));
    REQUIRE_FALSE (legacy["tracks"][1]["midi_regions"][0].contains ("take_provenance"));
    REQUIRE_FALSE (legacy["tracks"][1]["midi_regions"][0]["previous_takes"][0]
                       .contains ("take_provenance"));
    legacy["version"] = 4;
    writeRaw (target, legacy.dump());

    auto loaded = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*loaded, target));
    const auto& loadedAudio = loaded->track (0).regions[0];
    CHECK (loadedAudio.provenance.capturedAtMs == 0);
    CHECK (loadedAudio.provenance.loopPassOrdinal == 0);
    CHECK_FALSE (loadedAudio.provenance.partialPass);
    REQUIRE (loadedAudio.previousTakes.size() == 1);
    CHECK (loadedAudio.previousTakes[0].provenance.capturedAtMs == 0);
    CHECK (loadedAudio.previousTakes[0].provenance.loopPassOrdinal == 0);
    CHECK_FALSE (loadedAudio.previousTakes[0].provenance.partialPass);
    const auto& loadedMidi = loaded->track (1).midiRegions.current()[0];
    CHECK (loadedMidi.provenance.capturedAtMs == 0);
    CHECK (loadedMidi.provenance.loopPassOrdinal == 0);
    CHECK_FALSE (loadedMidi.provenance.partialPass);
    REQUIRE (loadedMidi.previousTakes.size() == 1);
    CHECK (loadedMidi.previousTakes[0].provenance.capturedAtMs == 0);
    CHECK (loadedMidi.previousTakes[0].provenance.loopPassOrdinal == 0);
    CHECK_FALSE (loadedMidi.previousTakes[0].provenance.partialPass);

    legacy["tracks"][0]["regions"][0]["take_provenance"] = {
        { "captured_at_ms", -12 }, { "loop_pass", -4 }, { "partial", true }
    };
    writeRaw (target, legacy.dump());
    auto clamped = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*clamped, target));
    const auto& clampedProvenance = clamped->track (0).regions[0].provenance;
    CHECK (clampedProvenance.capturedAtMs == 0);
    CHECK (clampedProvenance.loopPassOrdinal == 0);
    CHECK (clampedProvenance.partialPass);

    REQUIRE (SessionSerializer::save (*clamped, target));
    const auto upgraded = nlohmann::json::parse (
        target.loadFileAsString().toStdString(), nullptr, false);
    CHECK (upgraded["version"].get<int>() == 5);

    dir.deleteRecursively();
}

TEST_CASE ("Take payload helpers keep provenance attached to audio and MIDI takes",
           "[session][take-provenance]")
{
    using namespace duskstudio;

    AudioRegion audio;
    audio.file = decltype (audio.file) ("/tmp/active-take.wav");
    audio.sourceOffset = 11;
    audio.lengthInSamples = 22;
    audio.provenance = { 100, 1, false };

    TakeRef priorAudio;
    priorAudio.file = decltype (priorAudio.file) ("/tmp/prior-take.wav");
    priorAudio.sourceOffset = 33;
    priorAudio.lengthInSamples = 44;
    priorAudio.provenance = { 200, 2, true };

    swapAudioTakePayload (audio, priorAudio);
    CHECK (audio.file.getFileName() == "prior-take.wav");
    CHECK (audio.sourceOffset == 33);
    CHECK (audio.provenance.loopPassOrdinal == 2);
    CHECK (audio.provenance.partialPass);
    CHECK (priorAudio.file.getFileName() == "active-take.wav");
    CHECK (priorAudio.sourceOffset == 11);
    CHECK (priorAudio.provenance.capturedAtMs == 100);

    const auto copiedAudio = makeAudioTakeRef (audio);
    AudioRegion restoredAudio;
    applyAudioTakeRef (restoredAudio, copiedAudio);
    CHECK (restoredAudio.lengthInSamples == 44);
    CHECK (restoredAudio.provenance.capturedAtMs == 200);

    MidiRegion midi;
    midi.lengthInTicks = 960;
    midi.notes.push_back ({ 1, 60, 100, 0, 240 });
    midi.provenance = { 300, 3, false };

    MidiTakeRef priorMidi;
    priorMidi.lengthInTicks = 480;
    priorMidi.notes.push_back ({ 1, 48, 90, 0, 120 });
    priorMidi.provenance = { 400, 4, true };

    swapMidiTakePayload (midi, priorMidi);
    CHECK (midi.lengthInTicks == 480);
    REQUIRE (midi.notes.size() == 1);
    CHECK (midi.notes[0].noteNumber == 48);
    CHECK (midi.provenance.loopPassOrdinal == 4);
    CHECK (midi.provenance.partialPass);
    CHECK (priorMidi.lengthInTicks == 960);
    REQUIRE (priorMidi.notes.size() == 1);
    CHECK (priorMidi.notes[0].noteNumber == 60);
    CHECK (priorMidi.provenance.capturedAtMs == 300);

    const auto copiedMidi = makeMidiTakeRef (midi);
    MidiRegion restoredMidi;
    applyMidiTakeRef (restoredMidi, copiedMidi);
    CHECK (restoredMidi.lengthInTicks == 480);
    CHECK (restoredMidi.provenance.capturedAtMs == 400);
}
