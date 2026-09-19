#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/AuxLaneStrip.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrackIndex   = 4;
constexpr int kAuxLaneIndex = 3;
constexpr int kAuxSlotIndex = 0;
constexpr const char* kPluginId = "studio.dusk.test.multi-bus";

ScenarioResult runRoundTrip (ScenarioContext& ctx)
{
    const auto fixture = *ctx.fixture ("multi_bus.clap");
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    auto& trackStrip = engine.getChannelStrip (kTrackIndex);
    auto& auxStrip = engine.getAuxLaneStrip (kAuxLaneIndex);

    std::string error;
    const bool trackLoaded = trackStrip.getNativeClapSlot().load (
        fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error, kPluginId);
    if (! ctx.expect (trackLoaded, "could not load the track fixture"))
        ctx.note ("track load error: " + error);

    error.clear();
    const bool auxLoaded = auxStrip.getNativeClapSlot (kAuxSlotIndex).load (
        fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error, kPluginId);
    if (! ctx.expect (auxLoaded, "could not load the aux fixture"))
        ctx.note ("aux load error: " + error);

    if (! trackLoaded || ! auxLoaded)
        return ctx.verdict();

    // Different block counts on the two instances so their states diverge.
    std::array<float, ScenarioContext::kBlockSize> left {};
    std::array<float, ScenarioContext::kBlockSize> right {};
    trackStrip.getNativeClapSlot().processStereo (
        left.data(), right.data(), left.data(), right.data(), ScenarioContext::kBlockSize);
    for (int i = 0; i < 3; ++i)
        auxStrip.getNativeClapSlot (kAuxSlotIndex).processStereo (
            left.data(), right.data(), left.data(), right.data(), ScenarioContext::kBlockSize);

    engine.publishPluginStateForSave (true);
    const auto trackPath = session.track (kTrackIndex).nativeClapPath;
    const auto trackId = session.track (kTrackIndex).nativeClapPluginId;
    const auto trackState = session.track (kTrackIndex).nativeClapStateBase64;
    const auto auxPath = session.auxLane (kAuxLaneIndex).nativeClapPath[(std::size_t) kAuxSlotIndex];
    const auto auxId = session.auxLane (kAuxLaneIndex).nativeClapPluginId[(std::size_t) kAuxSlotIndex];
    const auto auxState = session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex];

    ctx.expect (trackPath.toStdString() == fixture.u8string(), "track path was not published");
    ctx.expect (auxPath.toStdString() == fixture.u8string(), "aux path was not published");
    ctx.expect (trackId == kPluginId, "track plugin ID was not published");
    ctx.expect (auxId == kPluginId, "aux plugin ID was not published");
    ctx.expect (trackState.isNotEmpty(), "track state was empty");
    ctx.expect (auxState.isNotEmpty(), "aux state was empty");
    ctx.expect (trackState != auxState, "track and aux state did not diverge");

    const auto& sessionDir = ctx.tempDir();
    if (sessionDir.empty())
        return ScenarioResult::fail ("could not create the temporary session directory");
    const auto sessionFile = sessionDir / "session.json";
    ctx.expect (SessionSerializer::save (session, sessionFile), "session JSON save failed");
    ctx.expect (SessionSerializer::load (session, sessionFile), "session JSON load failed");

    ctx.expect (session.track (kTrackIndex).nativeClapStateBase64 == trackState,
            "track state changed in session JSON");
    ctx.expect (session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex] == auxState,
            "aux state changed in session JSON");

    engine.consumePluginStateAfterLoad();
    ctx.expect (engine.getLastPluginLoadFailures().empty(), "restore reported a plugin load failure");
    engine.publishPluginStateForSave (true);
    ctx.expect (session.track (kTrackIndex).nativeClapStateBase64 == trackState,
            "track state changed after restore");
    ctx.expect (session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex] == auxState,
            "aux state changed after restore");

    // A plug-in that rejects a saved blob must not remain audible at defaults
    // while the save path silently preserves the old blob. Both the prepared
    // engine slots and the pre-prepare deferred strip path get the rejecting
    // fixture.
    const std::array<std::uint8_t, 3> corruptState { 0x00, 0x7f, 0x42 };
    const char* const corruptBase64 = "AH9C";   // RFC 4648: { 0x00, 0x7f, 0x42 }
    session.track (kTrackIndex).nativeClapStateBase64 = corruptBase64;
    session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex] = corruptBase64;

    engine.consumePluginStateAfterLoad();
    ctx.expect (! trackStrip.isNativeClapLoaded(),
            "prepared track stayed online after rejecting saved state");
    ctx.expect (! auxStrip.isNativeClapLoaded (kAuxSlotIndex),
            "prepared aux stayed online after rejecting saved state");
    ctx.expect (trackStrip.nativeClapReloadFailed(),
            "prepared track did not preserve its failed-restore reference");
    ctx.expect (auxStrip.nativeClapReloadFailed (kAuxSlotIndex),
            "prepared aux did not preserve its failed-restore reference");
    const auto& preparedFailures = engine.getLastPluginLoadFailures();
    if (ctx.expect (preparedFailures.size() == 2,
                "prepared rejection did not report both track and aux failures"))
    {
        ctx.expect (preparedFailures[0].format == "CLAP"
                    && preparedFailures[0].reason.find ("3 bytes") != std::string::npos
                    && preparedFailures[0].reason.find ("offline") != std::string::npos,
                "prepared track rejection omitted format or reason");
        ctx.expect (preparedFailures[1].format == "CLAP"
                    && preparedFailures[1].reason.find ("3 bytes") != std::string::npos
                    && preparedFailures[1].reason.find ("offline") != std::string::npos,
                "prepared aux rejection omitted format or reason");
    }
    engine.publishPluginStateForSave (true);
    ctx.expect (session.track (kTrackIndex).nativeClapStateBase64 == corruptBase64,
            "prepared track rejection discarded the saved state");
    ctx.expect (session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex] == corruptBase64,
            "prepared aux rejection discarded the saved state");

    const char* const unreadableStateText = "*** not base64 ***";
    session.track (kTrackIndex).nativeClapStateBase64 = unreadableStateText;
    session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex] = unreadableStateText;

    engine.consumePluginStateAfterLoad();
    ctx.expect (! trackStrip.isNativeClapLoaded(),
            "track with unreadable state came online at defaults");
    ctx.expect (! auxStrip.isNativeClapLoaded (kAuxSlotIndex),
            "aux with unreadable state came online at defaults");
    ctx.expect (trackStrip.nativeClapReloadFailed(),
            "track with unreadable state lost its failed-restore reference");
    ctx.expect (auxStrip.nativeClapReloadFailed (kAuxSlotIndex),
            "aux with unreadable state lost its failed-restore reference");
    const auto& unreadableFailures = engine.getLastPluginLoadFailures();
    if (ctx.expect (unreadableFailures.size() == 2,
                "unreadable state did not report both track and aux failures"))
    {
        ctx.expect (unreadableFailures[0].reason.find ("18 bytes") != std::string::npos
                    && unreadableFailures[0].reason.find ("unreadable") != std::string::npos,
                "unreadable track state omitted its encoded size or reason");
        ctx.expect (unreadableFailures[1].reason.find ("18 bytes") != std::string::npos
                    && unreadableFailures[1].reason.find ("unreadable") != std::string::npos,
                "unreadable aux state omitted its encoded size or reason");
    }
    engine.publishPluginStateForSave (true);
    ctx.expect (session.track (kTrackIndex).nativeClapStateBase64 == unreadableStateText,
            "unreadable track state did not survive publish");
    ctx.expect (session.auxLane (kAuxLaneIndex).nativeClapStateBase64[(std::size_t) kAuxSlotIndex] == unreadableStateText,
            "unreadable aux state did not survive publish");

    const std::vector<std::uint8_t> corruptBlob (corruptState.begin(), corruptState.end());

    // setPendingNativeClap takes JUCE's File; the published path string converts
    // to one implicitly, so this file needs no JUCE header of its own.
    ChannelStrip deferredTrack;
    deferredTrack.setPendingNativeClap (trackPath, corruptBlob, kPluginId);
    deferredTrack.prepare (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    ctx.expect (! deferredTrack.isNativeClapLoaded(),
            "deferred track stayed online after rejecting saved state");
    ctx.expect (deferredTrack.nativeClapReloadFailed(),
            "deferred track did not preserve its failed-restore reference");
    const auto deferredTrackFailures = deferredTrack.takeNativeRestoreFailures();
    ctx.expect (deferredTrackFailures.size() == 1
                && deferredTrackFailures[0].format == "CLAP"
                && deferredTrackFailures[0].reason.find ("3 bytes") != std::string::npos,
            "deferred track rejection omitted format or reason");

    AuxLaneStrip deferredAux;
    deferredAux.setPendingNativeClap (kAuxSlotIndex, auxPath, corruptBlob, kPluginId);
    deferredAux.prepare (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    ctx.expect (! deferredAux.isNativeClapLoaded (kAuxSlotIndex),
            "deferred aux stayed online after rejecting saved state");
    ctx.expect (deferredAux.nativeClapReloadFailed (kAuxSlotIndex),
            "deferred aux did not preserve its failed-restore reference");
    const auto deferredAuxFailures = deferredAux.takeNativeRestoreFailures();
    ctx.expect (deferredAuxFailures.size() == 1
                && deferredAuxFailures[0].slotIndex == kAuxSlotIndex
                && deferredAuxFailures[0].format == "CLAP"
                && deferredAuxFailures[0].reason.find ("3 bytes") != std::string::npos,
            "deferred aux rejection omitted slot, format, or reason");

    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "clap.state_roundtrip",
    { "clap", "state", "plugin" },
    Needs::Engine,
    { "multi_bus.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return runRoundTrip (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native CLAP host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
