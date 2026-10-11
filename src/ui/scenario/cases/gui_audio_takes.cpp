#include "../GuiScenarioSupport.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../session/SessionSerializer.h"
#include "../../../session/TakeComp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
// Three takes under a mixed comp. A drag across the middle take's lane shows its span
// while held, puts exactly that span of the take on the track on release as one undo
// step snapped to the grid, and a Cmd drag lands where the pointer went.
std::optional<ScenarioResult> runAudioTakePromoteDrag (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore, points = session.tempoMap.points()]
    {
        drainModals (host);
        host.closeAudioEditor();
        engine.clearTakeAudition();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        session.tempoMap.setPoints (points);
    });

    constexpr int kRate = 48000;
    constexpr std::int64_t kLength = 96000;
    auto& track = session.track (0);
    track.frozen.store (false);
    track.regions.clear();
    track.takes.clear();
    std::vector<TakeId> ids;
    for (int pass = 0; pass < 3; ++pass)
    {
        const auto path = ctx.tempDir() / ("Take" + std::to_string (pass + 1) + ".wav");
        dusk::audio::WriteSpec spec;
        spec.sampleRate = kRate;
        spec.numChannels = 1;
        auto writer = dusk::audio::FileWriter::create (path, spec);
        std::vector<float> signal ((std::size_t) kLength);
        const float level = 0.3f + 0.25f * (float) pass;
        for (std::size_t i = 0; i < signal.size(); ++i)
        {
            const double t = (double) i / kRate;
            signal[i] = level * (float) ((0.6 + 0.4 * std::sin (6.2831853 * (1.5 + pass) * t))
                                         * std::sin (6.2831853 * 110.0 * t));
        }
        const float* channels[] = { signal.data() };
        if (! writer || ! writer->write (channels, 1, kLength) || ! writer->flush())
            return ScenarioResult::fail ("could not write take fixture");
        AudioTake take;
        take.id = session.allocateTakeId();
        take.name = "Take " + std::to_string (pass + 1);
        take.file = decltype (take.file) (path.string());
        take.lengthInSamples = kLength;
        track.takes.push_back (take);
        ids.push_back (take.id);
    }
    promoteTakeRange (session, track, track.takes[2], 0, kLength);
    promoteTakeRange (session, track, track.takes[0], 24000, 48000);
    const auto regionsBefore = track.regions;

    session.tempoMap.clear();
    session.tempoBpm.store (120.0f);
    session.beatsPerBar.store (4);
    session.snapResolution = SnapResolution::Quarter;
    session.audioEditorSnap = true;
    engine.getUndoManager().clearUndoHistory();
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    using Span = std::pair<std::int64_t, std::int64_t>;
    const auto point = [&host, &ctx] (TakeId take, std::int64_t sample)
    {
        const auto at = host.audioEditorTakePoint ("lane", take, sample);
        ctx.expect (at.size() == 2, "take lane geometry unavailable");
        return at.size() == 2 ? at : std::vector<int> { 0, 0 };
    };
    // The drag's snapped span at either rate the grid can be read at: the file's own,
    // or the device's before the file's header has been read.
    const auto snappedSpan = [] (Span span)
    { return span == Span { 24000, 72000 } || span == Span { 22050, 66150 }; };
    auto dragged = std::make_shared<Span>();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 600, [&host, &ctx, ids, point]
    {
        ctx.expect (host.audioEditorTakeLanes() == std::vector<std::uint64_t> { ids[2], ids[1], ids[0] },
                    "the lanes are not the three takes, newest first");
        const auto from = point (ids[1], 30000);
        const auto to = point (ids[1], 70000);
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "take lane press failed");
        host.audioEditorPointer (to[0], to[1], true);
    } });
    steps->push_back ({ 400, [&host, &ctx, &track, ids, point, snappedSpan, dragged]
    {
        const auto state = host.audioEditorTakeState();
        if (ctx.expect (state.size() == 5 && state[2] == (std::int64_t) ids[1], "the drag is not held on the take"))
        {
            *dragged = { std::min (state[3], state[4]), std::max (state[3], state[4]) };
            ctx.expect (snappedSpan (*dragged), "the held span did not snap to quarter notes");
        }
        ctx.expect (takeCoverage (track, ids[1]).empty(), "the take went on the track before the release");
        const auto to = point (ids[1], 70000);
        host.audioEditorPointer (to[0], to[1], false);
    } });
    steps->push_back ({ 300, [&host, &ctx, &track, ids, dragged]
    {
        ctx.expect (takeCoverage (track, ids[1]) == std::vector<Span> { *dragged },
                    "the release did not put exactly the held span of the take on the track");
        ctx.expect (takeCoverage (track, ids[0]).empty(), "the span did not replace the first take's part");
        ctx.expect (track.takes.size() == 3, "the drag changed the takes");
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[0] >= 0 && selection[0] < (std::int64_t) track.regions.size()
                        && track.regions[(std::size_t) selection[0]].takeId == ids[1],
                    "the editor did not focus the promoted region");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the promote");
    } });
    steps->push_back ({ 300, [&host, &ctx, &track, ids, regionsBefore]
    {
        ctx.expect (takeCoverage (track, ids[1]).empty()
                        && takeCoverage (track, ids[0]) == std::vector<Span> { { 24000, 48000 } }
                        && track.regions.size() == regionsBefore.size(),
                    "one Undo did not take the whole promote back");
        ctx.expect (host.clickAudioEditorButton ("Redo"), "Redo unavailable after the undo");
    } });
    steps->push_back ({ 300, [&host, &ctx, &track, ids, point, dragged]
    {
        ctx.expect (takeCoverage (track, ids[1]) == std::vector<Span> { *dragged }, "Redo did not bring the span back");
        const auto from = point (ids[0], 60000);
        const auto to = point (ids[0], 84000);
        constexpr int command = 2;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true, command), "Cmd press on the lane failed");
        host.audioEditorPointer (to[0], to[1], true, command);
        host.audioEditorPointer (to[0], to[1], false, command);
    } });
    steps->push_back ({ 400, [&ctx, &track, ids]
    {
        const auto spans = takeCoverage (track, ids[0]);
        ctx.expect (spans.size() == 1 && std::abs (spans[0].first - 60000) < 256
                        && std::abs (spans[0].second - 84000) < 256,
                    "a Cmd drag did not land where the pointer went");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakePromoteDrag { Scenario {
    "gui.audio_take_promote_drag", { "gui", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakePromoteDrag (host, ctx); }
} };

constexpr float kRegionLevel = 0.25f;

bool levelIs (float level, float expected) { return std::abs (level - expected) < 0.02f; }

std::vector<std::uint64_t> laneOrder (const std::vector<TakeId>& oldestFirst)
{
    return std::vector<std::uint64_t> (oldestFirst.rbegin(), oldestFirst.rend());
}

// The editor lists a track's takes as lanes, newest first, whether or not a region
// plays; it opens on a track with takes and no region with none focused, shows no
// lanes on a track without takes, and does not open on a track with neither.
std::optional<ScenarioResult> runAudioTakeLanesOpen (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 3);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[1], 0, kTakeCaseLength);

    auto& plain = session.track (1);
    plain.frozen.store (false);
    plain.regions.clear();
    plain.takes.clear();
    if (! addLevelTake (ctx, plain, "Plain", 0, kTakeCaseLength, 0.3f))
        return ScenarioResult::fail ("could not write region fixture");
    auto region = regionFromTake (plain.takes.front(), 0, kTakeCaseLength);
    plain.takes.clear();
    if (! region) return ScenarioResult::fail ("could not cut the region fixture");
    region->takeId = 0;
    plain.regions.push_back (*region);

    auto& bare = session.track (2);
    bare.frozen.store (false);
    bare.regions.clear();
    bare.takes.clear();

    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");
    const auto expected = laneOrder (*ids);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &track, expected]
    {
        ctx.expect (host.audioEditorTakeLanes() == expected, "the lanes are not the three takes, newest first");
        const auto selection = host.audioEditorSelection();
        ctx.expect (! selection.empty() && selection[0] == 0, "the editor did not focus the track's region");
        host.closeAudioEditor();
        track.regions.clear();
        ctx.expect (host.openAudioEditorOnTakes (0), "a track with takes and no region did not open the editor");
    }, [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the take lanes never showed" });
    steps->push_back ({ 100, [&host, &ctx, expected]
    {
        ctx.expect (host.audioEditorTakeLanes() == expected, "without a region the lanes are not the three takes");
        const auto selection = host.audioEditorSelection();
        ctx.expect (! selection.empty() && selection[0] == -1, "the editor focused a region on a track without one");
        host.closeAudioEditor();
        ctx.expect (host.openAudioEditorOnTakes (1), "a track with a region and no takes did not open the editor");
    }, [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the lanes never showed without a region" });
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.audioEditorTakeLanes().empty(), "a track without takes showed take lanes");
        const auto selection = host.audioEditorSelection();
        ctx.expect (! selection.empty() && selection[0] == 0, "the editor did not focus the region of a track without takes");
        host.closeAudioEditor();
        ctx.expect (! host.openAudioEditorOnTakes (2), "a track with neither regions nor takes opened the editor");
    }, [&host] { return host.audioEditorPoint ("wave", kTakeCaseLength / 2).size() == 2; },
       "the editor on a track without takes never laid out" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// Opened on a region of a track with takes, the editor fits every take, so a punch
// that starts past the region shows in its lane; 0 still fits the region alone.
std::optional<ScenarioResult> runAudioEditorOpensOnEveryTake (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    const auto whole = addLevelTake (ctx, track, "Take 1", 0, kTakeCaseLength, kWholeTakeLevel);
    const auto punch = addLevelTake (ctx, track, "Take 2", 72000, 48000, kShortTakeLevel);
    if (! whole || ! punch) return ScenarioResult::fail ("could not write take fixture");
    auto region = regionFromTake (track.takes[0], 0, 24000);
    if (! region) return ScenarioResult::fail ("could not cut the region fixture");
    track.regions.push_back (*region);
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto opened = std::make_shared<std::vector<double>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, opened]
    {
        *opened = host.audioEditorView();
        const auto selection = host.audioEditorSelection();
        ctx.expect (! selection.empty() && selection[0] == 0, "the editor did not keep the double-clicked region");
        ctx.expect (opened->size() == 3 && std::abs ((*opened)[2]) < 0.5, "the edit cursor is not at the region's start");
        ctx.expect (host.pressAudioEditorKey ("0"), "the fit key was not handled");
    }, [&host] { return host.audioEditorTakeLanes().size() == 2; }, "the take lanes never showed" });
    steps->push_back ({ 100, [&host, &ctx, opened]
    {
        // The track spans 120000 samples, the region 24000: fitting the region alone
        // zooms in five times over what the editor opened on.
        const auto fitted = host.audioEditorView();
        const bool read = opened->size() == 3 && fitted.size() == 3 && (*opened)[0] > 0.0;
        const double ratio = read ? fitted[0] / (*opened)[0] : 0.0;
        ctx.expect (ratio > 4.5 && ratio < 5.5,
                    "the editor did not open on every take (region fit over open zoom " + std::to_string (ratio) + ")");
    }, [&host, opened]
    {
        const auto view = host.audioEditorView();
        return view.size() == 3 && opened->size() == 3 && std::abs (view[0] - (*opened)[0]) > 1.0e-9 * (*opened)[0];
    }, "the fit key never changed the zoom" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// Dragging the take lanes' caption hands the lanes more or less of the editor, and
// each lane grows or shrinks with its share.
std::optional<ScenarioResult> runAudioEditorLaneDivider (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    const auto ids = addLevelTakes (ctx, track, 4);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (ctx.session(), track, track.takes[3], 0, kTakeCaseLength);
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    // The distance between the two newest lanes' waveforms: one lane and its gap.
    const auto pitch = [&host, ids]
    {
        const auto top = host.audioEditorTakePoint ("lane", ids->at (3), kTakeCaseLength / 2);
        const auto next = host.audioEditorTakePoint ("lane", ids->at (2), kTakeCaseLength / 2);
        return top.size() == 2 && next.size() == 2 ? next[1] - top[1] : -1;
    };
    const auto dragBy = [&host, &ctx] (int dy)
    {
        const auto from = host.audioEditorPoint ("divider", 0);
        if (! ctx.expect (from.size() == 2, "the lane divider has no geometry")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the divider did not take the press");
        host.audioEditorPointer (from[0], from[1] + dy, true);
        host.audioEditorPointer (from[0], from[1] + dy, false);
    };
    const auto opened = std::make_shared<int> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&ctx, pitch, opened, dragBy]
    {
        *opened = pitch();
        ctx.expect (*opened > 0, "the lanes have no geometry");
        dragBy (-120);
    }, [&host] { return host.audioEditorTakeLanes().size() == 4; }, "the take lanes never showed" });
    steps->push_back ({ 150, [&ctx, pitch, opened, dragBy]
    {
        ctx.expect (pitch() > *opened, "dragging the divider up did not make the lanes taller ("
                                           + std::to_string (*opened) + " to " + std::to_string (pitch()) + ")");
        // Past where the lanes opened but short of the divider's floor: at the floor an
        // editor on a 900 px screen has room for the newest lane only, and the pitch
        // needs two.
        dragBy (200);
    } });
    steps->push_back ({ 150, [&ctx, pitch, opened]
    {
        ctx.expect (pitch() > 0 && pitch() < *opened, "dragging the divider down did not make the lanes shorter ("
                                                          + std::to_string (*opened) + " to " + std::to_string (pitch()) + ")");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// Down puts the take in the lane below on the focused region and Up the one above,
// each a "Switch take" step; with a range selected only the range switches. T
// auditions the focused region's take, and T again stops.
std::optional<ScenarioResult> runAudioEditorTakeKeys (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 3);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[2], 0, kTakeCaseLength);
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto plays = [&track] (TakeId id)
    {
        return takeCoverage (track, id) == std::vector<std::pair<std::int64_t, std::int64_t>> { { 0, kTakeCaseLength } };
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("cursor down"), "Down was not handled"); },
      [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the take lanes never showed" });
    steps->push_back ({ 150, [&host, &ctx, &engine = ctx.engine(), plays, ids]
    {
        ctx.expect (plays (ids->at (1)), "Down did not put the take in the lane below on the region");
        ctx.expect (undoDescription (engine) == "Switch take", "Down is not a \"Switch take\" step");
        ctx.expect (host.pressAudioEditorKey ("cursor up"), "Up was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, plays, ids]
    {
        ctx.expect (plays (ids->at (2)), "Up did not put the newest take back");
        ctx.expect (host.pressAudioEditorKey ("cursor up"), "a second Up was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, plays, ids]
    {
        ctx.expect (plays (ids->at (2)), "Up past the newest take changed the region");
        ctx.expect (host.audioEditorTakeCaption().find ("No newer take") != std::string::npos,
                    "Up past the newest take did not say why in the caption");
        // A range over the middle of the region switches that alone.
        const auto from = host.audioEditorPoint ("wave", 24000);
        const auto to = host.audioEditorPoint ("wave", 48000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "the waveform has no geometry")) return;
        host.audioEditorPointer (from[0], from[1], true, 1);
        host.audioEditorPointer (to[0], to[1], true, 1);
        host.audioEditorPointer (to[0], to[1], false, 1);
    } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[1] == 1, "the Shift drag did not select a range");
        ctx.expect (host.pressAudioEditorKey ("cursor down"), "Down over a range was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, &track, ids]
    {
        const auto middle = takeCoverage (track, ids->at (1));
        ctx.expect (middle.size() == 1 && std::abs (middle[0].first - 24000) < 256 && std::abs (middle[0].second - 48000) < 256,
                    "Down over a range did not switch just the range");
        ctx.expect (host.pressAudioEditorKey ("T"), "T was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, &session, ids]
    {
        ctx.expect (session.takeAudition.trackIdx == 0 && session.takeAudition.takeId == ids->at (1),
                    "T did not audition the focused region's take");
        ctx.expect (host.pressAudioEditorKey ("T"), "a second T was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &session]
    { ctx.expect (session.takeAudition.trackIdx == -1, "a second T did not stop the audition"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// The seam where one take gives way to the next drags from the take stripe along the
// top of the waveform: both regions' edges move together as one "Move comp seam"
// step, and one Undo puts them back.
std::optional<ScenarioResult> runAudioEditorSeamDrag (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[1], kTakeCaseLength / 2, kTakeCaseLength);
    session.audioEditorSnap = false;
    const auto before = track.regions;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    // Playback rebuilds when the drag lets go: the two region edits it records are
    // one rebuild.
    auto rebuildsBefore = std::make_shared<std::uint64_t>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, rebuildsBefore]
    {
        const auto from = host.audioEditorPoint ("stripe", kTakeCaseLength / 2);
        const auto to = host.audioEditorPoint ("stripe", kTakeCaseLength / 4);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "the take stripe has no geometry")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the seam did not take the press");
        host.audioEditorPointer (to[0], to[1], true);
        *rebuildsBefore = ctx.engine().getPlaybackEngine().rebuildCount();
        host.audioEditorPointer (to[0], to[1], false);
    }, [&host] { return host.audioEditorTakeLanes().size() == 2; }, "the take lanes never showed" });
    steps->push_back ({ 150, [&host, &ctx, &track, &engine = ctx.engine(), ids, rebuildsBefore]
    {
        const auto second = takeCoverage (track, ids->at (1));
        ctx.expect (second.size() == 1 && std::abs (second[0].first - kTakeCaseLength / 4) < 512
                        && second[0].second == kTakeCaseLength,
                    "dragging the seam did not move where the second take starts");
        ctx.expect (undoDescription (engine) == "Move comp seam", "the drag is not one \"Move comp seam\" step");
        const auto rebuilt = engine.getPlaybackEngine().rebuildCount() - *rebuildsBefore;
        ctx.expect (rebuilt == 1, "letting go of the seam rebuilt playback " + std::to_string (rebuilt) + " times");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the seam drag");
    } });
    steps->push_back ({ 150, [&ctx, &track, before]
    { ctx.expect (sameRegions (track.regions, before), "one Undo did not put both regions back"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// A click on a take lane gives that take the comp section under the click, as one
// "Switch take" step; a click where the take has no audio says so; and a divider
// drags from inside a lane as it does from the take stripe.
std::optional<ScenarioResult> runAudioEditorLaneClick (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 3);
    const auto punch = addLevelTake (ctx, track, "Punch", 24000, 48000, 0.9f);
    if (! ids || ! punch) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[2], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[3], 24000, 72000);
    session.audioEditorSnap = false;
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    using Span = std::pair<std::int64_t, std::int64_t>;
    const auto clickLane = [&host, &ctx] (TakeId take, std::int64_t sample)
    {
        const auto at = host.audioEditorTakePoint ("lane", take, sample);
        if (! ctx.expect (at.size() == 2, "take lane geometry unavailable")) return;
        ctx.expect (host.audioEditorPointer (at[0], at[1], true), "the lane did not take the press");
        host.audioEditorPointer (at[0], at[1], false);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [clickLane, ids] { clickLane (ids->at (1), 10000); },
                        [&host] { return host.audioEditorTakeLanes().size() == 4; }, "the take lanes never showed" });
    steps->push_back ({ 150, [&ctx, &track, &engine = ctx.engine(), ids, punch, clickLane]
    {
        ctx.expect (takeCoverage (track, ids->at (1)) == std::vector<Span> { { 0, 24000 + kPunchFadeSamples } },
                    "a click on Take 2 did not give it the first section");
        ctx.expect (takeCoverage (track, *punch) == std::vector<Span> { { 24000, 72000 } }, "the punch lost its section");
        ctx.expect (undoDescription (engine) == "Switch take", "the click is not one \"Switch take\" step");
        clickLane (*punch, 10000);
    } });
    steps->push_back ({ 150, [&host, &ctx, &track, punch]
    {
        ctx.expect (host.audioEditorTakeCaption().find ("has no audio here") != std::string::npos,
                    "a click where the punch has no audio did not say so");
        ctx.expect (takeCoverage (track, *punch) == std::vector<Span> { { 24000, 72000 } },
                    "a click where the punch has no audio changed the comp");
        // The divider where the punch gives way to Take 3, dragged earlier from Take 3's
        // lane: the punch ends where its take does, so it can only give ground.
        const auto seam = compSeamNear (track, 72000, 128);
        if (! ctx.expect (seam.has_value(), "no seam where the punch ends")) return;
        const auto at = track.regions[(std::size_t) seam->right].timelineStart;
        const auto from = host.audioEditorTakePoint ("lane", track.regions[(std::size_t) seam->right].takeId, at);
        const auto to = host.audioEditorTakePoint ("lane", track.regions[(std::size_t) seam->right].takeId, at - 12000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "take lane geometry unavailable")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the divider did not take the press");
        host.audioEditorPointer (to[0], to[1], true);
        host.audioEditorPointer (to[0], to[1], false);
    } });
    steps->push_back ({ 150, [&ctx, &track, &engine = ctx.engine(), punch]
    {
        const auto moved = takeCoverage (track, *punch);
        ctx.expect (moved.size() == 1 && moved[0].first == 24000 && std::abs (moved[0].second - 60000) < 512,
                    "dragging the divider in a lane did not move where the punch ends");
        ctx.expect (undoDescription (engine) == "Move comp seam", "the lane divider drag is not one step");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorLaneClick { Scenario {
    "gui.audio_editor_lane_click", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorLaneClick (host, ctx); }
} };

const ScenarioRegistrar audioEditorSeamDrag { Scenario {
    "gui.audio_editor_seam_drag", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorSeamDrag (host, ctx); }
} };

// Where a fade disc sits on a comp seam the disc keeps its drag: a press on the
// focused region's fade-out disc at the seam sets its fade-out as one "Fade-out"
// step and leaves the seam where it was.
std::optional<ScenarioResult> runAudioEditorFadeDiscAtSeam (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[1], kTakeCaseLength / 2, kTakeCaseLength);
    session.audioEditorSnap = false;
    const auto before = track.regions;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    auto fadeBefore = std::make_shared<std::int64_t> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &track, fadeBefore]
    {
        const auto disc = host.audioEditorPoint ("fadeOut", 0);
        const auto seam = host.audioEditorPoint ("stripe", kTakeCaseLength / 2);
        const auto selection = host.audioEditorSelection();
        if (! ctx.expect (disc.size() == 2 && seam.size() == 2 && ! selection.empty(), "the editor has no geometry")) return;
        if (! ctx.expect (std::abs (disc[0] - seam[0]) <= 4, "the focused region's fade-out disc is not on the seam")) return;
        *fadeBefore = track.regions[(std::size_t) selection[0]].fadeOutSamples;
        ctx.expect (host.audioEditorPointer (disc[0], disc[1], true), "the fade-out disc did not take the press");
        host.audioEditorPointer (disc[0] - 40, disc[1], true);
        host.audioEditorPointer (disc[0] - 40, disc[1], false);
    }, [&host] { return host.audioEditorTakeLanes().size() == 2; }, "the take lanes never showed" });
    steps->push_back ({ 150, [&host, &ctx, &track, &engine = ctx.engine(), before, fadeBefore]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (undoDescription (engine) == "Fade-out",
                    "the press on the fade-out disc at the seam made \"" + undoDescription (engine) + "\", not \"Fade-out\"");
        ctx.expect (sameRegions (track.regions, before), "the press on the fade-out disc moved the seam");
        ctx.expect (! selection.empty() && track.regions[(std::size_t) selection[0]].fadeOutSamples > *fadeBefore,
                    "dragging the fade-out disc at the seam did not lengthen the fade-out");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorFadeDiscAtSeam { Scenario {
    "gui.audio_editor_fade_disc_at_seam", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorFadeDiscAtSeam (host, ctx); }
} };

const ScenarioRegistrar audioEditorTakeKeys { Scenario {
    "gui.audio_editor_take_keys", { "gui", "keyboard", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorTakeKeys (host, ctx); }
} };

const ScenarioRegistrar audioEditorLaneDivider { Scenario {
    "gui.audio_editor_lane_divider", { "gui", "editor", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorLaneDivider (host, ctx); }
} };

const ScenarioRegistrar audioEditorOpensOnEveryTake { Scenario {
    "gui.audio_editor_opens_on_every_take", { "gui", "editor", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorOpensOnEveryTake (host, ctx); }
} };

const ScenarioRegistrar audioTakeLanesOpen { Scenario {
    "gui.audio_take_lanes_open", { "gui", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeLanesOpen (host, ctx); }
} };

// A click on a take's name puts the whole take on the track once the double-click
// time has passed, as one "Promote take" step that a single Undo takes back.
std::optional<ScenarioResult> runAudioTakePromoteName (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const std::pair<std::int64_t, std::int64_t> spans[] { { 0, kTakeCaseLength }, { 12000, 48000 },
                                                          { 0, kTakeCaseLength } };
    std::vector<TakeId> ids;
    for (const auto& [start, length] : spans)
    {
        const auto id = addLevelTake (ctx, track, "Take " + std::to_string (ids.size() + 1), start, length,
                                      0.2f + 0.2f * (float) ids.size());
        if (! id) return ScenarioResult::fail ("could not write take fixture");
        ids.push_back (*id);
    }
    promoteTakeRange (ctx.session(), track, track.takes[2], 0, kTakeCaseLength);
    const auto regionsBefore = track.regions;

    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");
    using Span = std::pair<std::int64_t, std::int64_t>;
    const auto middle = ids[1];
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, middle] { host.revealAudioEditorTake (middle); },
                        [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the take lanes never showed" });
    steps->push_back ({ 100, [&host, &ctx, middle]
    { ctx.expect (host.clickAudioEditorButton (takeControl ("name", middle)), "the take's name is not clickable"); },
      [&host, middle] { return host.audioEditorTakePoint ("name", middle, 0).size() == 2; },
      "the middle take's header never came into view" });
    // No delay: under a renderer that holds the message thread a step's delay costs
    // whole frames, and two of them would outlast the double-click time.
    steps->push_back ({ 0, [&ctx, &track, middle]
    { ctx.expect (takeCoverage (track, middle).empty(), "the name click promoted before the double-click time"); } });
    steps->push_back ({ 100, [&host, &ctx, &engine, &track, ids]
    {
        // The comp's own seam fades reach one fade into the promoted span.
        ctx.expect (takeCoverage (track, ids[2]) == std::vector<Span> { { 0, 12000 + kPunchFadeSamples },
                                                                         { 60000 - kPunchFadeSamples, kTakeCaseLength } },
                    "the promote did not replace exactly the take's span of the comp");
        const auto named = std::count_if (track.regions.begin(), track.regions.end(),
                                          [&ids] (const AudioRegion& r) { return r.takeId == ids[1]; });
        ctx.expect (named == 1, "the promote did not leave one region naming the take");
        ctx.expect (takeCoverage (track, ids[0]).empty() && track.takes.size() == 3, "the promote touched the other takes");
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[0] >= 0 && selection[0] < (std::int64_t) track.regions.size()
                        && track.regions[(std::size_t) selection[0]].takeId == ids[1],
                    "the editor did not focus the promoted region");
        ctx.expect (undoDescription (engine) == "Promote take", "the promote is not the \"Promote take\" step");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the promote");
    }, [&track, middle] { return takeCoverage (track, middle) == std::vector<Span> { { 12000, 60000 } }; },
       "a click on the take's name never put the whole take on the track" });
    steps->push_back ({ 100, [&ctx, &engine, &track, regionsBefore]
    {
        ctx.expect (sameRegions (track.regions, regionsBefore), "Undo did not put the comp back as it was");
        ctx.expect (! engine.getUndoManager().canUndo(), "the promote took more than one undo step");
    }, [&track, middle] { return takeCoverage (track, middle).empty(); }, "Undo did not take the take off the track" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakePromoteName { Scenario {
    "gui.audio_take_promote_name", { "gui", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakePromoteName (host, ctx); }
} };

// A double-click on a take's name opens it for typing without promoting it. Enter
// renames it as one "Rename take" step that a save carries; Escape, and Enter on the
// name as it opened, leave the name and the undo history alone.
std::optional<ScenarioResult> runAudioTakeRename (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto regionsBefore = track.regions;
    const auto newest = ids->back();
    const auto saved = ctx.tempDir() / "renamed" / "session.json";
    std::filesystem::create_directories (saved.parent_path());

    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");
    const auto nameOf = [&track, newest]
    {
        const auto* take = findTake (track, newest);
        return take != nullptr ? take->name : std::string();
    };
    const auto renaming = [&host, newest]
    {
        const auto state = host.audioEditorTakeState();
        return state.size() == 5 && state[0] == (std::int64_t) newest;
    };
    const auto fieldClosed = [&host]
    {
        const auto state = host.audioEditorTakeState();
        return state.size() == 5 && state[0] == 0;
    };
    const auto click = [&host, &ctx, newest]
    {
        ctx.expect (host.doubleClickAudioEditorButton (takeControl ("name", newest)),
                    "the take's name is not clickable");
    };

    const auto nameShown = [&host, newest] { return host.audioEditorTakePoint ("name", newest, 0).size() == 2; };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto doubleClick = [&steps, click, renaming, nameShown] (const char* timeout)
    {
        steps->push_back ({ 100, click, nameShown, "the take's header never came into view" });
        steps->push_back ({ 100, [] {}, renaming, timeout });
    };
    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorTakeLanes().size() == 2; },
                        "the take lanes never showed" });
    doubleClick ("a double-click on the take's name did not open it for typing");
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.typeInAudioEditor ("Chorus lead"), "typing into the field failed"); } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("return"), "Enter was not delivered"); } });
    steps->push_back ({ 100, [&ctx, &engine, &track, regionsBefore, fieldClosed, saved, newest]
    {
        ctx.expect (fieldClosed(), "Enter left the name field open");
        ctx.expect (undoDescription (engine) == "Rename take", "the rename is not the \"Rename take\" step");
        ctx.expect (sameRegions (track.regions, regionsBefore), "the double-click promoted the take");
        Session probe;
        if (ctx.expect (SessionSerializer::save (ctx.session(), saved), "could not save the renamed session")
            && ctx.expect (SessionSerializer::load (probe, saved), "the renamed session did not load"))
        {
            const auto* take = findTake (probe.track (0), newest);
            ctx.expect (take != nullptr && take->name == "Chorus lead", "the new name did not survive save and load");
        }
    }, [nameOf] { return nameOf() == "Chorus lead"; }, "Enter did not rename the take" });

    doubleClick ("the name did not reopen for typing");
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.typeInAudioEditor ("Discarded"), "typing into the field failed"); } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("escape"), "Escape was not delivered"); } });
    steps->push_back ({ 100, [&host, &ctx, nameOf]
    {
        ctx.expect (nameOf() == "Chorus lead", "Escape renamed the take");
        ctx.expect (host.audioEditorOpen(), "Escape in the name field closed the editor");
    }, fieldClosed, "Escape left the name field open" });

    doubleClick ("the name did not reopen for an unchanged Enter");
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("return"), "Enter was not delivered"); } });
    steps->push_back ({ 100, [&host, &ctx, &track, nameOf, regionsBefore]
    {
        ctx.expect (nameOf() == "Chorus lead", "an unchanged Enter changed the name");
        ctx.expect (sameRegions (track.regions, regionsBefore), "a double-click on the name promoted the take");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the rename");
    }, fieldClosed, "the unchanged Enter left the name field open" });
    steps->push_back ({ 100, [&ctx, &engine]
    {
        ctx.expect (! engine.getUndoManager().canUndo(), "Escape or an unchanged Enter recorded an undo step");
    }, [nameOf] { return nameOf() == "Take 2"; }, "Undo did not bring the old name back" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakeRename { Scenario {
    "gui.audio_take_rename", { "gui", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeRename (host, ctx); }
} };

// A name typed with letters outside ASCII is stored as typed.
std::optional<ScenarioResult> runAudioTakeRenameUtf8 (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    const auto ids = addLevelTakes (ctx, track, 1);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    const auto take = ids->front();
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    const std::string typed = "T\xc3\xa5ke \xc3\xbc \xe2\x82\xac";
    const auto nameOf = [&track, take]
    {
        const auto* found = findTake (track, take);
        return found != nullptr ? found->name : std::string();
    };
    const auto click = [&host, &ctx, take]
    { ctx.expect (host.doubleClickAudioEditorButton (takeControl ("name", take)), "the take's name is not clickable"); };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, click, [&host, take] { return host.audioEditorTakePoint ("name", take, 0).size() == 2; },
                        "the take's header never came into view" });
    steps->push_back ({ 100, [&host, &ctx, typed]
    { ctx.expect (host.typeInAudioEditor (typed), "typing into the field failed"); },
    [&host, take] { const auto s = host.audioEditorTakeState(); return s.size() == 5 && s[0] == (std::int64_t) take; },
    "a double-click on the take's name did not open it for typing" });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("return"), "Enter was not delivered"); } });
    steps->push_back ({ 200, [&ctx, nameOf, typed]
    { ctx.expect (nameOf() == typed, "the take was renamed \"" + nameOf() + "\" rather than as typed"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakeRenameUtf8 { Scenario {
    "gui.audio_take_rename_utf8", { "gui", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeRenameUtf8 (host, ctx); }
} };

// Delete asks in the lane first and Cancel keeps the take. Confirmed, it takes the
// take and every region cut from it off the track as one "Delete take" step, and
// Undo brings the take back under its own id and in its own lane. A locked region
// cut from the take refuses the delete with a notice and changes nothing.
std::optional<ScenarioResult> runAudioTakeDelete (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto ids = addLevelTakes (ctx, track, 3);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (ctx.session(), track, track.takes[2], 0, kTakeCaseLength);
    promoteTakeRange (ctx.session(), track, track.takes[0], 24000, 48000);
    const auto regionsBefore = track.regions;
    const auto oldest = ids->front();
    const auto expected = laneOrder (*ids);
    const auto newestSpans = takeCoverage (track, ids->back());

    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");
    using Span = std::pair<std::int64_t, std::int64_t>;
    const auto takeIds = [&track]
    {
        std::vector<TakeId> out;
        for (const auto& take : track.takes) out.push_back (take.id);
        return out;
    };
    const auto confirming = [&host] (TakeId take)
    {
        const auto state = host.audioEditorTakeState();
        return state.size() == 5 && state[1] == (std::int64_t) take;
    };
    const auto shown = [&host, oldest] (const char* kind)
    { return [&host, oldest, kind] { return host.audioEditorTakePoint (kind, oldest, 0).size() == 2; }; };
    const auto press = [&host, &ctx, oldest] (const char* kind)
    {
        return [&host, &ctx, oldest, kind]
        { ctx.expect (host.clickAudioEditorButton (takeControl (kind, oldest)), std::string ("no ") + kind + " control on the lane"); };
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, oldest] { host.revealAudioEditorTake (oldest); },
                        [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the take lanes never showed" });
    steps->push_back ({ 100, press ("delete"), shown ("delete"), "the oldest take's header never came into view" });
    steps->push_back ({ 100, press ("cancel"), shown ("cancel"), "Delete did not ask in the lane" });
    steps->push_back ({ 100, [&ctx, &track, ids, takeIds, regionsBefore]
    {
        ctx.expect (takeIds() == *ids && sameRegions (track.regions, regionsBefore), "Cancel deleted the take");
    }, [confirming] { return confirming (0); }, "Cancel left the question up" });
    steps->push_back ({ 100, press ("delete"), shown ("delete"), "Delete went away after Cancel" });
    steps->push_back ({ 100, press ("confirm"), [confirming, oldest] { return confirming (oldest); },
                        "Delete did not ask again" });
    steps->push_back ({ 100, [&host, &ctx, &engine, &track, ids, takeIds, newestSpans]
    {
        ctx.expect (takeIds() == std::vector<TakeId> { (*ids)[1], (*ids)[2] }, "the delete took more than the one take");
        ctx.expect (std::none_of (track.regions.begin(), track.regions.end(),
                                  [&ids] (const AudioRegion& r) { return r.takeId == ids->front(); }),
                    "a region cut from the deleted take stayed on the track");
        ctx.expect (takeCoverage (track, (*ids)[2]) == newestSpans, "the delete changed the other take's regions");
        ctx.expect (undoDescription (engine) == "Delete take", "the delete is not the \"Delete take\" step");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the delete");
    }, [&host] { return host.audioEditorTakeLanes().size() == 2; }, "Delete did not take the lane away" });
    steps->push_back ({ 100, [&host, &ctx, &engine, &track, ids, takeIds, oldest]
    {
        ctx.expect (takeIds() == *ids, "Undo did not restore the take under its id and in its place");
        ctx.expect (takeCoverage (track, oldest) == std::vector<Span> { { 24000, 48000 } },
                    "Undo did not restore the deleted take's region");
        ctx.expect (! engine.getUndoManager().canUndo(), "the delete took more than one undo step");
        for (auto& region : track.regions)
            if (region.takeId == oldest) region.locked = true;
        host.revealAudioEditorTake (oldest);
    }, [&host, expected] { return host.audioEditorTakeLanes() == expected; }, "Undo did not bring the lane back in order" });
    steps->push_back ({ 100, press ("delete"), shown ("delete"), "the restored take has no Delete" });
    steps->push_back ({ 100, press ("confirm"), [confirming, oldest] { return confirming (oldest); },
                        "Delete did not ask about the take with a locked region" });
    steps->push_back ({ 100, [&host, &ctx, &engine, &track, ids, takeIds, oldest]
    {
        ctx.expect (host.audioEditorTakeNotice() == "A region cut from this take is locked. Unlock it to delete the take.",
                    "the refused delete did not explain itself");
        ctx.expect (takeIds() == *ids && takeCoverage (track, oldest) == std::vector<Span> { { 24000, 48000 } },
                    "a locked region did not stop the delete");
        ctx.expect (! engine.getUndoManager().canUndo(), "the refused delete recorded an undo step");
    }, [&host] { return ! host.audioEditorTakeNotice().empty(); }, "the refused delete showed no notice" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakeDelete { Scenario {
    "gui.audio_take_delete", { "gui", "region", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeDelete (host, ctx); }
} };

// A track holding two or more takes shows their count in its tape-strip label
// cell, and the count follows a take added, deleted in the editor and brought
// back by undo. One take shows "1 take" only while no region on the track can
// reach it; none shows nothing. A click on the count opens the audio editor on
// that track's take lanes, except on a frozen track, which says so instead, and
// the second click of a double-click leaves the editor open.
std::optional<ScenarioResult> runTrackTakeBadge (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto& single = session.track (1);
    auto& empty = session.track (2);
    for (auto* other : { &single, &empty })
    {
        other->frozen.store (false);
        other->mode.store ((int) Track::Mode::Mono);
        other->regions.clear();
        other->takes.clear();
        other->midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    }
    const auto ids = addLevelTakes (ctx, track, 3);
    const auto only = addLevelTake (ctx, single, "Take 1", 0, kTakeCaseLength, 0.3f);
    if (! ids || ! only) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[2], 0, kTakeCaseLength);
    promoteTakeRange (session, single, single.takes[0], 0, kTakeCaseLength);
    const auto oldest = ids->front();
    const auto expected = laneOrder (*ids);
    const bool expanded = host.timelineViewMatches (true);
    // An editor left open would take the T.
    ctx.cleanup ([&host, expanded]
    {
        host.closeAudioEditor();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    if (! expanded) host.pressKey ("T", 't');

    const auto badgeReads = [&host] (int t, std::string text) { return [&host, t, text] { return host.tapeTakeBadgeText (t) == text; }; };
    const auto confirming = [&host] (TakeId take)
    {
        const auto state = host.audioEditorTakeState();
        return state.size() == 5 && state[1] == (std::int64_t) take;
    };
    const auto press = [&host, &ctx, oldest] (const char* kind)
    {
        return [&host, &ctx, oldest, kind]
        { ctx.expect (host.clickAudioEditorButton (takeControl (kind, oldest)), std::string ("no ") + kind + " control on the lane"); };
    };
    const auto shown = [&host, oldest] (const char* kind)
    { return [&host, oldest, kind] { return host.audioEditorTakePoint (kind, oldest, 0).size() == 2; }; };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &single]
    {
        ctx.expect (host.tapeTakeBadgeText (1).empty(), "a track with one take shows a take count");
        ctx.expect (host.tapeTakeBadgeText (2).empty(), "a track with no take shows a take count");
        ctx.expect (! host.clickTapeTakeBadge (1), "a track with one take has a badge to click");
        ctx.expect (addLevelTake (ctx, single, "Take 2", 0, kTakeCaseLength, 0.5f).has_value(),
                    "could not write take fixture");
    }, badgeReads (0, "3 takes"), "a track with three takes does not show \"3 takes\"" });
    steps->push_back ({ 100, [&host, &ctx, &track]
    {
        track.frozen.store (true);
        ctx.expect (host.clickTapeTakeBadge (0), "the badge did not take a click");
    }, badgeReads (1, "2 takes"), "a second take did not bring up \"2 takes\"" });
    steps->push_back ({ 100, [&host, &ctx, &track]
    {
        ctx.expect (! host.audioEditorOpen(), "the badge opened the editor on a frozen track");
        host.closeTopModal();
        track.frozen.store (false);
    }, [&host] { return host.modalText().find ("Track is frozen") != std::string::npos; },
       "the badge on a frozen track did not say it is frozen" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickTapeTakeBadge (0), "the badge did not take a click"); },
    [&host] { return host.modalStackEmpty(); }, "the frozen notice did not close" });
    steps->push_back ({ 100, [&host, oldest] { host.revealAudioEditorTake (oldest); },
                        [&host, expected] { return host.audioEditorOpen() && host.audioEditorTakeLanes() == expected; },
                        "the badge did not open the editor on the track's take lanes" });
    steps->push_back ({ 100, press ("delete"), shown ("delete"), "the oldest take's header never came into view" });
    steps->push_back ({ 100, press ("confirm"), [confirming, shown, oldest] { return confirming (oldest) && shown ("confirm")(); },
                        "Delete did not ask to confirm" });
    steps->push_back ({ 100, [&host] { host.closeAudioEditor(); },
                        [&host] { return host.audioEditorTakeLanes().size() == 2; }, "Delete did not take the lane away" });
    steps->push_back ({ 100, [&ctx] { ctx.engine().getUndoManager().undo(); },
                        [&host, badgeReads] { return ! host.audioEditorOpen() && badgeReads (0, "2 takes")(); },
                        "deleting a take in the editor did not bring the count to \"2 takes\"" });
    auto lone = std::make_shared<TakeId> (0);
    steps->push_back ({ 100, [&ctx, &track, &empty, ids, lone]
    {
        ctx.expect (track.takes.size() == ids->size(), "Undo did not bring the deleted take back");
        const auto id = addLevelTake (ctx, empty, "Take 1", 0, kTakeCaseLength, 0.4f);
        if (ctx.expect (id.has_value(), "could not write take fixture")) *lone = *id;
    }, badgeReads (0, "3 takes"), "undoing the delete did not bring the count back to \"3 takes\"" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickTapeTakeBadge (2, 2), "the \"1 take\" badge did not take a double-click"); },
    badgeReads (2, "1 take"), "a lone take with no region on its track does not show \"1 take\"" });
    steps->push_back ({ 100, [] {},
                        [&host, lone] { return host.audioEditorOpen() && host.audioEditorTakeLanes() == std::vector<std::uint64_t> { *lone }; },
                        "a double-click on the \"1 take\" badge did not leave the editor open on that take's lane" });
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.audioEditorOpen(), "the second click of a double-click on the badge closed the editor");
        host.closeAudioEditor();
    } });
    steps->push_back ({ 100, [] {}, [&host] { return ! host.audioEditorOpen(); }, "the editor did not close" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar trackTakeBadge { Scenario {
    "gui.track_take_badge", { "gui", "tape", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTrackTakeBadge (host, ctx); }
} };

// A lane's solo makes the track play one take alone in place of its regions and
// leaves the playhead where it is. Another lane's solo replaces it, a second press
// ends it, and closing the editor ends it too.
std::optional<ScenarioResult> runAudioTakeAudition (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto region = addLevelTake (ctx, track, "Take 1", 0, kTakeCaseLength, kRegionLevel);
    const auto shortTake = addLevelTake (ctx, track, "Take 2", 24000, 48000, kShortTakeLevel);
    const auto whole = addLevelTake (ctx, track, "Take 3", 0, kTakeCaseLength, kWholeTakeLevel);
    if (! region || ! shortTake || ! whole) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);

    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");
    engine.getTransport().locate (5000);
    // Read while stopped, when the audio thread leaves the track's streams alone.
    const auto levelAt = [&engine] (std::int64_t sample)
    {
        std::array<float, 64> probe {};
        const int frames = std::clamp (engine.getCurrentBlockSize(), 1, (int) probe.size());
        engine.getPlaybackEngine().readForTrack (0, sample, probe.data(), nullptr, frames);
        float peak = 0.0f;
        for (int i = 0; i < frames; ++i) peak = std::max (peak, std::abs (probe[(std::size_t) i]));
        return peak;
    };
    const auto auditioning = [&session] (TakeId take)
    { return session.takeAudition.trackIdx == 0 && session.takeAudition.takeId == take; };
    const auto audition = [&host, &ctx] (TakeId take)
    { ctx.expect (host.clickAudioEditorButton (takeControl ("audition", take)), "no solo on the take's lane"); };
    const auto shownAudition = [&host] (TakeId take)
    { return [&host, take] { return host.audioEditorTakePoint ("audition", take, 0).size() == 2; }; };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, t = *shortTake] { host.revealAudioEditorTake (t); },
                        [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the take lanes never showed" });
    steps->push_back ({ 100, [audition, t = *shortTake] { audition (t); }, shownAudition (*shortTake),
                        "the short take's header never came into view" });
    steps->push_back ({ 100, [&ctx, &engine]
    {
        ctx.expect (engine.getTransport().getPlayhead() == 5000, "a stopped solo moved the playhead");
    }, [auditioning, t = *shortTake] { return auditioning (t); }, "the solo did not solo the take" });
    steps->push_back ({ 100, [&host, &ctx, levelAt, t = *whole]
    {
        ctx.expect (levelIs (levelAt (48000), kShortTakeLevel), "the track does not play the auditioned take");
        ctx.expect (levelAt (12000) < 1.0e-4f && levelAt (84000) < 1.0e-4f,
                    "the track still plays its region beside the auditioned take");
        host.revealAudioEditorTake (t);
    }, [levelAt] { return levelAt (48000) > 0.1f; }, "the auditioned take never played" });
    steps->push_back ({ 100, [audition, t = *whole] { audition (t); }, shownAudition (*whole),
                        "the whole take's header never came into view" });
    steps->push_back ({ 100, [&ctx, &engine]
    {
        ctx.expect (engine.getTransport().getPlayhead() == 5000, "the second solo moved the playhead");
    }, [auditioning, t = *whole] { return auditioning (t); }, "another lane's solo did not replace the first" });
    steps->push_back ({ 100, [&ctx, levelAt, audition, t = *whole]
    {
        ctx.expect (levelIs (levelAt (12000), kWholeTakeLevel) && levelIs (levelAt (48000), kWholeTakeLevel),
                    "the track does not play the second take alone");
        audition (t);
    }, [levelAt] { return levelAt (12000) > 0.1f; }, "the second auditioned take never played" });
    steps->push_back ({ 100, [] {}, [&session] { return session.takeAudition.trackIdx == -1; },
                        "a second Audition press did not end the audition" });
    steps->push_back ({ 100, [&host, t = *shortTake] { host.revealAudioEditorTake (t); },
                        [levelAt] { return levelIs (levelAt (12000), kRegionLevel); },
                        "the track did not play its region again once the audition ended" });
    steps->push_back ({ 100, [audition, t = *shortTake] { audition (t); }, shownAudition (*shortTake),
                        "the short take's header did not come back into view" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("escape"), "Escape was not delivered"); },
      [auditioning, t = *shortTake] { return auditioning (t); }, "the solo did not start again" });
    steps->push_back ({ 100, [&ctx, &session]
    {
        ctx.expect (session.takeAudition.trackIdx == -1 && session.takeAudition.takeId == 0,
                    "closing the editor left the audition set");
    }, [&host] { return ! host.audioEditorOpen(); }, "Escape did not close the editor" });
    steps->push_back ({ 100, [] {}, [levelAt] { return levelIs (levelAt (48000), kRegionLevel); },
                        "after the editor closed the track still played the auditioned take" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakeAudition { Scenario {
    "gui.audio_take_audition", { "gui", "region", "take", "playback" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeAudition (host, ctx); }
} };

// A solo pressed while the transport rolls is heard at once: the track's streams
// switch to the take within moments and the caption says the track plays only it.
// Pressed again it ends at once, and the caption goes back to its hint.
std::optional<ScenarioResult> runAudioTakeAuditionWhileRolling (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    ctx.cleanup ([&engine] { engine.stop(); });
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    auto& transport = engine.getTransport();
    const auto newest = ids->back();
    const auto solo = [&host, &ctx, newest]
    { ctx.expect (host.clickAudioEditorButton (takeControl ("audition", newest)), "no solo on the take's lane"); };
    const auto streamsPlay = [&engine] (TakeId take)
    {
        return [&engine, take]
        {
            const auto playing = engine.getPlaybackEngine().playingAudition();
            return take == 0 ? playing.trackIdx == -1 : playing.trackIdx == 0 && playing.takeId == take;
        };
    };
    const std::string soloed = "Solo \"Take 2\": the track plays only this take.";
    const std::string idle = "Click a take to use it for that section, drag across it to pick any range, or drag a "
                             "divider to move a split.";

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&engine] { engine.play(); },
                        [&host] { return host.audioEditorTakeLanes().size() == 2; }, "the take lanes never showed" });
    steps->push_back ({ 100, solo, [&transport] { return transport.isPlaying(); }, "the transport did not start" });
    steps->push_back ({ 100, [&ctx, &host, soloed]
    {
        ctx.expect (host.audioEditorTakeCaption() == soloed,
                    "a solo pressed while rolling reads '" + host.audioEditorTakeCaption() + "'");
    }, streamsPlay (newest), "a solo pressed while rolling was not heard before the next Play" });
    steps->push_back ({ 100, solo });
    steps->push_back ({ 100, [&ctx, &host, idle, &transport]
    {
        ctx.expect (transport.isPlaying(), "ending the solo stopped the transport");
        ctx.expect (host.audioEditorTakeCaption() == idle,
                    "ending a solo while rolling reads '" + host.audioEditorTakeCaption() + "'");
    }, streamsPlay (0), "ending a solo while rolling was not heard before the next Play" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakeAuditionWhileRolling { Scenario {
    "gui.audio_take_audition_while_rolling", { "gui", "region", "take", "playback" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeAuditionWhileRolling (host, ctx); }
} };

// A promote over a locked region, and a take edit on a frozen track, are refused and
// the lane caption says why. A promote that fails for any other reason (a take with no
// length) blames no lock.
std::optional<ScenarioResult> runAudioTakeRefusalNotices (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    const auto empty = addLevelTake (ctx, track, "Empty", 0, 480, 0.3f);
    if (! ids || ! empty) return ScenarioResult::fail ("could not write take fixture");
    track.takes.back().lengthInSamples = 0;
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto regionsBefore = track.regions;
    const auto other = ids->back();
    session.audioEditorSnap = false;
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto lanePoint = [&host, &ctx, other] (std::int64_t sample)
    {
        const auto at = host.audioEditorTakePoint ("lane", other, sample);
        ctx.expect (at.size() == 2, "take lane geometry unavailable");
        return at.size() == 2 ? at : std::vector<int> { 0, 0 };
    };
    const auto dragAcross = [&host, lanePoint]
    {
        const auto from = lanePoint (24000);
        const auto to = lanePoint (48000);
        host.audioEditorPointer (from[0], from[1], true);
        host.audioEditorPointer (to[0], to[1], true);
        host.audioEditorPointer (to[0], to[1], false);
    };
    const auto clickName = [&host, &ctx] (TakeId take)
    { ctx.expect (host.clickAudioEditorButton (takeControl ("name", take)), "the take's name is not clickable"); };
    const auto unchanged = [&ctx, &engine, &track, regionsBefore] (const std::string& what)
    {
        ctx.expect (sameRegions (track.regions, regionsBefore), what + " changed the regions");
        ctx.expect (! engine.getUndoManager().canUndo(), what + " recorded an undo step");
    };
    const std::string lockNotice = "A locked region is in the way. Unlock it to use this part of the take.";
    const std::string frozenNotice = "Unfreeze this track to change its takes.";

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, e = *empty] { host.revealAudioEditorTake (e); },
                        [&host] { return host.audioEditorTakeLanes().size() == 3; }, "the take lanes never showed" });
    steps->push_back ({ 100, [clickName, e = *empty] { clickName (e); },
                        [&host, e = *empty] { return host.audioEditorTakePoint ("name", e, 0).size() == 2; },
                        "the empty take's name never came into view" });
    steps->push_back ({ 900, [&host, &ctx, unchanged, other]
    {
        ctx.expect (host.audioEditorTakeNotice().empty(),
                    "a promote with nothing to place reads '" + host.audioEditorTakeNotice() + "'");
        unchanged ("promoting a take with no length");
        for (auto& region : ctx.session().track (0).regions) region.locked = true;
        host.revealAudioEditorTake (other);
    } });
    steps->push_back ({ 100, dragAcross,
                        [&host, other] { return host.audioEditorTakePoint ("name", other, 0).size() == 2; },
                        "the second take's lane never came into view" });
    steps->push_back ({ 100, [&track, unchanged]
    {
        unchanged ("a promote over a locked region");
        for (auto& region : track.regions) region.locked = false;
        track.frozen.store (true);
    }, [&host, lockNotice] { return host.audioEditorTakeNotice() == lockNotice; },
       "a promote over a locked region did not say the lock is in the way" });
    steps->push_back ({ 100, [clickName, other] { clickName (other); } });
    steps->push_back ({ 900, [&host, &ctx, unchanged, frozenNotice]
    {
        ctx.expect (host.audioEditorTakeNotice() == frozenNotice,
                    "a promote on a frozen track reads '" + host.audioEditorTakeNotice() + "'");
        unchanged ("a promote on a frozen track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioTakeRefusalNotices { Scenario {
    "gui.audio_take_refusal_notices", { "gui", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioTakeRefusalNotices (host, ctx); }
} };

// A modal centred over the editor's native child would open behind it, unseen and
// unclickable, so an alert or panel raised while the editor is up closes the editor
// first, and the audition it started ends with it.
std::optional<ScenarioResult> runAudioEditorStepsAsideForModals (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto newest = ids->back();
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto lanesShown = [&host] { return host.audioEditorOpen() && host.audioEditorTakeLanes().size() == 2; };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, newest]
    { ctx.expect (host.clickAudioEditorButton (takeControl ("audition", newest)), "no solo on the take's lane"); },
    lanesShown, "the take lanes never showed" });
    steps->push_back ({ 100, [&host] { host.raiseAlert ("Audio device lost", "The audio device went away."); },
                        [&session, newest] { return session.takeAudition.takeId == newest; }, "the solo did not solo the take" });
    steps->push_back ({ 100, [&host, &ctx, &session]
    {
        ctx.expect (host.modalText().rfind ("Audio device lost", 0) == 0, "'" + host.modalText() + "' is up rather than the alert");
        ctx.expect (session.takeAudition.trackIdx == -1, "closing the editor for the alert left the audition set");
        ctx.expect (host.clickModalButton ("OK"), "the alert has no OK button");
    }, [&host] { return ! host.audioEditorOpen(); }, "the alert opened with the editor still up" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioEditorOnTakes (0), "the editor did not open again"); },
    [&host] { return host.modalStackEmpty(); }, "OK did not close the alert" });
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("shift + /", '?'); }, lanesShown, "the take lanes never showed again" });
    steps->push_back ({ 100, [&host] { host.closeTopModal(); },
                        [&host] { return ! host.audioEditorOpen() && host.shortcutsOpen(); },
                        "Keyboard Shortcuts opened with the editor still up" });
    steps->push_back ({ 100, [] {}, [&host] { return host.modalStackEmpty(); }, "Keyboard Shortcuts did not close" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorStepsAsideForModals { Scenario {
    "gui.audio_editor_steps_aside_for_modals", { "gui", "editor", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorStepsAsideForModals (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
