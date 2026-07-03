#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

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

    REQUIRE (b.track (2).name == "Vocal");
    REQUIRE (b.track (2).mode.load (std::memory_order_relaxed) == (int) Track::Mode::Midi);
    REQUIRE (b.track (2).midiChannel.load (std::memory_order_relaxed) == 5);

    dir.deleteRecursively();
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

    Session a;
    auto& lane = a.auxLane (1);
    lane.nativeClapPath[0]        = "/home/user/.clap/DuskVerb.clap";
    lane.nativeClapPluginId[0]    = "com.dusk.duskverb";
    lane.nativeClapStateBase64[0] = "Q0xBUFNUQVRFYmxvYg==";
    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));
    REQUIRE (b.auxLane (1).nativeClapPath[0]        == "/home/user/.clap/DuskVerb.clap");
    REQUIRE (b.auxLane (1).nativeClapPluginId[0]    == "com.dusk.duskverb");
    REQUIRE (b.auxLane (1).nativeClapStateBase64[0] == "Q0xBUFNUQVRFYmxvYg==");

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
