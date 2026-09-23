#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 || DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kSourceTrack = 2;
constexpr int kCloneTrack  = 5;

// One format's leg of the check. The three session strings a native insert
// persists are passed as pointers-to-member so the CLAP / LV2 / VST3 legs share
// this body; `load` puts the plugin on a track and moves it off its defaults.
template <typename StringMember, typename LoadFn, typename LoadedFn>
void runFormat (ScenarioContext& ctx, const char* format,
                StringMember pathMember, StringMember idMember, StringMember stateMember,
                LoadFn&& load, LoadedFn&& isLoaded)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const std::string prefix = std::string (format) + ": ";

    if (! ctx.expect (load (kSourceTrack), prefix + "could not load the fixture on the source track"))
        return;

    engine.publishPluginStateForSave (true);
    const auto sourceId    = session.track (kSourceTrack).*idMember;
    const auto sourceState = session.track (kSourceTrack).*stateMember;
    if (! ctx.expect (sourceState.isNotEmpty(), prefix + "the source published no state"))
        return;

    auto& undo = engine.getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new CloneTrackAction (session, engine,
                                                          kSourceTrack, kCloneTrack)),
                      prefix + "the clone action refused to run"))
        return;

    ctx.expect (isLoaded (kCloneTrack), prefix + "the clone left the destination slot empty");
    ctx.expect (session.track (kCloneTrack).*idMember == sourceId,
                prefix + "the clone loaded a different plugin");
    ctx.expect ((session.track (kCloneTrack).*stateMember).isNotEmpty(),
                prefix + "the clone persisted no state");

    // A save straight after the clone has to see the same bytes on both tracks:
    // the destination's live plugin must hold the source's settings, not just
    // the session string the action wrote. Both are published through the same
    // path here - the state a slot serialises depends on whether it has a
    // file-state directory, so a blob captured by the action and one captured by
    // the save path are not comparable byte for byte.
    engine.publishPluginStateForSave (true);
    ctx.expect (session.track (kCloneTrack).*stateMember == session.track (kSourceTrack).*stateMember,
                prefix + "the cloned plugin published different state from the source");
    ctx.expect (session.track (kSourceTrack).*stateMember == sourceState,
                prefix + "the source's own state changed across the clone");

    ctx.expect (undo.undo(), prefix + "undo refused");
    ctx.expect (! isLoaded (kCloneTrack), prefix + "undo left the clone loaded");
    ctx.expect ((session.track (kCloneTrack).*pathMember).isEmpty(),
                prefix + "undo left the clone's persisted path behind");
    ctx.expect ((session.track (kCloneTrack).*stateMember).isEmpty(),
                prefix + "undo left the clone's persisted state behind");
    ctx.expect (isLoaded (kSourceTrack), prefix + "undo also unloaded the source");

    ctx.expect (undo.redo(), prefix + "redo refused");
    ctx.expect (isLoaded (kCloneTrack), prefix + "redo left the destination slot empty");
    engine.publishPluginStateForSave (true);
    ctx.expect (session.track (kCloneTrack).*stateMember == session.track (kSourceTrack).*stateMember,
                prefix + "redo restored different state");

    undo.clearUndoHistory();
}

void clearTracks (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    for (const int t : { kSourceTrack, kCloneTrack })
    {
        auto& strip = engine.getChannelStrip (t);
        strip.unloadNativeClap();
        strip.unloadNativeLv2();
        strip.unloadNativeVst3();
        auto& track = session.track (t);
        track.nativeClapPath.clear();
        track.nativeClapPluginId.clear();
        track.nativeClapStateBase64.clear();
        track.nativeLv2Path.clear();
        track.nativeLv2PluginId.clear();
        track.nativeLv2StateBase64.clear();
        track.nativeVst3Path.clear();
        track.nativeVst3PluginId.clear();
        track.nativeVst3StateBase64.clear();
    }
}

ScenarioResult runClone (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    int formatsRun = 0;

   #if DUSKSTUDIO_HAS_NATIVE_CLAP
    if (const auto fixture = ctx.fixture ("multi_bus.clap"))
    {
        ++formatsRun;
        runFormat (ctx, "CLAP",
                   &Track::nativeClapPath, &Track::nativeClapPluginId,
                   &Track::nativeClapStateBase64,
                   [&] (int track)
                   {
                       auto& slot = engine.getChannelStrip (track).getNativeClapSlot();
                       std::string error;
                       if (! slot.load (*fixture, ScenarioContext::kSampleRate,
                                        ScenarioContext::kBlockSize, error,
                                        "studio.dusk.test.multi-bus"))
                       {
                           ctx.note ("CLAP load error: " + error);
                           return false;
                       }
                       // The fixture's state is its processed-block count.
                       std::array<float, ScenarioContext::kBlockSize> left {};
                       std::array<float, ScenarioContext::kBlockSize> right {};
                       for (int i = 0; i < 3; ++i)
                           slot.processStereo (left.data(), right.data(), left.data(),
                                               right.data(), ScenarioContext::kBlockSize);
                       return true;
                   },
                   [&] (int track) { return engine.getChannelStrip (track).isNativeClapLoaded(); });
        clearTracks (ctx);
    }
    else
    {
        ctx.note ("CLAP leg skipped: multi_bus.clap did not resolve");
    }
   #endif

   #if DUSKSTUDIO_HAS_NATIVE_LV2
    if (const auto fixture = ctx.fixture ("file_state.lv2"))
    {
        ++formatsRun;
        runFormat (ctx, "LV2",
                   &Track::nativeLv2Path, &Track::nativeLv2PluginId,
                   &Track::nativeLv2StateBase64,
                   [&] (int track)
                   {
                       auto& slot = engine.getChannelStrip (track).getNativeLv2Slot();
                       std::string error;
                       if (! slot.load (*fixture, ScenarioContext::kSampleRate,
                                        ScenarioContext::kBlockSize, error,
                                        "urn:duskstudio:test:control-state"))
                       {
                           ctx.note ("LV2 load error: " + error);
                           return false;
                       }
                       for (int i = 0; i < slot.paramCount(); ++i)
                           if (const auto* info = slot.paramInfo (i);
                               info != nullptr && info->name == "Gain")
                               slot.setParamValue (info->id, 0.75);
                       return true;
                   },
                   [&] (int track) { return engine.getChannelStrip (track).isNativeLv2Loaded(); });
        clearTracks (ctx);
    }
    else
    {
        ctx.note ("LV2 leg skipped: file_state.lv2 did not resolve");
    }
   #endif

   #if DUSKSTUDIO_HAS_NATIVE_VST3
    if (const auto fixture = ctx.fixture ("relayout.vst3"))
    {
        ++formatsRun;
        runFormat (ctx, "VST3",
                   &Track::nativeVst3Path, &Track::nativeVst3PluginId,
                   &Track::nativeVst3StateBase64,
                   [&] (int track)
                   {
                       auto& slot = engine.getChannelStrip (track).getNativeVst3Slot();
                       std::string error;
                       if (! slot.load (*fixture, ScenarioContext::kSampleRate,
                                        ScenarioContext::kBlockSize, error))
                       {
                           ctx.note ("VST3 load error: " + error);
                           return false;
                       }
                       for (int i = 0; i < slot.paramCount(); ++i)
                           if (const auto* info = slot.paramInfo (i);
                               info != nullptr && info->name == "Latency Mode")
                               slot.setParamValue (info->id, 1.0);
                       return true;
                   },
                   [&] (int track) { return engine.getChannelStrip (track).isNativeVst3Loaded(); });
        clearTracks (ctx);
    }
    else
    {
        ctx.note ("VST3 leg skipped: relayout.vst3 did not resolve");
    }
   #endif

    const auto verdict = ctx.verdict();
    if (verdict.status == ScenarioStatus::Fail)
        return verdict;
    if (formatsRun == 0)
        return ScenarioResult::skip ("no native plugin fixture resolved");
    ctx.note ("formats covered: " + std::to_string (formatsRun));
    return ScenarioResult::pass();
}
#endif

// A converted older session's band plays its format-7 dial while its frequency
// and voicing are the ones it was converted under. Clone Track copies the dial
// with both, so the copy sounds like its source, and the undo puts back the
// destination's own.
ScenarioResult runCloneEqDials (ScenarioContext& ctx)
{
    using EqFreq = ChannelStripParams::EqFreq;
    constexpr int kSource = 3;
    constexpr int kDest   = 4;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& src = session.track (kSource).strip;
    auto& dst = session.track (kDest).strip;

    std::array<std::uint64_t, ChannelStripParams::kNumEqFreqs> srcWords {};
    for (size_t i = 0; i < srcWords.size(); ++i) srcWords[i] = src.eqFreqDial[i].raw();
    ctx.cleanup ([&src, srcWords]
    {
        for (size_t i = 0; i < srcWords.size(); ++i) src.eqFreqDial[i].setRaw (srcWords[i]);
    });
    for (const auto f : { EqFreq::Hpf, EqFreq::Lpf, EqFreq::Lf, EqFreq::Lm })
        ctx.keep (src.eqFreq (f));
    ctx.keep (src.eqBlackMode);
    ctx.keep (src.lpfEnabled);
    auto& undo = engine.getUndoManager();
    ctx.cleanup ([&undo] { undo.clearUndoHistory(); });

    const auto dialOf = [] (const ChannelStripParams& strip, EqFreq f)
    {
        float hz = 0.0f;
        return strip.legacyDial (f).dialFor (strip.eqFreq (f), strip.eqBlackMode.load(), hz);
    };
    const auto hold = [] (ChannelStripParams& strip, EqFreq f, float hz, float dial)
    {
        strip.eqFreq (f).store (hz);
        strip.legacyDial (f).set (dial, hz, strip.eqBlackMode.load());
    };
    src.eqBlackMode.store (true);
    src.lpfEnabled.store (true);
    hold (src, EqFreq::Lf, 660.0f, 400.0f);
    hold (src, EqFreq::Hpf, 316.0f, 300.0f);
    hold (src, EqFreq::Lpf, 9000.0f, 12000.0f);
    src.legacyDial (EqFreq::Lm).clear();

    std::array<std::uint64_t, ChannelStripParams::kNumEqFreqs> dstBefore {};
    for (size_t i = 0; i < dstBefore.size(); ++i) dstBefore[i] = dst.eqFreqDial[i].raw();
    const bool dstBlack = dst.eqBlackMode.load();
    const float dstLpf = dst.lpfFreq.load();

    undo.clearUndoHistory();
    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new CloneTrackAction (session, engine, kSource, kDest)),
                      "the clone action refused to run"))
        return ctx.verdict();
    const auto copied = [&]
    {
        bool same = dst.eqBlackMode.load() && std::abs (dst.lpfFreq.load() - 9000.0f) < 1.0e-3f
                    && dst.lpfEnabled.load();
        for (size_t i = 0; i < srcWords.size(); ++i)
            same = same && dst.eqFreqDial[i].raw() == src.eqFreqDial[i].raw();
        return same && dialOf (dst, EqFreq::Lf) > 0.0f && dialOf (dst, EqFreq::Hpf) > 0.0f
                    && dialOf (dst, EqFreq::Lpf) > 0.0f && dialOf (dst, EqFreq::Lm) <= 0.0f;
    };
    ctx.expect (copied(), "the clone did not carry the source's dials, voicing and LPF");

    ctx.expect (undo.undo(), "undo refused");
    bool restored = dst.eqBlackMode.load() == dstBlack && std::abs (dst.lpfFreq.load() - dstLpf) < 1.0e-3f;
    for (size_t i = 0; i < dstBefore.size(); ++i)
        restored = restored && dst.eqFreqDial[i].raw() == dstBefore[i];
    ctx.expect (restored, "undo did not put back the destination's own dials, voicing and LPF");

    ctx.expect (undo.redo(), "redo refused");
    ctx.expect (copied(), "redo did not carry the source's dials again");
    ctx.expect (undo.undo(), "the closing undo refused");
    return ctx.verdict();
}

const ScenarioRegistrar eqDials { Scenario {
    "session.clone_track_keeps_converted_eq_dials",
    { "session", "clone", "eq" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runCloneEqDials (ctx); }
} };

const ScenarioRegistrar registrar { Scenario {
    "session.clone_track_keeps_native_state",
    { "session", "clone", "plugin", "state" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP || DUSKSTUDIO_HAS_NATIVE_LV2 || DUSKSTUDIO_HAS_NATIVE_VST3
        return runClone (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without any native plugin host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
