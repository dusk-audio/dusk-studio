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

ScenarioResult runCheapNativeIdentity (ScenarioContext& ctx)
{
    const auto fixture = *ctx.fixture ("multi_bus.clap");
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (kTrackIndex);
    auto& lane = ctx.session().auxLane (kAuxLaneIndex);
    auto& trackStrip = engine.getChannelStrip (kTrackIndex);
    auto& auxStrip = engine.getAuxLaneStrip (kAuxLaneIndex);
    ctx.cleanup ([&trackStrip, &auxStrip]
    {
        trackStrip.unloadNativeClap();
        auxStrip.unloadNativeClap (kAuxSlotIndex);
    });
    const auto priorState = engine.getTransport().getState();
    ctx.cleanup ([&engine, priorState] { engine.getTransport().setState (priorState); });
    std::string error;
    if (! ctx.expect (trackStrip.getNativeClapSlot().load (fixture, ScenarioContext::kSampleRate,
                         ScenarioContext::kBlockSize, error, kPluginId), "could not load track CLAP: " + error)
        || ! ctx.expect (auxStrip.getNativeClapSlot (kAuxSlotIndex).load (fixture, ScenarioContext::kSampleRate,
                         ScenarioContext::kBlockSize, error, kPluginId), "could not load aux CLAP: " + error))
        return ctx.verdict();
    const auto saveCount = [] (auto& slot)
    {
        const auto* plugin = slot.getInstance()->getPlugin();
        return static_cast<const std::uint32_t*> (plugin->get_extension (plugin, "studio.dusk.test.state-save-count"));
    };
    const auto* trackSaves = saveCount (trackStrip.getNativeClapSlot());
    const auto* auxSaves = saveCount (auxStrip.getNativeClapSlot (kAuxSlotIndex));
    if (! ctx.expect (trackSaves != nullptr && auxSaves != nullptr, "the CLAP save counter extension is missing"))
        return ctx.verdict();
    track.nativeClapPath = "old.clap";
    track.nativeClapPluginId = "old-plugin";
    track.nativeClapStateBase64 = "old-track-state";
    lane.nativeClapPath[kAuxSlotIndex] = "old.clap";
    lane.nativeClapPluginId[kAuxSlotIndex] = "old-plugin";
    lane.nativeClapStateBase64[kAuxSlotIndex] = "old-aux-state";
    engine.getTransport().setState (Transport::State::Playing);
    ctx.expect (engine.publishPluginStateForSave (false), "capture-free publication failed");
    ctx.expect (track.nativeClapPath.toStdString() == fixture.u8string() && track.nativeClapPluginId == kPluginId,
                "capture-free publication missed the track's native identity");
    ctx.expect (lane.nativeClapPath[kAuxSlotIndex].toStdString() == fixture.u8string()
                    && lane.nativeClapPluginId[kAuxSlotIndex] == kPluginId,
                "capture-free publication missed the aux slot's native identity");
    ctx.expect (track.nativeClapStateBase64.isEmpty() && lane.nativeClapStateBase64[kAuxSlotIndex].isEmpty(),
                "a replacement retained state owned by its predecessor");
    track.nativeClapStateBase64 = "cached-track-state";
    lane.nativeClapStateBase64[kAuxSlotIndex] = "cached-aux-state";
    ctx.expect (engine.publishPluginStateForSave (false), "repeated capture-free publication failed");
    ctx.expect (track.nativeClapStateBase64 == "cached-track-state"
                    && lane.nativeClapStateBase64[kAuxSlotIndex] == "cached-aux-state",
                "capture-free publication changed matching opaque state");
    ctx.expect (*trackSaves == 0 && *auxSaves == 0, "capture-free publication called CLAP state save");
    ctx.expect (engine.getTransport().isPlaying(), "capture-free publication stopped playback");
    trackStrip.unloadNativeClap();
    auxStrip.unloadNativeClap (kAuxSlotIndex);
    ctx.expect (engine.publishPluginStateForSave (false), "empty native-slot publication failed");
    ctx.expect (track.nativeClapPath.isEmpty() && track.nativeClapPluginId.isEmpty()
                    && track.nativeClapStateBase64.isEmpty(), "capture-free publication missed track removal");
    ctx.expect (lane.nativeClapPath[kAuxSlotIndex].isEmpty() && lane.nativeClapPluginId[kAuxSlotIndex].isEmpty()
                    && lane.nativeClapStateBase64[kAuxSlotIndex].isEmpty(),
                "capture-free publication missed aux slot removal");

    const char* const legacyState = "RFNLQwEAAAAAAAAA";
    const std::vector<std::uint8_t> legacyBytes { 'D', 'S', 'K', 'C', 1, 0, 0, 0, 0, 0, 0, 0 };
    const auto legacyReferences = [&]
    {
        track.nativeClapPath = fixture.u8string().c_str();
        track.nativeClapPluginId.clear();
        track.nativeClapStateBase64 = legacyState;
        lane.nativeClapPath[kAuxSlotIndex] = fixture.u8string().c_str();
        lane.nativeClapPluginId[kAuxSlotIndex].clear();
        lane.nativeClapStateBase64[kAuxSlotIndex] = legacyState;
    };
    legacyReferences();
    trackStrip.setPendingNativeClap (track.nativeClapPath, legacyBytes);
    auxStrip.setPendingNativeClap (kAuxSlotIndex, lane.nativeClapPath[kAuxSlotIndex], legacyBytes);
    ctx.expect (trackStrip.hasPendingNativeClapRestore() && auxStrip.hasPendingNativeClapRestore (kAuxSlotIndex),
                "queued restores were not visible before prepare");
    ctx.expect (engine.publishPluginStateForSave (false), "pending native restore publication failed");
    ctx.expect (track.nativeClapStateBase64 == legacyState && lane.nativeClapStateBase64[kAuxSlotIndex] == legacyState,
                "capture-free publication discarded a queued restore");
    trackStrip.unloadNativeClap();
    auxStrip.unloadNativeClap (kAuxSlotIndex);
    ctx.expect (! trackStrip.hasPendingNativeClapRestore() && ! auxStrip.hasPendingNativeClapRestore (kAuxSlotIndex),
                "explicit unload left a queued restore behind");
    ctx.expect (engine.publishPluginStateForSave (false), "removed pending restore publication failed");
    ctx.expect (track.nativeClapPath.isEmpty() && track.nativeClapStateBase64.isEmpty()
                    && lane.nativeClapPath[kAuxSlotIndex].isEmpty() && lane.nativeClapStateBase64[kAuxSlotIndex].isEmpty(),
                "a removed pending restore was retained as an offline plug-in");

    legacyReferences();
    engine.getTransport().setState (Transport::State::Stopped);
    engine.consumePluginStateAfterLoad();
    if (! ctx.expect (engine.getLastPluginLoadFailures().empty() && trackStrip.isNativeClapLoaded()
                         && auxStrip.isNativeClapLoaded (kAuxSlotIndex), "the legacy-ID fixture did not restore"))
        return ctx.verdict();
    ctx.expect (track.nativeClapPluginId == kPluginId && lane.nativeClapPluginId[kAuxSlotIndex] == kPluginId,
                "accepted restore did not publish its canonical plug-in ID");
    ctx.expect (track.nativeClapStateBase64 == legacyState && lane.nativeClapStateBase64[kAuxSlotIndex] == legacyState,
                "accepted restore changed its opaque state");
    const auto baseline = SessionSerializer::serialize (ctx.session());
    ctx.expect (engine.publishPluginStateForSave (false), "restored identity publication failed");
    ctx.expect (SessionSerializer::serialize (ctx.session()) == baseline,
                "capture-free publication dirtied a freshly restored legacy-ID session");
    ctx.expect (*saveCount (trackStrip.getNativeClapSlot()) == 0
                    && *saveCount (auxStrip.getNativeClapSlot (kAuxSlotIndex)) == 0,
                "legacy-ID normalization captured opaque state");

    trackStrip.unloadNativeClap();
    auxStrip.unloadNativeClap (kAuxSlotIndex);
    legacyReferences();
    trackStrip.setPendingNativeClap (track.nativeClapPath, legacyBytes);
    auxStrip.setPendingNativeClap (kAuxSlotIndex, lane.nativeClapPath[kAuxSlotIndex], legacyBytes);
    engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    if (! ctx.expect (trackStrip.isNativeClapLoaded() && auxStrip.isNativeClapLoaded (kAuxSlotIndex),
                      "prepare did not finish the queued native restores"))
        return ctx.verdict();
    ctx.expect (! trackStrip.hasPendingNativeClapRestore() && ! auxStrip.hasPendingNativeClapRestore (kAuxSlotIndex),
                "successful prepare left pending restore references");
    ctx.expect (track.nativeClapPluginId == kPluginId && lane.nativeClapPluginId[kAuxSlotIndex] == kPluginId,
                "prepare did not publish accepted canonical plug-in IDs");
    ctx.expect (track.nativeClapStateBase64 == legacyState && lane.nativeClapStateBase64[kAuxSlotIndex] == legacyState,
                "prepare changed accepted opaque state");
    const auto preparedBaseline = SessionSerializer::serialize (ctx.session());
    ctx.expect (engine.publishPluginStateForSave (false), "prepared identity publication failed");
    ctx.expect (SessionSerializer::serialize (ctx.session()) == preparedBaseline,
                "capture-free publication changed accepted deferred-restore state");
    ctx.expect (*saveCount (trackStrip.getNativeClapSlot()) == 0
                    && *saveCount (auxStrip.getNativeClapSlot (kAuxSlotIndex)) == 0,
                "deferred restore normalization captured opaque state");
    return ctx.verdict();
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
ScenarioResult runCheapStandardIdentity (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (4);
    auto& lane = ctx.session().auxLane (3);
    auto& trackSlot = engine.getChannelStrip (4).getPluginSlot();
    auto& auxSlot = engine.getAuxLaneStrip (3).getPluginSlot (0);
    ctx.cleanup ([&trackSlot, &auxSlot] { trackSlot.unload(); auxSlot.unload(); });
    const auto priorState = engine.getTransport().getState();
    ctx.cleanup ([&engine, priorState] { engine.getTransport().setState (priorState); });
    using SessionFile = decltype (ctx.session().getSessionDirectory());
    const SessionFile fixture (ctx.fixture ("relayout.vst3")->u8string().c_str());
    decltype (trackSlot.getLoadedName()) error;
    if (! ctx.expect (trackSlot.loadFromFile (fixture, error), "could not load standard track insert: " + error.toStdString())
        || ! ctx.expect (auxSlot.loadFromFile (fixture, error), "could not load standard aux insert: " + error.toStdString()))
        return ctx.verdict();
    const auto trackDescriptor = trackSlot.getDescriptorForSave (0);
    const auto auxDescriptor = auxSlot.getDescriptorForSave (0);
    if (! ctx.expect (trackDescriptor.has_value() && auxDescriptor.has_value(), "loaded standard inserts have no descriptors"))
        return ctx.verdict();
    track.pluginStateBase64 = "old-track-state";
    lane.pluginStateBase64[0] = "old-aux-state";
    engine.getTransport().setState (Transport::State::Playing);
    ctx.expect (engine.publishPluginStateForSave (false), "standard identity publication failed");
    ctx.expect (track.pluginDescriptor == trackDescriptor && lane.pluginDescriptor[0] == auxDescriptor,
                "capture-free publication missed loaded standard insert descriptors");
    ctx.expect (track.pluginStateBase64.isEmpty() && lane.pluginStateBase64[0].isEmpty(),
                "a new standard insert retained state with no matching owner");
    track.pluginStateBase64 = "cached-track-state";
    lane.pluginStateBase64[0] = "cached-aux-state";
    ctx.expect (engine.publishPluginStateForSave (false), "standard repeated publication failed");
    ctx.expect (track.pluginStateBase64 == "cached-track-state" && lane.pluginStateBase64[0] == "cached-aux-state",
                "capture-free publication read standard insert state");
    ctx.expect (engine.getTransport().isPlaying(), "standard identity publication stopped playback");
    trackSlot.unload();
    auxSlot.unload();
    ctx.expect (engine.publishPluginStateForSave (false), "empty standard insert publication failed");
    ctx.expect (! track.pluginDescriptor.has_value() && track.pluginLegacyDescriptionXml.isEmpty()
                    && track.pluginStateBase64.isEmpty(), "capture-free publication missed standard track removal");
    ctx.expect (! lane.pluginDescriptor[0].has_value() && lane.pluginLegacyDescriptionXml[0].isEmpty()
                    && lane.pluginStateBase64[0].isEmpty(), "capture-free publication missed standard aux removal");
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
const ScenarioRegistrar cheapNativeIdentityRegistrar { Scenario {
    "plugin.native_identity_without_capture", { "plugin", "state", "autosave" }, Needs::Engine, { "multi_bus.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return runCheapNativeIdentity (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native CLAP host");
       #endif
    }
} };
const ScenarioRegistrar cheapStandardIdentityRegistrar { Scenario {
    "plugin.standard_identity_without_capture", { "plugin", "state", "autosave" }, Needs::Engine, { "relayout.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runCheapStandardIdentity (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the VST3 fixture");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
