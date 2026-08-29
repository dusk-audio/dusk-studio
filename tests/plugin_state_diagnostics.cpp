#include <catch2/catch_test_macros.hpp>

#include "engine/PluginStateDiagnostics.h"
#include "engine/hosting/NativeStateIdentity.h"
#include "engine/hosting/NativeRestorePolicy.h"
#include "session/Session.h"

TEST_CASE ("aux native-state rejection diagnostics identify lane and slot",
           "[session][native][diagnostics]")
{
    for (const char* format : { "CLAP", "LV2", "VST3", "AU" })
    {
        INFO (format);
        REQUIRE (duskstudio::pluginstate::stateRejectedMessage (
                     std::string ("aux ") + format, 1, 4096, 2)
                 == std::string ("[Dusk Studio/session] aux ") + format
                    + " lane 3 slot 2 rejected its saved state (4096 bytes); "
                      "slot left offline to preserve the saved state");
    }

    REQUIRE (duskstudio::pluginstate::stateRejectedMessage (
                 "track CLAP", 0, 128)
             == "[Dusk Studio/session] track CLAP 1 rejected its saved state "
                "(128 bytes); slot left offline to preserve the saved state");
}

TEST_CASE ("native restore rejection unloads the slot and preserves the saved-state reason",
           "[session][native][regression][issue-386]")
{
    bool unloaded = false;
    const auto reason = duskstudio::hosting::enforceRestorePolicy (
        true, true, false, {}, 37, [&] { unloaded = true; });
    REQUIRE (unloaded);
    REQUIRE (reason == "saved state was rejected (37 bytes); slot left offline "
                       "to preserve the saved state");
}

TEST_CASE ("native restore policy keeps healthy slots saveable and reports load reasons",
           "[session][native][regression][issue-386]")
{
    bool unloaded = false;
    REQUIRE (duskstudio::hosting::enforceRestorePolicy (
                 true, true, true, {}, 12, [&] { unloaded = true; }).empty());
    REQUIRE_FALSE (unloaded);

    const auto reason = duskstudio::hosting::enforceRestorePolicy (
        false, false, false, "component factory returned null", 0,
        [&] { unloaded = true; });
    REQUIRE (unloaded);
    REQUIRE (reason == "plug-in load failed: component factory returned null; "
                       "slot left offline");

    REQUIRE (duskstudio::pluginstate::restoreFailureLine (
                 "Aux 2 slot 3", "Echo", "VST3", reason)
             == "Aux 2 slot 3 - Echo [VST3]: " + reason);

    REQUIRE (duskstudio::hosting::nativePluginName (
                 "/plugins/Multi Bus.clap", "studio.dusk.test.multi-bus")
             == "Multi Bus");
    REQUIRE (duskstudio::hosting::nativePluginName (
                 {}, "http://example.test/plugins/reverb")
             == "http://example.test/plugins/reverb");
}

TEST_CASE ("native reactivation failure quarantines rather than unloads",
           "[session][native][regression][issue-386]")
{
    bool quarantined = false;
    const auto reason = duskstudio::hosting::enforceReactivationPolicy (
        false, "activate rejected 192 kHz", [&] { quarantined = true; });
    REQUIRE (quarantined);
    REQUIRE (reason == "plug-in reactivation failed: activate rejected 192 kHz; "
                       "slot left offline and instance retained for safe editor teardown");

    quarantined = false;
    REQUIRE (duskstudio::hosting::enforceReactivationPolicy (
                 true, {}, [&] { quarantined = true; }).empty());
    REQUIRE_FALSE (quarantined);

    bool unloaded = false;
    const auto lostReason = duskstudio::hosting::enforceReactivationPolicy (
        false, "instantiate failed", false,
        [&] { quarantined = true; }, [&] { unloaded = true; });
    REQUIRE_FALSE (quarantined);
    REQUIRE (unloaded);
    REQUIRE (lostReason == "plug-in reactivation failed: instantiate failed; "
                           "plug-in instance was lost and the slot was unloaded");
}

TEST_CASE ("track replacement cannot retain the predecessor native state blob",
           "[session][native][regression][issue-389]")
{
    using duskstudio::hosting::NativeStateIdentity;
    using duskstudio::hosting::retainStateForLiveIdentity;

    duskstudio::Session session;
    auto& track = session.track (3);
    track.nativeClapPath = "/plugins/synths.clap";
    track.nativeClapPluginId = "studio.dusk.synth-a";
    track.nativeClapStateBase64 = "opaque state from synth A";

    const NativeStateIdentity predecessor {
        "CLAP", track.nativeClapPath.toStdString(),
        track.nativeClapPluginId.toStdString() };
    const NativeStateIdentity replacement {
        "CLAP", "/plugins/synths.clap", "studio.dusk.synth-b" };

    REQUIRE_FALSE (retainStateForLiveIdentity (
        predecessor, replacement, track.nativeClapStateBase64));
    REQUIRE (track.nativeClapStateBase64.isEmpty());
}

TEST_CASE ("aux replacement fallback requires the exact native state owner",
           "[session][native][regression][issue-389]")
{
    using duskstudio::hosting::NativeStateIdentity;
    using duskstudio::hosting::retainStateForLiveIdentity;

    duskstudio::Session session;
    auto& lane = session.auxLane (1);
    constexpr size_t slot = 2;
    lane.nativeVst3Path[slot] = "/plugins/Delay.vst3";
    lane.nativeVst3PluginId[slot] = "com.dusk.delay";

    const NativeStateIdentity owner {
        "VST3", lane.nativeVst3Path[slot].toStdString(),
        lane.nativeVst3PluginId[slot].toStdString() };

    SECTION ("an unchanged identity retains its last good capture")
    {
        lane.nativeVst3StateBase64[slot] = "last good delay state";
        REQUIRE (retainStateForLiveIdentity (
            owner, owner, lane.nativeVst3StateBase64[slot]));
        REQUIRE (lane.nativeVst3StateBase64[slot] == "last good delay state");
    }

    SECTION ("a different bundle path invalidates the fallback")
    {
        lane.nativeVst3StateBase64[slot] = "state from the old aux plug-in";
        REQUIRE_FALSE (retainStateForLiveIdentity (
            owner,
            { "VST3", "/plugins/Reverb.vst3", "com.dusk.delay" },
            lane.nativeVst3StateBase64[slot]));
        REQUIRE (lane.nativeVst3StateBase64[slot].isEmpty());
    }

    SECTION ("a different host format invalidates the fallback")
    {
        lane.nativeVst3StateBase64[slot] = "state from the old aux plug-in";
        REQUIRE_FALSE (retainStateForLiveIdentity (
            owner,
            { "AU", "/plugins/Delay.vst3", "com.dusk.delay" },
            lane.nativeVst3StateBase64[slot]));
        REQUIRE (lane.nativeVst3StateBase64[slot].isEmpty());
    }
}
