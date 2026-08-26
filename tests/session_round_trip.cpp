#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/Base64.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

using Catch::Matchers::WithinAbs;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-session-round-trip-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// Round-trip safety net for SessionSerializer. A real Patreon user opens
// a session, mutates a handful of audible parameters, saves, reloads —
// expects every parameter to come back exactly. Without this regression
// guard, a JSON-key rename or a missed field in serialise/deserialise
// silently zeros user data on next load.
TEST_CASE ("SessionSerializer round-trip preserves transport + per-track state",
           "[session][serializer]")
{
    using duskstudio::Session;
    using duskstudio::Track;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.tempoBpm.store (137.0f, std::memory_order_relaxed);

    auto& t0 = a.track (0);
    t0.name = "Kick";
    t0.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    a.setTrackArmed (0, true);
    t0.strip.faderDb.store (-3.5f, std::memory_order_relaxed);
    t0.strip.pan.store (-0.25f, std::memory_order_relaxed);

    auto& t1 = a.track (1);
    t1.name = "Bass DI";
    t1.mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    t1.strip.faderDb.store (1.5f, std::memory_order_relaxed);
    t1.strip.hpfFreq.store (80.0f, std::memory_order_relaxed);
    t1.strip.lfGainDb.store (2.0f, std::memory_order_relaxed);
    t1.strip.insertBypassed.store (true, std::memory_order_relaxed);
    t1.strip.auxSendsBypassed.store (true, std::memory_order_relaxed);

    auto& t2 = a.track (2);
    t2.name = "Vocal";
    t2.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    t2.midiChannel.store (5, std::memory_order_relaxed);

    a.tempoMap.setPoints ({ { 0, 120.0f }, { 96000, 90.0f } });

    REQUIRE (SessionSerializer::save (a, target));
    REQUIRE (target.existsAsFile());

    Session b;
    REQUIRE (SessionSerializer::load (b, target));

    REQUIRE_THAT (b.tempoBpm.load (std::memory_order_relaxed),
                  WithinAbs (137.0f, 1e-4f));

    REQUIRE (b.tempoMap.points().size() == 2);
    REQUIRE (b.tempoMap.points()[0].timelineSamples == 0);
    REQUIRE_THAT (b.tempoMap.points()[0].bpm, WithinAbs (120.0f, 1e-4f));
    REQUIRE (b.tempoMap.points()[1].timelineSamples == 96000);
    REQUIRE_THAT (b.tempoMap.points()[1].bpm, WithinAbs (90.0f, 1e-4f));

    REQUIRE (b.track (0).name == "Kick");
    REQUIRE (b.track (0).mode.load (std::memory_order_relaxed) == (int) Track::Mode::Mono);
    // recordArmed is a per-take volatile flag; SessionSerializer
    // intentionally drops it on save (a session opens with no tracks
    // armed by default). No assertion here.
    REQUIRE_THAT (b.track (0).strip.faderDb.load (std::memory_order_relaxed),
                  WithinAbs (-3.5f, 1e-4f));
    REQUIRE_THAT (b.track (0).strip.pan.load (std::memory_order_relaxed),
                  WithinAbs (-0.25f, 1e-4f));

    REQUIRE (b.track (1).name == "Bass DI");
    REQUIRE (b.track (1).mode.load (std::memory_order_relaxed) == (int) Track::Mode::Stereo);
    REQUIRE_THAT (b.track (1).strip.faderDb.load (std::memory_order_relaxed),
                  WithinAbs (1.5f, 1e-4f));
    REQUIRE_THAT (b.track (1).strip.hpfFreq.load (std::memory_order_relaxed),
                  WithinAbs (80.0f, 1e-3f));
    REQUIRE_THAT (b.track (1).strip.lfGainDb.load (std::memory_order_relaxed),
                  WithinAbs (2.0f, 1e-4f));
    REQUIRE (b.track (1).strip.insertBypassed.load (std::memory_order_relaxed));
    REQUIRE_FALSE (b.track (0).strip.insertBypassed.load (std::memory_order_relaxed));
    REQUIRE (b.track (1).strip.auxSendsBypassed.load (std::memory_order_relaxed));
    REQUIRE_FALSE (b.track (0).strip.auxSendsBypassed.load (std::memory_order_relaxed));

    REQUIRE (b.track (2).name == "Vocal");
    REQUIRE (b.track (2).mode.load (std::memory_order_relaxed) == (int) Track::Mode::Midi);
    REQUIRE (b.track (2).midiChannel.load (std::memory_order_relaxed) == 5);

    dir.deleteRecursively();
}

// The MTC settings were written by the Audio Settings panel but never
// serialized, so chase / emit / frame rate reset on every reload. Absent keys
// must reset to the model default rather than inherit the previous session's.
TEST_CASE ("SessionSerializer round-trips the MTC sync settings",
           "[session][serializer][sync]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("dusk-mtc-"
                                         + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    const struct ScopedDir { juce::File d; ~ScopedDir() { d.deleteRecursively(); } } scopedDir { dir };
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.setSessionDirectory (dir);
    a.externalTimeCodeChasesTransport.store (true, std::memory_order_relaxed);
    a.syncOutputEmitTimeCode.store (true, std::memory_order_relaxed);
    a.syncOutputTimeCodeFrameRate.store (2, std::memory_order_relaxed);
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    b.setSessionDirectory (dir);
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.externalTimeCodeChasesTransport.load (std::memory_order_relaxed));
    REQUIRE (b.syncOutputEmitTimeCode.load (std::memory_order_relaxed));
    REQUIRE (b.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed) == 2);

    // A session predating the keys resets to the defaults, not the live values.
    REQUIRE (target.replaceWithText (R"({"version":3,"transport":{}})"));
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE_FALSE (b.externalTimeCodeChasesTransport.load (std::memory_order_relaxed));
    REQUIRE_FALSE (b.syncOutputEmitTimeCode.load (std::memory_order_relaxed));
    REQUIRE (b.syncOutputTimeCodeFrameRate.load (std::memory_order_relaxed) == 3);
}

TEST_CASE ("SessionSerializer save is atomic - tmp file gone after success",
           "[session][serializer]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.tempoBpm.store (90.0f, std::memory_order_relaxed);
    REQUIRE (SessionSerializer::save (a, target));

    // The atomic-save pattern writes to <target>.tmp then renames into
    // place. After a successful save the tmp must NOT linger — a stale
    // tmp from a prior incomplete save could fool a recovery script.
    REQUIRE (target.existsAsFile());
    REQUIRE_FALSE (dir.getChildFile ("session.json.tmp").exists());

    dir.deleteRecursively();
}

// Native CLAP aux slots persist as a path + base64 state pair, parallel to the
// JUCE plugin pair. The picker (3c) and engine restore consume these; this guards
// the serializer format so the data survives a save/reload before that lands.
TEST_CASE ("SessionSerializer round-trips an aux native-CLAP slot", "[session][serializer][clap]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    // Encoded the way AudioEngine::publishPluginStateForSave writes it, so the
    // stored string is checked against the decoder the restore path uses instead
    // of only against itself.
    const std::vector<std::uint8_t> state { 0x00, 0x01, 0xfe, 0xff, 0x7f, 0x80, 0x2b, 0x2f };
    const auto encoded = juce::Base64::toBase64 (state.data(), state.size());

    Session a;
    auto& lane = a.auxLane (1);
    lane.nativeClapPath[0]        = "/home/user/.clap/DuskVerb.clap";
    lane.nativeClapPluginId[0]    = "com.dusk.duskverb";
    lane.nativeClapStateBase64[0] = encoded;
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.auxLane (1).nativeClapPath[0]        == "/home/user/.clap/DuskVerb.clap");
    REQUIRE (b.auxLane (1).nativeClapPluginId[0]    == "com.dusk.duskverb");

    const auto& stored = b.auxLane (1).nativeClapStateBase64[0];
    REQUIRE (stored == encoded);
    REQUIRE (dusk::base64::decode (stored.toRawUTF8(), stored.getNumBytesAsUTF8()) == state);

    // A lane with no native CLAP stays empty (keys omitted on write).
    REQUIRE (b.auxLane (0).nativeClapPath[0].isEmpty());
    REQUIRE (b.auxLane (0).nativeClapStateBase64[0].isEmpty());

    dir.deleteRecursively();
}

// Same guard for the native-LV2 pair, on both a track and an aux slot.
TEST_CASE ("SessionSerializer round-trips native-LV2 slots", "[session][serializer][lv2]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.track (2).nativeLv2Path         = "/home/user/.lv2/SilkVerb.lv2";
    a.track (2).nativeLv2PluginId     = "https://dusk.audio/silkverb";
    a.track (2).nativeLv2StateBase64  = "TFYyU1RBVEVibG9i";
    auto& lane = a.auxLane (1);
    lane.nativeLv2Path[0]        = "/usr/lib64/lv2/a-comp.lv2";
    lane.nativeLv2StateBase64[0] = "TFYyU1RBVEVibG9i";
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.track (2).nativeLv2Path        == "/home/user/.lv2/SilkVerb.lv2");
    REQUIRE (b.track (2).nativeLv2PluginId    == "https://dusk.audio/silkverb");
    REQUIRE (b.track (2).nativeLv2StateBase64 == "TFYyU1RBVEVibG9i");
    REQUIRE (b.auxLane (1).nativeLv2Path[0]        == "/usr/lib64/lv2/a-comp.lv2");
    REQUIRE (b.auxLane (1).nativeLv2StateBase64[0] == "TFYyU1RBVEVibG9i");

    // Untouched slots stay empty (keys omitted on write).
    REQUIRE (b.track (0).nativeLv2Path.isEmpty());
    REQUIRE (b.auxLane (0).nativeLv2Path[0].isEmpty());

    dir.deleteRecursively();
}

// Same guard for the native-VST3 pair, on both a track and an aux slot.
TEST_CASE ("SessionSerializer round-trips native-VST3 slots", "[session][serializer][vst3]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.track (2).nativeVst3Path        = "/home/user/.vst3/SilkVerb.vst3";
    a.track (2).nativeVst3PluginId    = "ABCDEF0123456789ABCDEF0123456789";
    a.track (2).nativeVst3StateBase64 = "VlNUM1NUQVRFYmxvYg==";
    auto& lane = a.auxLane (1);
    lane.nativeVst3Path[0]        = "/usr/lib/vst3/DuskVerb.vst3";
    lane.nativeVst3StateBase64[0] = "VlNUM1NUQVRFYmxvYg==";
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.track (2).nativeVst3Path        == "/home/user/.vst3/SilkVerb.vst3");
    REQUIRE (b.track (2).nativeVst3PluginId    == "ABCDEF0123456789ABCDEF0123456789");
    REQUIRE (b.track (2).nativeVst3StateBase64 == "VlNUM1NUQVRFYmxvYg==");
    REQUIRE (b.auxLane (1).nativeVst3Path[0]        == "/usr/lib/vst3/DuskVerb.vst3");
    REQUIRE (b.auxLane (1).nativeVst3StateBase64[0] == "VlNUM1NUQVRFYmxvYg==");

    // Untouched slots stay empty (keys omitted on write).
    REQUIRE (b.track (0).nativeVst3Path.isEmpty());
    REQUIRE (b.auxLane (0).nativeVst3Path[0].isEmpty());

    dir.deleteRecursively();
}

// Audio Units use their stable type/subtype/manufacturer identifier in place
// of a filesystem path.  Keep the keys additive on both channel and aux slots.
TEST_CASE ("SessionSerializer round-trips native-AU slots", "[session][serializer][au]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.track (2).nativeAuIdentifier = "AudioUnit:Effects/aufx,dely,appl";
    a.track (2).nativeAuStateBase64 = "QVVTVEFURWJsb2I=";
    auto& lane = a.auxLane (1);
    lane.nativeAuIdentifier[0] = "AudioUnit:Effects/aufx,lpas,appl";
    lane.nativeAuStateBase64[0] = "QVVTVEFURWJsb2I=";
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.track (2).nativeAuIdentifier == "AudioUnit:Effects/aufx,dely,appl");
    REQUIRE (b.track (2).nativeAuStateBase64 == "QVVTVEFURWJsb2I=");
    REQUIRE (b.auxLane (1).nativeAuIdentifier[0] == "AudioUnit:Effects/aufx,lpas,appl");
    REQUIRE (b.auxLane (1).nativeAuStateBase64[0] == "QVVTVEFURWJsb2I=");

    REQUIRE (b.track (0).nativeAuIdentifier.isEmpty());
    REQUIRE (b.auxLane (0).nativeAuIdentifier[0].isEmpty());

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer round-trips native multisample references",
           "[session][serializer][multisample]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session source;
    source.track (3).nativeMultisamplePath = "/banks/portable/full-range.sfz";
    source.track (3).nativeMultisampleStateBase64 = "bXVsdGlzYW1wbGU=";
    REQUIRE (SessionSerializer::save (source, target));

    Session restored;
    REQUIRE (SessionSerializer::load (restored, target));
    CHECK (restored.track (3).nativeMultisamplePath
           == "/banks/portable/full-range.sfz");
    CHECK (restored.track (3).nativeMultisampleStateBase64
           == "bXVsdGlzYW1wbGU=");

    dir.deleteRecursively();
}

// The canonical session sample rate must survive save/load, reset to 0 when
// absent (legacy file), and reject garbage — the load UI keys the device-
// switch / mismatch warning off it.
TEST_CASE ("SessionSerializer round-trips the session sample rate", "[session][serializer]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.sessionSampleRate = 44100.0;
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    b.sessionSampleRate = 96000.0;   // must be overwritten, not inherited
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.sessionSampleRate == 44100.0);

    // Legacy file without the key resets to 0 (adopt-device-rate signal).
    target.replaceWithText (R"({"version":3,"tracks":[],"buses":[],"aux_lanes":[]})");
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.sessionSampleRate == 0.0);

    // Out-of-range / non-finite values are rejected.
    target.replaceWithText (R"({"version":3,"session_sample_rate":1e40,"tracks":[],"buses":[],"aux_lanes":[]})");
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.sessionSampleRate == 0.0);

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer round-trips structured plugin descriptors and legacy fallback",
           "[session][serializer][plugin-descriptor]")
{
    using duskstudio::PluginBackend;
    using duskstudio::PluginDescriptor;
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    PluginDescriptor descriptor;
    descriptor.name = "Offline Synth";
    descriptor.manufacturer = "Dusk";
    descriptor.formatName = "FutureFormat";
    descriptor.backend = PluginBackend::JuceLegacy;
    descriptor.location = "/missing/Offline.future";
    descriptor.pluginId = "inner.plugin";
    descriptor.uniqueId = 1234;
    descriptor.deprecatedUid = 4321;
    descriptor.numInputChannels = 0;
    descriptor.numOutputChannels = 2;
    descriptor.lastFileModificationMs = 987654321;
    descriptor.lastInfoUpdateMs = 123456789;
    descriptor.isInstrument = true;
    descriptor.hasSharedContainer = true;
    descriptor.hasAraExtension = true;

    auto source = std::make_unique<Session>();
    source->track (0).pluginDescriptor = descriptor;
    source->track (0).pluginStateBase64 = "c3RhdGU=";
    source->auxLane (0).pluginDescriptor[0] = descriptor;
    source->auxLane (0).pluginStateBase64[0] = "YXV4";
    source->track (1).pluginLegacyDescriptionXml = "<BROKEN legacy=\"keep me\"";
    source->track (1).pluginStateBase64 = "bGVnYWN5";

    REQUIRE (SessionSerializer::save (*source, target));
    const auto saved = target.loadFileAsString();
    CHECK (saved.contains ("\"plugin_descriptor\""));
    CHECK_FALSE (saved.contains ("Offline.future\\ninner.plugin"));

    auto restored = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*restored, target));
    REQUIRE (restored->track (0).pluginDescriptor.has_value());
    CHECK (*restored->track (0).pluginDescriptor == descriptor);
    CHECK (restored->track (0).pluginStateBase64 == "c3RhdGU=");
    REQUIRE (restored->auxLane (0).pluginDescriptor[0].has_value());
    CHECK (*restored->auxLane (0).pluginDescriptor[0] == descriptor);
    CHECK (restored->auxLane (0).pluginStateBase64[0] == "YXV4");
    CHECK_FALSE (restored->track (1).pluginDescriptor.has_value());
    CHECK (restored->track (1).pluginLegacyDescriptionXml
           == "<BROKEN legacy=\"keep me\"");
    CHECK (restored->track (1).pluginStateBase64 == "bGVnYWN5");

    dir.deleteRecursively();
}

TEST_CASE ("SessionSerializer preserves XML fallback for rejected structured descriptors",
           "[session][serializer][plugin-descriptor]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");
    const juce::String trackFallback ("<PLUGIN name=\"track fallback\"/>");
    const juce::String auxFallback ("<PLUGIN name=\"aux fallback\"/>");

    auto source = std::make_unique<Session>();
    source->track (0).pluginLegacyDescriptionXml = trackFallback;
    source->auxLane (0).pluginLegacyDescriptionXml[0] = auxFallback;
    REQUIRE (SessionSerializer::save (*source, target));

    auto document = nlohmann::json::parse (
        target.loadFileAsString().toStdString());
    const auto rejected = nlohmann::json {
        { "version", 1 },
        { "backend", "native" },
        { "unique_id", "not an integer" }
    };
    document["tracks"][0]["plugin_descriptor"] = rejected;
    document["aux_lanes"][0]["plugin_slots"][0]["plugin_descriptor"] = rejected;
    REQUIRE (target.replaceWithText (document.dump()));

    auto restored = std::make_unique<Session>();
    REQUIRE (SessionSerializer::load (*restored, target));
    CHECK_FALSE (restored->track (0).pluginDescriptor.has_value());
    CHECK (restored->track (0).pluginLegacyDescriptionXml == trackFallback);
    CHECK_FALSE (restored->auxLane (0).pluginDescriptor[0].has_value());
    CHECK (restored->auxLane (0).pluginLegacyDescriptionXml[0] == auxFallback);

    dir.deleteRecursively();
}
