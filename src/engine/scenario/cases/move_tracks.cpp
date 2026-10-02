#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../ScenarioWorld.h"
#include "../../AudioEngine.h"
#include "../../Transport.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/Json.h"
#include "../../../session/AutomationRecorder.h"
#include "../../../session/ParamEditAction.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"
#include "../../../session/TrackMove.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr int kN = Session::kNumTracks;
constexpr int kMoved = 17;   // track 18, dragged to the top
constexpr int kFrames = ScenarioContext::kBlockSize;
constexpr int kInputs = kN;
constexpr double kTwoPi = 6.283185307179586;

// The engine's file type, named through Session's own getter so this file stays
// free of the framework header.
using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

using StripOrder = std::array<const ChannelStrip*, kN>;

StripOrder stripOrder (AudioEngine& engine)
{
    StripOrder order {};
    for (int t = 0; t < kN; ++t) order[(size_t) t] = &engine.getChannelStrip (t);
    return order;
}

// The order a plan should leave the strips in.
StripOrder movedOrder (const StripOrder& was, const TrackMovePlan& plan)
{
    StripOrder order {};
    for (int t = 0; t < kN; ++t) order[(size_t) t] = was[(size_t) plan.newToOld[(size_t) t]];
    return order;
}

// Registered last, so it runs first: the strips, and the tracks with them, go
// back to the order the case found them in before any per-slot restore runs,
// and the case's history and hooks go with them.
void restoreOrderAtEnd (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    const auto original = stripOrder (engine);
    ctx.cleanup ([&engine, original]
    {
        engine.onBeforeTracksMove = nullptr;
        engine.onTracksMoved = nullptr;
        engine.getTransport().setState (Transport::State::Stopped);
        // Slot t gets back the strip it started with, from wherever that is now.
        std::array<int, kN> newToOld {};
        const auto now = stripOrder (engine);
        for (int t = 0; t < kN; ++t)
            newToOld[(size_t) t] = (int) (std::find (now.begin(), now.end(), original[(size_t) t])
                                          - now.begin());
        if (const auto back = trackMoveFromNewToOld (newToOld))
            engine.moveTracks (*back);
        engine.getUndoManager().clearUndoHistory();
    });
}

nlohmann::json saved (const Session& session)
{
    return nlohmann::json::parse (SessionSerializer::serialize (session).toStdString(), nullptr, false);
}

// A saved track with what a move rewrites on purpose - a followed input and
// the LV2 directory - taken out.
nlohmann::json savedTrackBody (const nlohmann::json& root, int t)
{
    auto track = dusk::json::array (root, "tracks")[(size_t) t];
    track.erase ("input_source");
    track.erase ("lv2_state_tag");
    return track;
}

std::vector<std::uint8_t> builtinState (AudioEngine& engine, int t)
{
    std::vector<std::uint8_t> state;
    engine.getChannelStrip (t).getBuiltinSlot().saveState (state);
    return state;
}

// Loads Utility on the strip at slot t, moved to gainDb, unloaded when the case
// ends wherever the strip has got to by then.
bool loadUtility (ScenarioContext& ctx, int t, float gainDb)
{
    auto& strip = ctx.engine().getChannelStrip (t);
    std::string error;
    if (! ctx.expect (strip.loadBuiltin ("dusk.builtin.utility", error),
                      "could not load Utility on track " + std::to_string (t + 1) + ": " + error))
        return false;
    ctx.keep (strip.insertMode);
    ctx.cleanup ([&strip] { strip.unloadBuiltin(); });
    strip.insertMode.store (ChannelStrip::kInsertPlugin);
    auto& slot = strip.getBuiltinSlot();
    for (int index = 0; index < slot.paramCount(); ++index)
        if (const auto* info = slot.paramInfo (index);
            info != nullptr && info->id != nullptr && std::string_view (info->id) == "gain_db")
        {
            slot.setParamValue (index, gainDb);
            return true;
        }
    return ctx.expect (false, "Utility has no gain parameter");
}

// RMS at the master's left output of a 440 Hz tone arriving on input `input`,
// every other input silent, after the smoothers have settled.
double masterLevelForToneOn (AudioEngine& engine, int input)
{
    std::vector<std::vector<float>> in ((size_t) kInputs, std::vector<float> ((size_t) kFrames, 0.0f));
    std::array<float, kFrames> outL {}, outR {};
    std::vector<const float*> inputs;
    for (auto& channel : in) inputs.push_back (channel.data());
    float* outputs[] = { outL.data(), outR.data() };

    constexpr int kWarm = 40, kMeasure = 20;
    double phase = 0.0, sum = 0.0;
    const double step = kTwoPi * 440.0 / ScenarioContext::kSampleRate;
    for (int block = 0; block < kWarm + kMeasure; ++block)
    {
        for (auto& s : in[(size_t) input])
        {
            s = 0.25f * (float) std::sin (phase);
            phase = std::fmod (phase + step, kTwoPi);
        }
        engine.audioDeviceIOCallback (inputs.data(), kInputs, outputs, 2, kFrames, {});
        if (block < kWarm) continue;
        for (const float s : outL) sum += (double) s * s;
    }
    return std::sqrt (sum / (double) (kMeasure * kFrames));
}

double db (double level, double reference)
{
    return 20.0 * std::log10 (std::max (level, 1.0e-9) / std::max (reference, 1.0e-9));
}

AudioRegion regionAt (std::int64_t start, const std::string& name)
{
    AudioRegion region;
    region.file = SessionFile ((std::string ("/nonexistent/") + name + ".wav").c_str());
    region.timelineStart = start;
    region.lengthInSamples = 4800;
    return region;
}

// Track 18 dragged to the top: its strip, the Utility it hosts with its live
// settings, its regions, name and routing all land on track 1, tracks 1-17
// shift down one, and the strip at each slot is bound to that slot's track.
ScenarioResult runMoveCarriesTheStrip (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& master = session.master();
    for (auto* value : { &master.eqEnabled, &master.compEnabled, &master.tapeEnabled })
        ctx.keep (*value);
    for (auto* value : { &session.mcu.selectedChannel, &session.tuneTrackIndex, &session.midiLearnPending })
        ctx.keep (*value);
    master.eqEnabled.store (false);
    master.compEnabled.store (false);
    master.tapeEnabled.store (false);

    if (! loadUtility (ctx, 0, 3.0f) || ! loadUtility (ctx, kMoved, -6.0f))
        return ctx.verdict();

    // Track 18 hears its own input; track 17 is faded out, so a strip still
    // reading slot 18's controls after the move would go silent.
    auto& moving = session.track (kMoved);
    moving.mode.store ((int) Track::Mode::Mono);
    moving.inputSource.store (kInputFollowsTrack);
    moving.inputMonitor.store (true);
    moving.regions = { regionAt (4800, "eighteen") };
    session.track (kMoved - 1).strip.faderDb.store (ChannelStripParams::kFaderMinDb);
    session.track (0).inputSource.store (5);
    session.track (0).regions = { regionAt (9600, "one") };
    session.recomputeRtCounters();

    session.mcu.selectedChannel.store (kMoved);
    session.tuneTrackIndex.store (kMoved - 1);
    session.midiLearnPending.store (3);
    engine.getRegionClipboard().sourceTrack = kMoved;

    const auto before = saved (session);
    const auto order = stripOrder (engine);
    const auto utility1 = builtinState (engine, 0);
    const auto utility18 = builtinState (engine, kMoved);
    ctx.expect (! utility1.empty() && utility1 != utility18,
                "the two Utility inserts do not save different states");
    const double levelBefore = masterLevelForToneOn (engine, kMoved);
    ctx.note ("track 18 on its own input before the move: " + std::to_string (levelBefore));
    if (! ctx.expect (levelBefore > 0.01, "track 18 does not reach the master before the move"))
        return ctx.verdict();

    const auto plan = planBlockMove ({ kMoved }, 0);
    StripOrder seenBefore {}, seenAfter {};
    int hookCalls = 0;
    engine.onBeforeTracksMove = [&] (const TrackMovePlan&) { seenBefore = stripOrder (engine); ++hookCalls; };
    engine.onTracksMoved = [&] (const TrackMovePlan& moved)
    {
        seenAfter = stripOrder (engine);
        ++hookCalls;
        ctx.expect (moved.newToOld == plan.newToOld, "the hook saw another plan");
    };
    restoreOrderAtEnd (ctx);

    if (! ctx.expect (commitTrackMove (engine, plan), "the move was refused"))
        return ctx.verdict();

    const auto expected = movedOrder (order, plan);
    ctx.expect (stripOrder (engine) == expected, "the strips did not move with their tracks");
    ctx.expect (hookCalls == 2 && seenBefore == order && seenAfter == expected,
                "the move hooks did not run once each, around the move");
    ctx.expect (&engine.getChannelStrip (0) == order[(size_t) kMoved],
                "track 1 is not running the strip track 18 had");
    ctx.expect (builtinState (engine, 0) == utility18 && builtinState (engine, 1) == utility1,
                "a moved Utility does not hold the settings it had");

    const auto after = saved (session);
    for (int t = 0; t < kN; ++t)
    {
        const int from = plan.newToOld[(size_t) t];
        ctx.expect (savedTrackBody (after, t) == savedTrackBody (before, from),
                    "track " + std::to_string (t + 1) + " does not save as track "
                        + std::to_string (from + 1) + " did");
    }
    ctx.expect (session.track (0).name == "18" && session.track (1).name == "1"
                    && session.track (17).name == "17" && session.track (18).name == "19",
                "the names did not shift with their tracks");
    ctx.expect (session.track (0).regions.size() == 1 && session.track (0).regions[0].timelineStart == 4800
                    && session.track (1).regions.size() == 1
                    && session.track (1).regions[0].timelineStart == 9600,
                "the regions did not move with their tracks");
    ctx.expect (session.track (0).inputSource.load() == kMoved && session.track (1).inputSource.load() == 5
                    && session.track (2).inputSource.load() == 1
                    && session.track (18).inputSource.load() == kInputFollowsTrack,
                "a moved track did not keep the input it had");
    ctx.expect (session.lv2StateTagFor (0) == "track18" && session.lv2StateTagFor (1) == "track01"
                    && session.track (18).lv2StateTag.empty(),
                "the LV2 state directories did not go with their tracks");
    ctx.expect (session.mcu.selectedChannel.load() == 0 && session.tuneTrackIndex.load() == kMoved,
                "the control surface's channel or the tuner did not follow its track");
    ctx.expect (session.midiLearnPending.load() == -1, "a pending MIDI learn survived the move");
    ctx.expect (engine.getRegionClipboard().sourceTrack == 0, "the clipboard's source track did not follow");

    const double levelAfter = masterLevelForToneOn (engine, kMoved);
    ctx.note ("the same input after the move: " + std::to_string (levelAfter) + " ("
              + std::to_string (db (levelAfter, levelBefore)) + " dB)");
    ctx.expect (std::abs (db (levelAfter, levelBefore)) < 0.1,
                "the moved track does not sound as it did: its strip is not reading its own controls");
    return ctx.verdict();
}

// The move is the only step in the history, undo puts every track back as it
// was, followed inputs included, before anything is told the tracks moved, and
// redo moves them again. Neither runs, nor costs the history, while the
// transport rolls or while a track it would shift is frozen.
ScenarioResult runMoveUndo (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& undo = engine.getUndoManager();
    restoreOrderAtEnd (ctx);

    session.track (3).name = "Snare";
    session.track (kMoved).name = "Kick";
    session.track (kMoved).regions = { regionAt (48000, "kick") };
    for (const char* step : { "earlier one", "earlier two" })
    {
        undo.beginNewTransaction (step);
        undo.perform (new ParamEditAction ([] {}, [] {}));
    }

    const auto before = saved (session);
    const auto order = stripOrder (engine);
    const auto plan = planBlockMove ({ kMoved }, 0);
    const auto moved = movedOrder (order, plan);

    if (! ctx.expect (commitTrackMove (engine, plan), "the move was refused"))
        return ctx.verdict();
    ctx.expect (undo.getUndoDescription() == kMoveTracksTransaction, "the move is not the step on top");

    const auto expectHeld = [&] (TrackMoveStep step, TrackMoveRefusal::Kind kind, const std::string& message,
                                 const std::string& when)
    {
        const bool redo = step == TrackMoveStep::Redo;
        const auto refusal = trackMoveUndoRefusal (engine, redo);
        ctx.expect (refusal.kind == kind, when + " did not give the expected reason");
        ctx.expect (trackMoveRefusalMessage (refusal, step) == message,
                    when + " said \"" + trackMoveRefusalMessage (refusal, step) + "\"");
        const auto orderWas = stripOrder (engine);
        ctx.expect (! (redo ? redoTransaction (engine) : undoTransaction (engine)), when + " ran");
        ctx.expect (stripOrder (engine) == orderWas, when + " still moved the tracks");
        ctx.expect ((redo ? undo.canRedo() : undo.canUndo())
                        && (redo ? undo.getRedoDescription() : undo.getUndoDescription()) == kMoveTracksTransaction,
                    when + " cost the history");
    };

    auto& transport = engine.getTransport();
    transport.setState (Transport::State::Playing);
    expectHeld (TrackMoveStep::Undo, TrackMoveRefusal::Kind::Playing,
                "Stop playback, then undo the move again.", "an undo while playing");
    transport.setState (Transport::State::Stopped);

    // Freezing is not a step in the history, so a track the move shifted can
    // be frozen under it; its freeze file is named by the slot it is on now.
    session.track (5).frozen.store (true);
    expectHeld (TrackMoveStep::Undo, TrackMoveRefusal::Kind::Frozen,
                "Unfreeze track 6, then undo the move again. A frozen track can't be moved or shifted.",
                "an undo with a shifted track frozen");
    ctx.expect (! engine.moveTracks (invertTrackMove (plan)), "the engine shifted a frozen track");
    session.track (5).frozen.store (false);
    // One the undo leaves where it is does not hold it up.
    session.track (20).frozen.store (true);

    // An input the move pinned and the user then changed stays changed.
    session.track (1 + 3).inputSource.store (9);

    std::array<int, kN> inputsSeen {};
    engine.onTracksMoved = [&] (const TrackMovePlan&)
    {
        for (int t = 0; t < kN; ++t) inputsSeen[(size_t) t] = session.track (t).inputSource.load();
    };
    if (! ctx.expect (undoTransaction (engine), "undo refused with the transport stopped"))
        return ctx.verdict();
    engine.onTracksMoved = nullptr;
    session.track (20).frozen.store (false);
    ctx.expect (! undo.canUndo(), "the history held more than the move");
    ctx.expect (stripOrder (engine) == order, "undo did not put the strips back");
    ctx.expect (session.track (3).inputSource.load() == 9, "undo discarded an input changed after the move");
    {
        std::array<int, kN> inputsNow {};
        for (int t = 0; t < kN; ++t) inputsNow[(size_t) t] = session.track (t).inputSource.load();
        ctx.expect (inputsSeen == inputsNow && inputsSeen[5] == kInputFollowsTrack,
                    "the tracks-moved hook ran before the undo had put the followed inputs back");
    }
    session.track (3).inputSource.store (kInputFollowsTrack);
    {
        std::vector<std::string> different;
        const auto now = saved (session);
        for (int t = 0; t < kN; ++t)
            if (dusk::json::array (now, "tracks")[(size_t) t] != dusk::json::array (before, "tracks")[(size_t) t])
                different.push_back (std::to_string (t + 1));
        std::string list;
        for (const auto& t : different) list += (list.empty() ? "" : ", ") + t;
        ctx.expect (different.empty(), "undo did not put back tracks " + list);
    }

    transport.setState (Transport::State::Playing);
    expectHeld (TrackMoveStep::Redo, TrackMoveRefusal::Kind::Playing,
                "Stop playback, then redo the move again.", "a redo while playing");
    transport.setState (Transport::State::Stopped);
    session.track (0).frozen.store (true);
    expectHeld (TrackMoveStep::Redo, TrackMoveRefusal::Kind::Frozen,
                "Unfreeze track 1, then redo the move again. A frozen track can't be moved or shifted.",
                "a redo with a track it shifts frozen");
    session.track (0).frozen.store (false);

    ctx.expect (redoTransaction (engine), "redo refused");
    ctx.expect (stripOrder (engine) == moved && session.track (0).name == "Kick"
                    && session.track (4).name == "Snare",
                "redo did not move the tracks again");
    ctx.expect (undoTransaction (engine) && stripOrder (engine) == order, "the closing undo refused");
    return ctx.verdict();
}

// A move waits for a stopped transport with no pass open on a track it moves,
// and never shifts a frozen track; a refused one leaves the tracks and the
// history alone.
ScenarioResult runMoveRefusals (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& undo = engine.getUndoManager();
    restoreOrderAtEnd (ctx);

    undo.beginNewTransaction ("earlier");
    undo.perform (new ParamEditAction ([] {}, [] {}));

    const auto order = stripOrder (engine);
    const auto plan = planBlockMove ({ kMoved }, 0);
    const auto expectRefused = [&] (TrackMoveRefusal::Kind kind, const std::string& message,
                                    const std::string& when)
    {
        const auto refusal = trackMoveRefusalFor (session, engine, plan);
        ctx.expect (refusal.kind == kind, when + " did not give the expected reason");
        ctx.expect (trackMoveRefusalMessage (refusal) == message,
                    when + " said \"" + trackMoveRefusalMessage (refusal) + "\"");
        ctx.expect (! commitTrackMove (engine, plan), "the tracks moved " + when);
        ctx.expect (stripOrder (engine) == order && session.track (0).name == "1",
                    "a move refused " + when + " still moved something");
        ctx.expect (undo.getUndoDescription() == "earlier", "a move refused " + when + " cost the history");
    };

    auto& transport = engine.getTransport();
    transport.setState (Transport::State::Playing);
    expectRefused (TrackMoveRefusal::Kind::Playing, "Stop playback, then move the tracks again.",
                   "while playing");
    ctx.expect (! engine.moveTracks (plan), "the engine moved tracks while playing");
    transport.setState (Transport::State::Recording);
    expectRefused (TrackMoveRefusal::Kind::Playing, "Stop playback, then move the tracks again.",
                   "while recording");
    transport.setState (Transport::State::Stopped);

    {
        auto& lane = session.track (4).automationLanes[(size_t) AutomationParam::FaderDb];
        const auto laneWas = lane.pointsConst();
        AutomationPassRecorder pass (AutomationParam::FaderDb);
        pass.record (lane, 1000, -3.0f, 120.0f, 0, 0);
        expectRefused (TrackMoveRefusal::Kind::Playing, "Stop playback, then move the tracks again.",
                       "during an automation pass on a track that shifts");
        pass.finish (lane);
        lane.publishPoints (laneWas);
    }

    session.track (5).frozen.store (true);
    session.track (9).frozen.store (true);
    expectRefused (TrackMoveRefusal::Kind::Frozen,
                   "Unfreeze track 6, then move the tracks again. A frozen track can't be moved or shifted.",
                   "with a frozen track in the way");
    ctx.expect (! engine.moveTracks (plan), "the engine shifted a frozen track");
    session.track (5).frozen.store (false);
    session.track (9).frozen.store (false);

    session.track (20).frozen.store (true);
    ctx.expect (trackMoveRefusalFor (session, engine, plan).kind == TrackMoveRefusal::Kind::None,
                "a frozen track the move leaves where it is refused the move");
    ctx.expect (commitTrackMove (engine, plan) && stripOrder (engine) == movedOrder (order, plan),
                "a move past a frozen track that stays put did not run");
    ctx.expect (session.track (20).frozen.load(), "the frozen track that stays put lost its freeze");
    session.track (20).frozen.store (false);
    return ctx.verdict();
}

// The interleaving a live session has: a device thread runs the callback, with
// worker lanes, the whole time the message thread moves tracks, undoes and
// redoes. Each move waits out the block in flight and rewires the strips with
// none running, so however the two threads meet, the strips end where the last
// move left them. Worth running under ThreadSanitizer.
ScenarioResult runMoveUnderRunningCallback (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    for (int t = 0; t < kN; ++t)
        session.track (t).inputMonitor.store (true);
    engine.setWorkerCountForTest (2);
    ctx.cleanup ([&engine] { engine.applyDesiredWorkers(); });
    restoreOrderAtEnd (ctx);

    std::atomic<bool> stop { false };
    std::atomic<int> blocks { 0 };
    std::thread device ([&engine, &stop, &blocks]
    {
        std::vector<std::vector<float>> in ((size_t) kInputs, std::vector<float> ((size_t) kFrames, 0.1f));
        std::array<float, kFrames> outL {}, outR {};
        std::vector<const float*> inputs;
        for (auto& channel : in) inputs.push_back (channel.data());
        float* outputs[] = { outL.data(), outR.data() };
        while (! stop.load (std::memory_order_acquire))
        {
            engine.audioDeviceIOCallback (inputs.data(), kInputs, outputs, 2, kFrames, {});
            blocks.fetch_add (1, std::memory_order_release);
        }
    });
    const auto waitForBlock = [&blocks]
    {
        const int seen = blocks.load (std::memory_order_acquire);
        while (blocks.load (std::memory_order_acquire) == seen)
            std::this_thread::yield();
    };

    waitForBlock();
    const auto gatedBefore = engine.getGatedBlockCount();
    const int blocksBefore = blocks.load();
    const auto order = stripOrder (engine);
    const auto plan = planBlockMove ({ 3, kMoved }, 10);
    int moves = 0;
    constexpr int kRounds = 25;
    for (int round = 0; round < kRounds; ++round)
    {
        moves += commitTrackMove (engine, plan) ? 1 : 0;
        moves += undoTransaction (engine) ? 1 : 0;
        moves += redoTransaction (engine) ? 1 : 0;
        moves += engine.moveTracks (invertTrackMove (plan)) ? 1 : 0;
    }
    waitForBlock();
    stop.store (true, std::memory_order_release);
    device.join();

    ctx.note (std::to_string (blocks.load() - blocksBefore) + " blocks ran across the moves, "
              + std::to_string (engine.getGatedBlockCount() - gatedBefore) + " of them gated");
    ctx.expect (moves == 4 * kRounds, "only " + std::to_string (moves) + " of the moves ran");
    ctx.expect (stripOrder (engine) == order, "the strips did not end where the last move left them");
    ctx.expect (session.track (3).name == "4" && session.track (kMoved).name == "18",
                "the tracks did not end where the last move left them");
    return ctx.verdict();
}

// A take solo moves with its track, and the streams the move rebuilds play it on
// the track's new slot, as every other rebuild honours a solo.
ScenarioResult runMoveKeepsTheAudition (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& playback = engine.getPlaybackEngine();
    restoreOrderAtEnd (ctx);
    ctx.cleanup ([&engine] { engine.clearTakeAudition(); });

    AudioTake take;
    take.id = session.allocateTakeId();
    take.name = "Take 1";
    take.file = SessionFile ("/nonexistent/eighteen-take.wav");
    take.lengthInSamples = 4800;
    session.track (kMoved).takes.push_back (take);
    engine.setTakeAudition (kMoved, take.id);
    const auto before = playback.playingAudition();
    if (! ctx.expect (before.trackIdx == kMoved && before.takeId == take.id,
                      "the streams did not play the solo before the move"))
        return ctx.verdict();

    if (! ctx.expect (engine.moveTracks (planBlockMove ({ kMoved }, 0)), "the move was refused"))
        return ctx.verdict();
    ctx.expect (session.takeAudition.trackIdx == 0 && session.takeAudition.takeId == take.id,
                "the solo did not move with its track");
    const auto after = playback.playingAudition();
    ctx.expect (after.trackIdx == 0 && after.takeId == take.id,
                after.trackIdx < 0 ? std::string ("after the move the streams play no solo")
                                   : "after the move the streams play a solo on track " + std::to_string (after.trackIdx + 1));
    return ctx.verdict();
}

// The suite's reset between cases puts moved tracks back and says so, so a case
// that leaves them moved fails instead of quietly skewing every later one.
ScenarioResult runWorldResetPutsTracksBack (ScenarioContext& ctx)
{
    ScenarioWorld world;
    auto& engine = world.engine();
    const auto order = stripOrder (engine);
    ctx.expect (world.reset().empty(), "resetting an untouched world reported something left behind");

    if (! ctx.expect (engine.moveTracks (planBlockMove ({ 2, 9 }, 20)), "the tracks did not move"))
        return ctx.verdict();
    world.session().track (4).frozen.store (true);
    engine.onTracksMoved = [] (const TrackMovePlan&) {};
    const auto dirt = world.reset();
    for (const auto& line : dirt) ctx.note ("reset: " + line);
    ctx.expect (dirt.size() == 2, "the reset did not name both the moved tracks and the hook");
    ctx.expect (stripOrder (engine) == order, "the reset did not put the strips back in slot order");
    ctx.expect (! engine.onTracksMoved, "the reset left the hook installed");
    ctx.expect (world.reset().empty(), "a second reset still found something");
    return ctx.verdict();
}

const ScenarioRegistrar worldReset { Scenario {
    "engine.scenario_world_puts_moved_tracks_back",
    { "engine", "move" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runWorldResetPutsTracksBack (ctx); }
} };

const ScenarioRegistrar live { Scenario {
    "session.move_tracks_under_a_running_callback",
    { "session", "move", "rt" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMoveUnderRunningCallback (ctx); }
} };

const ScenarioRegistrar carries { Scenario {
    "session.move_tracks_carries_the_strip",
    { "session", "move", "plugin", "state" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMoveCarriesTheStrip (ctx); }
} };

const ScenarioRegistrar undoStep { Scenario {
    "session.move_tracks_is_one_undo_step",
    { "session", "move", "undo" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMoveUndo (ctx); }
} };

const ScenarioRegistrar refusals { Scenario {
    "session.move_tracks_refusals",
    { "session", "move", "freeze" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMoveRefusals (ctx); }
} };

const ScenarioRegistrar audition { Scenario {
    "session.move_tracks_keeps_the_take_solo",
    { "session", "move", "take" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runMoveKeepsTheAudition (ctx); }
} };
} // namespace
} // namespace duskstudio::scenario
