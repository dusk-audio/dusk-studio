#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../Transport.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/Json.h"
#include "../../../session/AutomationRecorder.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

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

nlohmann::json savedTrack (const Session& session, int index)
{
    const auto root = nlohmann::json::parse (SessionSerializer::serialize (session).toStdString(),
                                             nullptr, false);
    const auto& tracks = dusk::json::array (root, "tracks");
    return index < (int) tracks.size() ? tracks[(size_t) index] : nlohmann::json {};
}

// Sorts every leaf path of a against b into the ones that agree and the ones
// that do not. Objects and same-length arrays are walked, so one missed key or
// element is named rather than hidden in its parent.
void compareLeaves (const nlohmann::json& a, const nlohmann::json& b, const std::string& path,
                    std::vector<std::string>& same, std::vector<std::string>& different)
{
    static const nlohmann::json absent;
    if (a.is_object() && b.is_object())
    {
        std::set<std::string> keys;
        for (auto it = a.begin(); it != a.end(); ++it) keys.insert (it.key());
        for (auto it = b.begin(); it != b.end(); ++it) keys.insert (it.key());
        for (const auto& key : keys)
        {
            const auto ia = a.find (key);
            const auto ib = b.find (key);
            compareLeaves (ia != a.end() ? *ia : absent, ib != b.end() ? *ib : absent,
                           path + "/" + key, same, different);
        }
        return;
    }
    if (a.is_array() && b.is_array() && a.size() == b.size() && ! a.empty())
    {
        for (size_t i = 0; i < a.size(); ++i)
            compareLeaves (a[i], b[i], path + "[" + std::to_string (i) + "]", same, different);
        return;
    }
    (a == b ? same : different).push_back (path);
}

std::string joined (const std::vector<std::string>& paths)
{
    std::string out;
    for (const auto& p : paths) out += (out.empty() ? "" : ", ") + p;
    return out;
}

// A clone carries everything a save writes for its source, bar the name it
// tags, and the undo puts the destination's own back. The source is moved off
// the destination's value on every key a save writes before cloning, and the
// case fails while any key is left unmoved, so a key the save learns later
// fails here until it is set below - and then again if the clone drops it.
ScenarioResult runCloneSavedFields (ScenarioContext& ctx)
{
    constexpr int kSource = 8;
    constexpr int kDest   = 9;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& src = session.track (kSource);
    auto& undo = engine.getUndoManager();
    ctx.cleanup ([&undo] { undo.clearUndoHistory(); });

    const auto set = [&ctx] (auto& atom, auto value)
    {
        ctx.keep (atom);
        atom.store (value);
    };

    const auto colourWas = src.colour;
    const auto midiInWas = src.midiInputIdentifier;
    const auto midiOutWas = src.midiOutputIdentifier;
    const auto routingWas = src.hardwareInsert.routing.current();
    auto& srcStrip = engine.getChannelStrip (kSource);
    auto& dstStrip = engine.getChannelStrip (kDest);
    ctx.keep (srcStrip.insertMode);
    ctx.cleanup ([&src, colourWas, midiInWas, midiOutWas, routingWas]
    {
        src.colour = colourWas;
        src.midiInputIdentifier = midiInWas;
        src.midiOutputIdentifier = midiOutWas;
        src.hardwareInsert.routing.publish (std::make_unique<HardwareInsertRouting> (routingWas));
    });

    auto& dstTrack = session.track (kDest);
    src.name = "Source";
    src.colour = dstTrack.colour.contrasting();

    auto& st = src.strip;
    set (st.faderDb, -7.25f);
    set (st.pan, 0.35f);
    set (st.mute, true);
    set (st.solo, true);
    set (st.phaseInvert, true);
    set (st.insertBypassed, true);
    set (st.faderGroupId, 3);
    set (src.inputMonitor, true);
    set (src.printEffects, true);
    set (src.inputSource, 5);
    set (src.inputSourceR, 6);
    set (src.midiInputIndex, 2);
    src.midiInputIdentifier = "scenario-midi-in";
    // Past the end of any device list, so the clone's eager open is a no-op.
    set (src.midiOutputIndex, 200);
    src.midiOutputIdentifier = "scenario-midi-out";
    set (src.midiChannel, 9);
    set (src.mode, (int) Track::Mode::Stereo);
    for (auto& bus : st.busAssign) set (bus, true);
    set (st.auxSendsBypassed, true);
    for (size_t i = 0; i < st.auxSendDb.size(); ++i)
    {
        set (st.auxSendDb[i], -3.0f - 3.0f * (float) i);
        set (st.auxSendPreFader[i], true);
    }

    set (st.hpfEnabled, true);
    set (st.hpfFreq, 120.0f);
    set (st.lpfEnabled, true);
    set (st.lpfFreq, 9000.0f);
    set (st.eqEnabled, true);
    set (st.eqBlackMode, true);
    set (st.lfGainDb, 3.0f);
    set (st.lfFreq, 150.0f);
    set (st.lmGainDb, -2.0f);
    set (st.lmFreq, 800.0f);
    set (st.lmQ, 1.5f);
    set (st.hmGainDb, 4.0f);
    set (st.hmFreq, 3000.0f);
    set (st.hmQ, 2.0f);
    set (st.hfGainDb, -5.0f);
    set (st.hfFreq, 10000.0f);

    set (st.compEnabled, true);
    set (st.compModePicked, true);
    set (st.compMode, 1);
    set (st.compOptoPeakRed, 44.0f);
    set (st.compOptoGain, 61.0f);
    set (st.compOptoLimit, true);
    set (st.compFetInput, 12.0f);
    set (st.compFetOutput, 3.0f);
    set (st.compFetAttack, 0.5f);
    set (st.compFetRelease, 600.0f);
    set (st.compFetRatio, 2);
    set (st.compFetThresholdDb, -20.0f);
    set (st.compVcaThreshDb, -15.0f);
    set (st.compVcaRatio, 6.0f);
    set (st.compVcaAttack, 3.0f);
    set (st.compVcaRelease, 250.0f);
    set (st.compVcaOutput, 2.0f);
    set (st.compVcaOverEasy, true);
    set (st.compVcaDetectorClassic, true);

    auto& hw = src.hardwareInsert;
    set (hw.enabled, true);
    {
        HardwareInsertRouting routing;
        routing.outputChL = 2;
        routing.outputChR = 3;
        routing.inputChL = 4;
        routing.inputChR = 5;
        routing.latencySamples = 123;
        routing.format = 1;
        hw.routing.publish (std::make_unique<HardwareInsertRouting> (routing));
    }
    set (hw.outputGainDb, -2.0f);
    set (hw.inputGainDb, 1.5f);
    set (hw.dryWet, 0.6f);
    srcStrip.insertMode.store (ChannelStrip::kInsertHardware);

    src.regions.push_back ({});
    src.regions.back().timelineStart = 4800;
    src.regions.back().lengthInSamples = 9600;
    src.regions.back().previousTakes.push_back ({});
    src.regions.back().previousTakes.back().lengthInSamples = 9600;
    {
        MidiRegion region;
        region.lengthInSamples = 24000;
        region.lengthInTicks = 960;
        region.notes.push_back ({ 1, 64, 90, 0, 480 });
        region.previousTakes.push_back ({});
        region.previousTakes.back().lengthInTicks = 960;
        region.previousTakes.back().notes.push_back ({ 1, 67, 80, 0, 240 });
        src.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
    }
    for (int p = 0; p < kNumAutomationParams; ++p)
        src.automationLanes[(size_t) p].publishPoints ({ { 0, 0.25f, 120.0f },
                                                         { 48000 + p, 0.75f, 120.0f } });
    set (src.automationMode, (int) AutomationMode::Read);
    session.recomputeRtCounters();

    const auto sourceSaved = savedTrack (session, kSource);
    const auto destSaved = savedTrack (session, kDest);
    const int destInsertMode = dstStrip.insertMode.load();
    {
        std::vector<std::string> same, different;
        compareLeaves (sourceSaved, destSaved, "", same, different);
        same.erase (std::remove (same.begin(), same.end(), "/frozen"), same.end());
        if (! ctx.expect (same.empty(), "the source still matches the destination on "
                                            + joined (same) + "; set it above"))
            return ctx.verdict();
    }

    // A pass keeps its ride on the side until it closes, so a clone taken during
    // one would miss the source's ride or have the destination's dropped.
    const auto expectRefused = [&] (const std::string& when)
    {
        ctx.expect (CloneTrackAction::refusalFor (session, engine, kSource, kDest)
                        == CloneTrackAction::Refusal::Playing,
                    when + " did not report playback as the reason");
        undo.clearUndoHistory();
        undo.beginNewTransaction();
        ctx.expect (! undo.perform (new CloneTrackAction (session, engine, kSource, kDest)),
                    "a clone ran " + when);
        ctx.expect (savedTrack (session, kDest) == destSaved, "a clone refused " + when
                                                              + " still changed the destination");
        ctx.expect (dstStrip.insertMode.load() == destInsertMode,
                    "a clone refused " + when + " still changed the destination's insert mode");
    };
    for (auto* passTrack : { &src, &dstTrack })
    {
        auto& lane = passTrack->automationLanes[(size_t) AutomationParam::Pan];
        const auto laneWas = lane.pointsConst();
        AutomationPassRecorder pass (AutomationParam::Pan);
        pass.record (lane, 1000, 0.5f, 120.0f, 0, 0);
        expectRefused (passTrack == &src ? "during a pass on the source" : "during a pass on the destination");
        pass.finish (lane);
        lane.publishPoints (laneWas);
    }
    auto& transport = engine.getTransport();
    transport.setState (Transport::State::Playing);
    expectRefused ("while playing");
    transport.setState (Transport::State::Stopped);
    transport.setPlayhead (0);

    undo.clearUndoHistory();
    undo.beginNewTransaction();
    if (! ctx.expect (CloneTrackAction::refusalFor (session, engine, kSource, kDest)
                          == CloneTrackAction::Refusal::None
                      && undo.perform (new CloneTrackAction (session, engine, kSource, kDest)),
                      "the clone action refused to run"))
        return ctx.verdict();

    const auto expectCopied = [&] (const std::string& when)
    {
        const auto cloneSaved = savedTrack (session, kDest);
        std::vector<std::string> same, different;
        compareLeaves (sourceSaved, cloneSaved, "", same, different);
        different.erase (std::remove (different.begin(), different.end(), "/name"), different.end());
        ctx.expect (different.empty(), when + " left " + joined (different)
                                           + " on the destination unlike the source");
        ctx.expect (dstTrack.name == "Source (copy)", when + " did not tag the clone's name");
        ctx.expect (dstStrip.insertMode.load() == ChannelStrip::kInsertHardware,
                    when + " did not carry the source's insert mode");
    };
    expectCopied ("the clone");
    ctx.expect (savedTrack (session, kSource) == sourceSaved, "the clone changed the source");
    dstTrack.automationLanes[0].publishPoints ({});
    ctx.expect (src.automationLanes[0].pointsConst().size() == 2,
                "clearing the clone's lane cleared the source's too");

    ctx.expect (undo.undo(), "undo refused");
    {
        std::vector<std::string> same, different;
        compareLeaves (destSaved, savedTrack (session, kDest), "", same, different);
        ctx.expect (different.empty(), "undo did not put back the destination's " + joined (different));
        ctx.expect (dstStrip.insertMode.load() == destInsertMode,
                    "undo did not put back the destination's insert mode");
    }

    ctx.expect (undo.redo(), "redo refused");
    expectCopied ("redo");
    ctx.expect (undo.undo(), "the closing undo refused");

    for (auto* frozen : { &src, &dstTrack })
    {
        frozen->frozen.store (true);
        CloneTrackAction refused (session, engine, kSource, kDest);
        ctx.expect (CloneTrackAction::refusalFor (session, engine, kSource, kDest)
                        == CloneTrackAction::Refusal::Frozen,
                    "a frozen track was not reported as the reason");
        ctx.expect (! refused.perform(), "a clone ran with a frozen track on one side");
        frozen->frozen.store (false);
    }
    ctx.expect (savedTrack (session, kDest) == destSaved, "a refused clone changed the destination");
    return ctx.verdict();
}

const ScenarioRegistrar savedFields { Scenario {
    "session.clone_track_carries_every_saved_field",
    { "session", "clone" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runCloneSavedFields (ctx); }
} };

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
