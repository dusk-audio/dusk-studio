#include "../GuiScenarioSupport.h"
#include "../../AppConfig.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../engine/audiofile/FileReader.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
std::optional<ScenarioResult> runAudioAutomationGestures (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalPlayhead = engine.getTransport().getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, originalPlayhead, restore]
    {
        engine.stop();
        drainModals (host);
        host.closeAudioEditor();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getTransport().locate (originalPlayhead);
    });
    const auto source = ctx.tempDir() / "Automation.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    std::vector<float> signal (48000, 0.5f);
    const float* channels[] = { signal.data() };
    if (! writer || ! writer->write (channels, 1, 48000) || ! writer->flush())
        return ScenarioResult::fail ("could not write editor fixture");
    writer.reset();
    const auto read = [] (const std::filesystem::path& path)
    {
        std::ifstream input (path, std::ios::binary);
        return std::string (std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>());
    };
    const auto originalBytes = read (source);
    auto& track = session.track (0);
    track.frozen.store (false);
    track.regions.clear();
    AudioRegion region;
    region.file = decltype (region.file) (source.string());
    region.lengthInSamples = 48000;
    track.regions.push_back (region);
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");
    auto& lane = track.automationLanes[(size_t) AutomationParam::FaderDb];
    lane.publishPoints ({});
    track.automationMode.store ((int) AutomationMode::Off);
    const auto pointer = [&host, &ctx] (std::int64_t sample, float value, bool down, int modifiers = 0)
    {
        const auto point = host.audioAutomationPoint (sample, value);
        if (ctx.expect (point.size() == 2, "automation point geometry unavailable"))
            ctx.expect (host.audioEditorPointer (point[0], point[1], down, modifiers), "automation pointer failed");
    };
    const auto onePoint = [&ctx, &lane] (std::int64_t time, float value)
    {
        const auto& points = lane.pointsConst();
        if (ctx.expect (points.size() == 1, "expected one automation point"))
            ctx.expect (std::abs (points[0].timeSamples - time) < 128 && std::abs (points[0].value - value) < 0.01f,
                        "automation point time/value differs from gesture");
    };
    const auto stroke = [pointer]
    {
        pointer (6000, 0.2f, true);
        pointer (12000, 0.5f, true);
        pointer (18000, 0.8f, true);
        pointer (24000, 0.3f, true);
        pointer (24000, 0.3f, false);
    };
    auto drawn = std::make_shared<std::vector<AutomationPoint>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickAudioEditorButton ("Auto: Off"), "automation picker unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Fader (dB)"), "fader lane unavailable"); } });
    steps->push_back ({ 150, [pointer] { pointer (12000, 0.25f, true); pointer (12000, 0.25f, false); } });
    steps->push_back ({ 150, [onePoint, &track, &ctx, pointer]
    {
        onePoint (12000, 0.25f);
        ctx.expect (track.automationMode.load() == (int) AutomationMode::Read, "adding a point did not arm READ");
        pointer (12000, 0.25f, true); pointer (24000, 0.75f, true); pointer (24000, 0.75f, false);
    } });
    steps->push_back ({ 150, [onePoint, pointer]
    { onePoint (24000, 0.75f); pointer (24000, 0.75f, true, 4); pointer (24000, 0.75f, false, 4); } });
    steps->push_back ({ 150, [&lane, &ctx, &host, &track]
    {
        ctx.expect (lane.pointsConst().empty(), "right-click did not delete point");
        track.automationMode.store ((int) AutomationMode::Write);
        ctx.expect (host.clickAudioEditorButton ("Draw"), "Draw toolbar button unavailable");
    } });
    steps->push_back ({ 150, [stroke] { stroke(); } });
    steps->push_back ({ 150, [&lane, &ctx, &track, &host, drawn]
    {
        *drawn = lane.pointsConst();
        if (ctx.expect (drawn->size() >= 3, "freehand stroke did not retain its shape"))
        {
            ctx.expect (std::abs (drawn->front().timeSamples - 6000) < 128
                        && std::abs (drawn->front().value - 0.2f) < 0.01f
                        && std::abs (drawn->back().timeSamples - 24000) < 128
                        && std::abs (drawn->back().value - 0.3f) < 0.01f, "stroke endpoints differ from gesture");
            ctx.expect (std::is_sorted (drawn->begin(), drawn->end(), [] (const auto& a, const auto& b)
                        { return a.timeSamples < b.timeSamples; }), "stroke points are not sorted");
            ctx.expect (std::any_of (drawn->begin(), drawn->end(), [] (const auto& point)
                        { return point.value > 0.78f; }), "stroke lost its peak");
        }
        ctx.expect (track.automationMode.load() == (int) AutomationMode::Read, "drawing did not replace WRITE with READ");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "stroke Undo unavailable");
    } });
    steps->push_back ({ 150, [&lane, &ctx, &host]
    {
        ctx.expect (lane.pointsConst().empty(), "stroke was not one undo step");
        ctx.expect (host.clickAudioEditorButton ("Redo"), "stroke Redo unavailable");
    } });
    steps->push_back ({ 150, [&lane, &ctx, &engine, drawn]
    {
        ctx.expect (lane.pointsConst() == *drawn, "Redo did not restore stroke");
        engine.play();
    } });
    steps->push_back ({ 200, [&engine, &ctx, pointer]
    {
        ctx.expect (! engine.getTransport().isStopped(), "transport did not start for editing guard");
        pointer (30000, 0.9f, true);
        pointer (36000, 0.1f, true);
        pointer (36000, 0.1f, false);
    } });
    steps->push_back ({ 150, [&lane, &track, &ctx, &engine, drawn]
    {
        ctx.expect (lane.pointsConst() == *drawn, "gesture begun during playback edited automation");
        ctx.expect (track.regions.size() == 1 && track.regions[0].timelineStart == 0
                    && track.regions[0].lengthInSamples == 48000, "blocked drawing changed region");
        engine.stop();
    } });
    steps->push_back ({ 150, [&engine, &ctx, read, source, originalBytes]
    {
        ctx.expect (engine.getTransport().isStopped(), "transport did not stop");
        ctx.expect (read (source) == originalBytes, "automation modified source audio");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioAutomationGestures { Scenario {
    "gui.audio_automation_gestures", { "gui", "automation" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioAutomationGestures (host, ctx); }
} };

std::optional<ScenarioResult> runMiniTimelineMarkers (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the mini timeline");
    for (int track = 0; track < Session::kNumTracks; ++track)
        if (! session.track (track).regions.empty() || ! session.track (track).midiRegions.current().empty())
            return ScenarioResult::skip ("requires an empty arrangement");
    const auto markers = session.getMarkers();
    const auto playhead = transport.getPlayhead();
    const bool shown = host.setTimelineShown (false);
    ctx.cleanup ([&host, &session, &transport, markers, playhead, shown]
    {
        host.captureMiniMarkers (false);
        session.getMarkers() = markers;
        transport.locate (playhead);
        host.setTimelineShown (shown);
    });
    const double rate = engine.getCurrentSampleRate();
    session.getMarkers().clear();
    const std::array<const char*, 3> names { "Verse", "Chorus", "Outro" };
    for (int index = 0; index < 3; ++index)
    {
        Marker marker;
        marker.name = names[(size_t) index];
        marker.timelineSamples = (std::int64_t) ((10 + index * 15) * rate);
        marker.colour = decltype (marker.colour) (0xff906030);
        session.getMarkers().push_back (std::move (marker));
    }
    transport.locate (0);
    if (! host.captureMiniMarkers (true)) return ScenarioResult::fail ("mini timeline is unavailable");
    auto baseline = std::make_shared<std::vector<MiniMarkerPaint>>();
    const auto check = [&ctx, &host, baseline, names] (int active)
    {
        const auto rows = host.miniMarkerPaint();
        if (! ctx.expect (rows.size() == 3 && baseline->size() == 3, "mini timeline did not paint every marker")) return;
        const auto brightness = [] (std::uint32_t colour)
        { return ((colour >> 16) & 255) + ((colour >> 8) & 255) + (colour & 255); };
        for (int index = 0; index < 3; ++index)
        {
            const auto& row = rows[(size_t) index];
            const auto& dim = (*baseline)[(size_t) index];
            ctx.expect (row.name == names[(size_t) index] && row.labelWidth > 0, "marker label was not painted");
            if (index == active)
            {
                ctx.expect (brightness (row.tickColour) > brightness (dim.tickColour)
                            && brightness (row.labelColour) > brightness (dim.labelColour), "current section was not brightened");
                ctx.expect (row.tickHeight > dim.tickHeight && row.tickWidth > dim.tickWidth,
                            "current marker tick was not emphasized");
            }
            else
                ctx.expect (row.tickColour == dim.tickColour && row.labelColour == dim.labelColour
                            && std::abs (row.tickHeight - dim.tickHeight) < 0.001f
                            && std::abs (row.tickWidth - dim.tickWidth) < 0.001f, "inactive marker was highlighted");
        }
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 350, [&host, &ctx, rate]
    { ctx.expect (host.clickMiniSample ((std::int64_t) (5 * rate)), "mini timeline seek failed"); } });
    steps->push_back ({ 200, [&host, &ctx, &transport, rate, baseline, check]
    {
        ctx.expect (transport.getPlayhead() > 0 && transport.getPlayhead() < (std::int64_t) (10 * rate),
                    "mini timeline did not seek before the first marker");
        *baseline = host.miniMarkerPaint();
        check (-1);
    } });
    for (int index = 0; index < 3; ++index)
    {
        steps->push_back ({ 150, [&host, &ctx, rate, index]
        { ctx.expect (host.clickMiniSample ((std::int64_t) ((20 + index * 15) * rate)), "section seek failed"); } });
        steps->push_back ({ 200, [check, index] { check (index); } });
    }
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickMiniMarker (0), "visible marker label did not receive a click"); } });
    steps->push_back ({ 200, [&ctx, &session, &transport, check]
    {
        ctx.expect (transport.getPlayhead() == session.getMarkers()[0].timelineSamples, "marker label did not seek exactly to the marker");
        check (0);
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar miniTimelineMarkers { Scenario {
    "gui.mini_timeline_markers", { "gui", "tape" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMiniTimelineMarkers (host, ctx); }
} };

std::optional<ScenarioResult> runTapeRuler (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    if (! host.modalStackEmpty() || ! session.getMarkers().empty() || session.tempoMap.points().size() > 1)
        return ScenarioResult::skip ("requires an unobstructed ruler");
    const std::array<std::int64_t, 6> saved { transport.getPlayhead(), transport.getLoopStart(), transport.getLoopEnd(),
        transport.getPunchIn(), transport.getPunchOut(), session.lastClickedTimelineSample.load() };
    const bool loop = transport.isLoopEnabled(), punch = transport.isPunchEnabled(), snap = session.snapToGrid;
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, &transport, saved, loop, punch, snap, shown]
    {
        host.tapeRulerPointer (0.18f, false);
        drainModals (host);
        transport.setLoopRange (saved[1], saved[2]);
        transport.setLoopEnabled (loop);
        transport.setPunchRange (saved[3], saved[4]);
        transport.setPunchEnabled (punch);
        transport.locate (saved[0]);
        session.lastClickedTimelineSample.store (saved[5]);
        session.snapToGrid = snap;
        host.setTimelineShown (shown);
    });
    transport.placeLoopRange (0, 0);
    transport.placePunchRange (0, 0);
    session.snapToGrid = false;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.tapeRulerPointer (0.18f, true), "ruler click failed");
        host.tapeRulerPointer (0.18f, false);
    } });
    steps->push_back ({ 150, [&host, &ctx, &transport, &session]
    {
        const auto expected = host.tapeRulerSample (0.18f);
        ctx.expect (expected > 0 && transport.getPlayhead() == expected, "ruler click did not seek");
        ctx.expect (session.lastClickedTimelineSample.load() == expected, "ruler click did not remember the stop-return point");
    } });
    const auto drag = [&host, &ctx, steps] (float from, float to, bool shift)
    {
        steps->push_back ({ 150, [&host, &ctx, from, shift]
        { ctx.expect (host.tapeRulerPointer (from, true, shift), "ruler drag did not start"); } });
        steps->push_back ({ 150, [&host, to, shift] { host.tapeRulerPointer (to, true, shift); } });
        steps->push_back ({ 150, [&host, to, shift] { host.tapeRulerPointer (to, false, shift); } });
    };
    drag (0.3f, 0.5f, false);
    steps->push_back ({ 150, [&host, &ctx, &transport]
    {
        ctx.expect (! transport.isLoopEnabled() && transport.getLoopStart() == 0 && transport.getLoopEnd() == 0,
                    "drag committed a loop before the menu choice");
        ctx.expect (host.clickContextMenuItem ("Set loop here"), "range menu lacks Set loop here");
    } });
    const auto checkLoop = [&host, &ctx, &transport]
    {
        ctx.expect (transport.isLoopEnabled() && transport.getLoopStart() == host.tapeRulerSample (0.3f)
                    && transport.getLoopEnd() == host.tapeRulerSample (0.5f), "loop does not match the dragged range");
    };
    steps->push_back ({ 150, checkLoop });
    drag (0.8f, 0.65f, true);
    steps->push_back ({ 150, [&host, &ctx, checkLoop]
    {
        checkLoop();
        ctx.expect (host.clickContextMenuItem ("Cancel"), "range menu lacks Cancel");
    } });
    steps->push_back ({ 150, checkLoop });
    drag (0.55f, 0.7f, false);
    steps->push_back ({ 150, [&host, &ctx, &transport]
    {
        ctx.expect (! transport.isPunchEnabled(), "drag committed punch before the menu choice");
        ctx.expect (host.clickContextMenuItem ("Set punch in / out here"), "range menu lacks punch placement");
    } });
    steps->push_back ({ 150, [&host, &ctx, &transport, checkLoop]
    {
        checkLoop();
        ctx.expect (transport.isPunchEnabled() && transport.getPunchIn() == host.tapeRulerSample (0.55f)
                    && transport.getPunchOut() == host.tapeRulerSample (0.7f), "punch does not match the dragged range");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tapeRuler { Scenario {
    "gui.tape_ruler", { "gui", "tape" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTapeRuler (host, ctx); }
} };

std::optional<ScenarioResult> runTimelineDrawer (GuiHost& host, ScenarioContext& ctx)
{
    const bool expanded = host.timelineViewMatches (true);
    const auto launchScale = static_cast<float> (host.uiScale());
    ctx.cleanup ([&host, expanded, launchScale]
    {
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
        host.restoreUiScale (launchScale);
    });
    ctx.expect (host.timelineViewMatches (false), "the timeline is not collapsed at launch");
    const auto anyCompact = [&host]
    {
        for (int track = 0; track < Session::kNumTracks; ++track)
            if (host.stripCompact (track)) return true;
        return false;
    };
    const auto toggle = [&host, &ctx]
    {
        auto steps = std::make_shared<std::vector<Step>>();
        for (const bool show : { true, false, true, false })
        {
            steps->push_back ({ 0, [&host, &ctx, show]
            {
                ctx.expect (show ? host.pressKey ("T", 't') : host.pressKey ("command + \\", '\\'),
                            "the timeline shortcut was not handled");
            } });
            steps->push_back ({ 100, [&host, &ctx, show]
            {
                ctx.expect (host.timelineViewMatches (show), "the timeline visibility did not follow its toggle");
                for (int track = 0; track < Session::kNumTracks; ++track)
                    ctx.expect (host.stripCompact (track) == show,
                                "timeline expansion left the wrong layout on strip " + std::to_string (track + 1));
                ctx.expect (host.grMetersShown(), "a GR meter beside a fader is hidden or has lost its threshold handle");
            } });
        }
        runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    };
    if (! anyCompact())
    {
        toggle();
        return std::nullopt;
    }
    // A window too short for full-height strips compacts them on its own, which
    // would hide the timeline doing it. At the smallest UI scale the same window
    // is twice as tall in interface units.
    host.restoreUiScale (appconfig::kUiScaleMin);
    ctx.later (1000, [&ctx, anyCompact, toggle]
    {
        if (anyCompact())
        {
            ctx.complete (ScenarioResult::skip ("the window is too short for full-height strips even at the "
                                                "smallest UI scale, so the timeline cannot be seen compacting them"));
            return;
        }
        toggle();
    });
    return std::nullopt;
}

const ScenarioRegistrar timelineDrawer { Scenario {
    "gui.timeline_drawer", { "gui", "timeline" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineDrawer (host, ctx); }
} };

std::optional<ScenarioResult> runTapeNudgeKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (0);
    const bool expanded = host.timelineViewMatches (true);
    ctx.keep (session.tempoBpm);
    ctx.keep (session.beatsPerBar);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup (host.preserveKeyboardFocus());
    ctx.cleanup ([&host, &ctx, &track, expanded, regions = track.regions, mode = session.editMode]
    {
        track.regions = regions;
        ctx.session().editMode = mode;
        ctx.engine().getUndoManager().clearUndoHistory();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    session.tempoBpm.store (120.0f);
    session.beatsPerBar.store (3);
    session.editMode = EditMode::Grab;
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    const auto beat = static_cast<std::int64_t> (std::llround (ctx.engine().getCurrentSampleRate() / 2.0));
    const auto path = ctx.tempDir() / "nudge.wav";
    auto writer = dusk::audio::FileWriter::create (path, { ctx.engine().getCurrentSampleRate(), 1, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the nudge fixture")) return ctx.verdict();
    std::vector<float> silence (static_cast<std::size_t> (beat * 2), 0.0f);
    const float* channels[] { silence.data() };
    if (! ctx.expect (writer->write (channels, 1, beat * 2), "could not write the nudge fixture")) return ctx.verdict();
    writer.reset();
    AudioRegion first;
    first.file = decltype (first.file) (path.u8string().c_str());
    first.timelineStart = beat * 4;
    first.sourceOffset = beat / 2;
    first.lengthInSamples = beat;
    auto second = first;
    second.timelineStart = beat * 12;
    track.regions = { first, second };
    ctx.engine().getUndoManager().clearUndoHistory();
    if (! expanded) host.pressKey ("T", 't');
    auto steps = std::make_shared<std::vector<Step>>();
    struct Nudge { const char* key; int beatsFromStart; };
    for (const auto nudge : { Nudge { "command + cursor right", 1 }, Nudge { "command + cursor left", 0 },
                              Nudge { "command + shift + cursor right", 3 }, Nudge { "command + shift + cursor left", 0 } })
    {
        steps->push_back ({ 500, [&host, &ctx]
        { ctx.expect (host.clickAudioRegion (0, 0), "could not select the region with a timeline click"); } });
        steps->push_back ({ 100, [&host, &ctx, nudge]
        { ctx.expect (host.pressKey (nudge.key), "the region nudge shortcut was not handled"); } });
        steps->push_back ({ 100, [&ctx, &track, first, second, beat, nudge]
        {
            if (ctx.expect (track.regions.size() == 2, "nudge changed the region count"))
                ctx.expect (track.regions[0].timelineStart == first.timelineStart + beat * nudge.beatsFromStart
                            && track.regions[0].sourceOffset == first.sourceOffset
                            && track.regions[0].lengthInSamples == first.lengthInSamples
                            && track.regions[1].timelineStart == second.timelineStart,
                            "nudge did not move only the selected region by the requested beat or bar");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tapeNudgeKeys { Scenario {
    "gui.tape_nudge_keys", { "gui", "keyboard", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTapeNudgeKeys (host, ctx); }
} };

int regionStartingAt (const Track& track, std::int64_t start)
{
    for (std::size_t i = 0; i < track.regions.size(); ++i)
        if (track.regions[i].timelineStart == start) return static_cast<int> (i);
    return -1;
}

// Cmd/Ctrl+D puts a copy of the selected region straight after it, and
// Cmd/Ctrl+E splits the selected region at the playhead, both through the
// window's own key handling.
std::optional<ScenarioResult> runRegionEditKeys (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion original;
    if (auto early = seedTapeRegion (host, ctx, original)) return early;
    auto& track = ctx.session().track (0);
    auto& transport = ctx.engine().getTransport();
    const auto end = original.timelineStart + original.lengthInSamples;
    const auto cut = original.timelineStart + original.lengthInSamples / 4;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0), "could not select the region with a timeline click"); } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("command + D", 'd'), "Cmd+D was not handled"); } });
    steps->push_back ({ 100, [&ctx, &track, original, end]
    {
        const int copy = regionStartingAt (track, end);
        if (! ctx.expect (track.regions.size() == 2 && copy >= 0, "Cmd+D did not add a region right after the original"))
            return;
        const auto& placed = track.regions[(std::size_t) copy];
        ctx.expect (placed.lengthInSamples == original.lengthInSamples && placed.sourceOffset == original.sourceOffset
                    && placed.file == original.file, "Cmd+D did not place a copy of the original");
    } });
    // Adding a region clears the selection, so Cmd+E has nothing to split yet.
    steps->push_back ({ 100, [&host, &transport, cut]
    {
        transport.locate (cut);
        host.pressPeerKey ("command + E", 'e');
    } });
    // Past the double-click window, so the second click selects rather than
    // opening the region editor.
    steps->push_back ({ 700, [&host, &ctx, &track, &transport, original, cut]
    {
        ctx.expect (track.regions.size() == 2, "Cmd+E split a region the duplicate left unselected");
        ctx.expect (host.clickAudioRegion (0, regionStartingAt (track, original.timelineStart)),
                    "could not reselect the original region");
        transport.locate (cut);
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("command + E", 'e'), "Cmd+E was not handled"); } });
    steps->push_back ({ 100, [&ctx, &track, original, end, cut]
    {
        const int head = regionStartingAt (track, original.timelineStart);
        const int tail = regionStartingAt (track, cut);
        if (! ctx.expect (track.regions.size() == 3 && head >= 0 && tail >= 0 && regionStartingAt (track, end) >= 0,
                          "Cmd+E did not split the selected region at the playhead"))
            return;
        ctx.expect (track.regions[(std::size_t) head].lengthInSamples == cut - original.timelineStart
                    && track.regions[(std::size_t) tail].lengthInSamples == end - cut
                    && track.regions[(std::size_t) tail].sourceOffset
                           == original.sourceOffset + (cut - original.timelineStart),
                    "Cmd+E split the region somewhere other than the playhead");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar regionEditKeys { Scenario {
    "gui.region_split_duplicate_keys", { "gui", "keyboard", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRegionEditKeys (host, ctx); }
} };

// The region right-click menu: Loop region, the label prompt, Reverse region,
// a Color swatch, and Lock / Unlock region, each on the region it opened over.
std::optional<ScenarioResult> runRegionMenuItems (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion original;
    if (auto early = seedTapeRegion (host, ctx, original)) return early;
    auto& track = ctx.session().track (0);
    auto& transport = ctx.engine().getTransport();
    const auto end = original.timelineStart + original.lengthInSamples;
    const auto takesDir = ctx.tempDir() / "session" / "takes";
    auto steps = std::make_shared<std::vector<Step>>();
    const auto pick = [&host, &ctx, steps] (std::vector<std::string> path)
    {
        steps->push_back ({ 300, [&host, &ctx]
        { ctx.expect (host.clickAudioRegion (0, 0, true), "the region did not take a right-click"); } });
        for (const auto& item : path)
            steps->push_back ({ 200, [&host, &ctx, item]
            { ctx.expect (host.clickContextMenuItem (item), "region menu item is unavailable: " + item); } });
    };
    const auto check = [&ctx, &track, steps] (std::function<bool (const AudioRegion&)> holds, std::string failure)
    {
        steps->push_back ({ 300, [&ctx, &track, holds, failure]
        { ctx.expect (track.regions.size() == 1 && holds (track.regions.front()), failure); } });
    };

    pick ({ "Loop region" });
    steps->push_back ({ 300, [&ctx, &transport, original, end]
    {
        ctx.expect (transport.isLoopEnabled() && transport.getLoopStart() == original.timelineStart
                    && transport.getLoopEnd() == end && transport.getPlayhead() == original.timelineStart,
                    "Loop region did not loop the region's span from its start");
    } });

    pick ({ "Add label..." });
    steps->push_back ({ 300, [&host, &ctx]
    {
        if (! ctx.expect (host.focusModalTextInput(), "Add label did not open a text input")) return;
        for (const char character : std::string ("verse"))
            ctx.expect (host.pressPeerKey (std::string (1, character), character), "the label input rejected a character");
        ctx.expect (host.pressPeerKey ("return"), "the label input did not accept Return");
    } });
    check ([] (const AudioRegion& r) { return r.label == "verse"; }, "the label prompt did not name the region");

    pick ({ "Reverse region" });
    steps->push_back ({ 300, [&ctx, &track, original, takesDir]
    {
        if (! ctx.expect (track.regions.size() == 1, "Reverse region changed the region count")) return;
        const auto& reversed = track.regions.front();
        const std::filesystem::path file (reversed.file.getFullPathName().toStdString());
        if (! ctx.expect (reversed.file != original.file && file.parent_path() == takesDir
                          && reversed.sourceOffset == 0 && reversed.lengthInSamples == original.lengthInSamples,
                          "Reverse region did not point the region at a rendered copy in takes/")) return;
        auto reader = dusk::audio::FileReader::open (file);
        if (! ctx.expect (reader != nullptr, "the reversed take is not readable")) return;
        std::array<float, 1> first {};
        float* destination[] { first.data() };
        const auto rampFrames = static_cast<float> (original.lengthInSamples * 2);
        const float lastOfSlice = -0.5f + static_cast<float> (original.sourceOffset + original.lengthInSamples - 1) / rampFrames;
        ctx.expect (reader->read (destination, 1, 0, 1) == 1 && std::abs (first[0] - lastOfSlice) < 1.0e-4f,
                    "the reversed take does not open on the region's last sample");
    } });

    pick ({ "Color", "Blue" });
    check ([] (const AudioRegion& r) { return r.customColour.getARGB() == 0xff6090d0; },
           "the Color submenu did not tint the region");

    pick ({ "Lock region" });
    check ([] (const AudioRegion& r) { return r.locked; }, "Lock region did not lock the region");
    pick ({ "Unlock region" });
    check ([] (const AudioRegion& r) { return ! r.locked; }, "Unlock region did not unlock the region");

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar regionMenuItems { Scenario {
    "gui.region_menu_items", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRegionMenuItems (host, ctx); }
} };

// A locked region's right-click menu offers neither Split at playhead nor Delete
// region, and Join selected regions is unavailable while a selected region is
// locked; the track keeps its regions and no undo step is recorded.
std::optional<ScenarioResult> runRegionMenuLockedRefuses (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion original;
    if (auto early = seedTapeRegion (host, ctx, original)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    auto neighbour = original;
    neighbour.timelineStart = original.timelineStart + original.lengthInSamples;
    track.regions.push_back (neighbour);
    track.regions.front().locked = true;
    host.pressKey ("0", '0');
    const auto before = track.regions;
    const auto playhead = original.timelineStart + original.lengthInSamples / 2;

    const auto untouched = [&ctx, &engine, &track, before] (const std::string& what)
    {
        ctx.expect (sameRegions (track.regions, before) && track.regions.front().locked,
                    what + " changed the track's regions");
        ctx.expect (! engine.getUndoManager().canUndo(), what + " recorded an undo step");
    };
    const auto refused = [&host, &ctx] (const char* item, const std::string& when)
    { ctx.expect (! host.contextMenuItemEnabled (item), std::string (item) + " is offered " + when); };
    const auto closeMenu = [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Loop region"), "the region menu did not open"); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 500, [&host, &ctx, &transport = engine.getTransport(), playhead]
    {
        transport.locate (playhead);
        ctx.expect (host.clickAudioRegion (0, 0, true), "the locked region did not take a right-click");
    } });
    steps->push_back ({ 200, [refused, closeMenu]
    {
        refused ("Split at playhead", "on a locked region");
        refused ("Delete region", "on a locked region");
        closeMenu();
    } });
    steps->push_back ({ 300, [&host, &ctx, untouched]
    {
        untouched ("the locked region's menu");
        ctx.expect (host.clickAudioRegion (0, 0), "could not select the locked region");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 1, false, 2), "the neighbour did not take a Cmd-click"); } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 1, true), "the neighbour did not take a right-click"); } });
    steps->push_back ({ 200, [&host, &ctx, refused, closeMenu]
    {
        ctx.expect (host.contextMenuItemEnabled ("Split at playhead") || host.contextMenuItemEnabled ("Delete region"),
                    "the neighbour's menu offers neither Split at playhead nor Delete region");
        refused ("Join selected regions", "with a locked region selected");
        closeMenu();
    } });
    steps->push_back ({ 300, [untouched] { untouched ("Join selected regions"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar regionMenuLockedRefuses { Scenario {
    "gui.region_menu_locked_refuses", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRegionMenuLockedRefuses (host, ctx); }
} };

// Alt+T and Alt+Shift+T step the selected MIDI region round its take ring, key
// after key, undo and redo drop the selection they step, and the Takes submenu
// brings a chosen take live. An audio region has neither: the keys leave it
// alone and its menu has no Takes.
std::optional<ScenarioResult> runRegionTakeControls (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion live;
    if (auto early = seedTapeRegion (host, ctx, live)) return early;
    auto& session = ctx.session();
    auto& audioTrack = session.track (0);
    const auto liveFile = live.file.getFullPathName().toStdString();

    auto& midiTrack = session.track (1);
    midiTrack.frozen.store (false);
    midiTrack.regions.clear();
    midiTrack.mode.store ((int) Track::Mode::Midi);
    const auto takeOf = [] (int pitch)
    {
        MidiTakeRef take;
        take.lengthInTicks = 3840;
        take.notes = { { 1, pitch, 100, 0, 480 } };
        return take;
    };
    MidiRegion midi;
    midi.timelineStart = live.timelineStart;
    midi.lengthInTicks = 3840;
    midi.lengthInSamples = live.lengthInSamples;
    midi.notes = takeOf (60).notes;
    midi.previousTakes = { takeOf (62), takeOf (64) };
    midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { midi }));
    const auto livePitch = [&midiTrack]
    {
        const auto& regions = midiTrack.midiRegions.current();
        return regions.size() == 1 && regions.front().notes.size() == 1 ? regions.front().notes.front().noteNumber : -1;
    };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto expectPitch = [&ctx, livePitch, steps] (int pitch, std::string failure)
    {
        steps->push_back ({ 200, [&ctx, livePitch, pitch, failure] { ctx.expect (livePitch() == pitch, failure); } });
    };
    // One click: a take step keeps the region selected for the next key.
    const auto press = [&host, &ctx, steps] (std::string key, char text)
    {
        steps->push_back ({ 100, [&host, &ctx, key, text]
        { ctx.expect (host.pressPeerKey (key, text), key + " was not handled"); } });
    };
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.clickMidiRegion (1, 0), "could not select the MIDI region with a timeline click"); } });
    press ("alt + T", 't');
    expectPitch (62, "Alt+T did not bring the next MIDI take live");
    press ("alt + shift + T", 'T');
    expectPitch (60, "Alt+Shift+T did not step back to the previous MIDI take");
    press ("alt + shift + T", 'T');
    expectPitch (64, "Alt+Shift+T did not wrap to the last MIDI take");
    steps->push_back ({ 100, [&ctx] { ctx.engine().getUndoManager().undo(); } });
    expectPitch (60, "undo did not take back the last step");
    // Undo may have moved regions, so it drops the selection: Alt+T has
    // nothing to step until the region is clicked again.
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("alt + T", 't'); } });
    expectPitch (60, "Alt+T stepped a region the undo left unselected");
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickMidiRegion (1, 0), "could not select the MIDI region with a timeline click"); } });
    steps->push_back ({ 100, [&ctx] { ctx.engine().getUndoManager().redo(); } });
    expectPitch (64, "redo did not bring the step back");
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("alt + T", 't'); } });
    expectPitch (64, "Alt+T stepped a region the redo left unselected");
    auto secondTake = std::make_shared<int> (-1);
    steps->push_back ({ 700, [&host, &ctx, &midiTrack, secondTake]
    {
        const auto& regions = midiTrack.midiRegions.current();
        if (regions.size() == 1 && ! regions.front().previousTakes.empty()
            && regions.front().previousTakes.front().notes.size() == 1)
            *secondTake = regions.front().previousTakes.front().notes.front().noteNumber;
        ctx.expect (host.clickMidiRegion (1, 0, true), "the MIDI region did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Takes"), "the MIDI region menu has no Takes submenu"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Take 2"), "the Takes submenu has no Take 2"); } });
    steps->push_back ({ 200, [&ctx, livePitch, secondTake]
    { ctx.expect (*secondTake >= 0 && livePitch() == *secondTake, "Take 2 in the Takes submenu did not bring that take live"); } });

    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0), "could not select the audio region with a timeline click"); } });
    steps->push_back ({ 100, [&host, &ctx, &audioTrack, liveFile]
    {
        host.pressPeerKey ("alt + T", 't');
        host.pressPeerKey ("alt + shift + T", 'T');
        ctx.expect (audioTrack.regions.size() == 1
                        && audioTrack.regions.front().file.getFullPathName().toStdString() == liveFile,
                    "Alt+T or Alt+Shift+T changed the audio region");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0, true), "the audio region did not take a right-click"); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        const auto items = host.contextMenuItems();
        ctx.expect (! items.empty(), "the audio region menu did not open");
        ctx.expect (std::find (items.begin(), items.end(), "Takes") == items.end(), "the audio region menu has a Takes submenu");
        host.closeTopModal();
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar regionTakeControls { Scenario {
    "gui.region_take_controls", { "gui", "keyboard", "region", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRegionTakeControls (host, ctx); }
} };

// M drops a marker at the playhead and opens the naming prompt: typing names
// it, Escape keeps the default. The pill drags along the ruler, and its
// right-click menu renames and deletes it.
std::optional<ScenarioResult> runMarkerGestures (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion unused;
    if (auto early = seedTapeRegion (host, ctx, unused)) return early;
    auto& markers = ctx.session().getMarkers();
    auto& transport = ctx.engine().getTransport();
    const auto named = host.tapeRulerSample (0.3f);
    const auto dropped = host.tapeRulerSample (0.6f);
    const auto unnamed = host.tapeRulerSample (0.2f);
    if (named <= unnamed || dropped <= named) return ScenarioResult::fail ("tape ruler geometry unavailable");
    auto steps = std::make_shared<std::vector<Step>>();
    const auto type = [&host, &ctx] (const std::string& text)
    {
        for (const char character : text)
            ctx.expect (host.pressPeerKey (std::string (1, character), character), "the marker prompt rejected a character");
        ctx.expect (host.pressPeerKey ("return"), "the marker prompt did not accept Return");
    };
    steps->push_back ({ 300, [&host, &ctx, &transport, named]
    {
        transport.locate (named);
        ctx.expect (host.pressPeerKey ("M", 'm'), "M was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &markers, named, type]
    {
        ctx.expect (markers.size() == 1 && markers.front().timelineSamples == named, "M did not drop a marker at the playhead");
        // Typed straight in: the default name is pre-selected, so it is replaced.
        if (ctx.expect (! host.modalStackEmpty(), "M did not open the naming prompt")) type ("chorus");
    } });
    steps->push_back ({ 300, [&host, &ctx, &markers]
    {
        ctx.expect (host.modalStackEmpty() && markers.size() == 1 && markers.front().name == "chorus",
                    "typing in the naming prompt did not name the marker");
        ctx.expect (host.dragTapeMarker (0, 0.6f), "the marker pill did not take a drag");
    } });
    steps->push_back ({ 300, [&host, &ctx, &markers, dropped]
    {
        ctx.expect (markers.size() == 1 && markers.front().timelineSamples == dropped, "dragging the pill did not move the marker");
        ctx.expect (host.clickTapeMarker (0, true), "the marker pill did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Rename \"chorus\"..."), "the marker menu has no Rename item"); } });
    steps->push_back ({ 300, [&host, &ctx, type]
    {
        if (! ctx.expect (host.focusModalTextInput(), "Rename did not open a text input")) return;
        ctx.expect (host.pressPeerKey ("command + A"), "the rename prompt did not accept Select All");
        type ("bridge");
    } });
    steps->push_back ({ 300, [&host, &ctx, &markers, &transport, unnamed]
    {
        ctx.expect (markers.size() == 1 && markers.front().name == "bridge", "Rename did not rename the marker");
        transport.locate (unnamed);
        ctx.expect (host.pressPeerKey ("M", 'm'), "M was not handled");
    } });
    auto defaultName = std::make_shared<std::string>();
    steps->push_back ({ 300, [&host, &ctx, &markers, unnamed, defaultName]
    {
        if (! ctx.expect (markers.size() == 2 && markers.front().timelineSamples == unnamed,
                          "the second M did not drop a marker at the playhead")) return;
        *defaultName = markers.front().name.toStdString();
        ctx.expect (host.pressPeerKey ("escape"), "the naming prompt did not take Escape");
    } });
    steps->push_back ({ 300, [&host, &ctx, &markers, defaultName]
    {
        ctx.expect (host.modalStackEmpty() && markers.size() == 2 && ! defaultName->empty()
                    && markers.front().name.toStdString() == *defaultName,
                    "Escape did not keep the default marker name");
        ctx.expect (host.clickTapeMarker (1, true), "the marker pill did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Delete \"bridge\""), "the marker menu has no Delete item"); } });
    steps->push_back ({ 300, [&ctx, &markers, unnamed]
    {
        ctx.expect (markers.size() == 1 && markers.front().timelineSamples == unnamed,
                    "Delete did not remove only the right-clicked marker");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar markerGestures { Scenario {
    "gui.marker_key_prompt_and_pill", { "gui", "keyboard", "marker" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMarkerGestures (host, ctx); }
} };

// With no audio device running the timeline still lays the session out at the
// rate the device last ran at: the ruler reads the times it read with the device
// up, an audio region draws on its row, and a MIDI region is there to double-click.
std::optional<ScenarioResult> runTimelineWithoutDevice (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const double rate = engine.getCurrentSampleRate();
    if (host.engineDetached() || ! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    auto& midiTrack = session.track (0);
    auto& audioTrack = session.track (1);
    ctx.keep (midiTrack.mode);
    ctx.keep (midiTrack.frozen);
    ctx.keep (audioTrack.frozen);
    const auto view = host.tapeView();
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, &engine, &midiTrack, &audioTrack, timeline, view,
                  midi = midiTrack.midiRegions.current(), audio = audioTrack.regions]
    {
        host.closePianoRoll();
        if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback();
        midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (midi));
        audioTrack.regions = audio;
        host.setTimelineShown (timeline);
        host.restoreTapeView (view);
    });
    midiTrack.mode.store ((int) Track::Mode::Midi);
    midiTrack.frozen.store (false);
    audioTrack.frozen.store (false);
    MidiRegion midi;
    midi.timelineStart = (std::int64_t) std::llround (rate * 2.0);
    midi.lengthInTicks = 3840;
    midi.lengthInSamples = session.ticksToSamples (session.samplesToTicks (midi.timelineStart, rate) + midi.lengthInTicks, rate)
                         - midi.timelineStart;
    midi.notes = { { 1, 60, 100, 0, 480 } };
    midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    AudioRegion audio;
    audio.lengthInSamples = (std::int64_t) std::llround (rate * 4.0);
    audioTrack.regions.clear();
    audioTrack.regions.push_back (audio);

    const auto rulerAt = std::make_shared<std::int64_t> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("0", '0'), "the zoom-to-fit key was not handled"); } });
    steps->push_back ({ 200, [&host, &ctx, &engine, rulerAt]
    {
        *rulerAt = host.tapeRulerSample (0.5f);
        ctx.expect (*rulerAt > 0, "with the device up the ruler's middle reads sample 0");
        engine.detachAudioCallback();
        ctx.expect (engine.getCurrentSampleRate() <= 0.0, "a detached engine still reads a running rate");
    } });
    steps->push_back ({ 300, [&host, &ctx, rulerAt]
    {
        const auto now = host.tapeRulerSample (0.5f);
        ctx.expect (now == *rulerAt, "with no device running the ruler's middle reads sample " + std::to_string (now)
                                         + ", not the " + std::to_string (*rulerAt) + " it read with the device up");
        ctx.expect (host.tapeRegionShown (1, 0), "with no device running the tape strip does not draw track 2's region");
        ctx.expect (host.doubleClickMidiRegion (0, 0), "with no device running track 1's MIDI region is not on screen");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pianoRollOpen() && host.pianoRollRegion() == 0,
                    "double-clicking the MIDI region with no device running did not open it");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineWithoutDevice { Scenario {
    "gui.timeline_without_device", { "gui", "tape" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineWithoutDevice (host, ctx); }
} };

const ScenarioRegistrar markerKeysPlayback { Scenario {
    "gui.marker_keys_preserve_playback", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        auto& engine = ctx.engine();
        auto& transport = engine.getTransport();
        if (! transport.isStopped()) return ScenarioResult::skip ("requires a stopped fixture");
        const auto position = transport.getPlayhead();
        ctx.keep (ctx.session().lastRecordPointSamples);
        ctx.cleanup ([&engine, &transport, position]
        {
            engine.stop();
            transport.setPlayhead (position);
        });
        const auto& scenarios = allScenarios();
        const auto marker = std::find_if (scenarios.begin(), scenarios.end(), [] (const auto& scenario)
        { return scenario.name == "gui.marker_arrow_keys"; });
        if (marker == scenarios.end()) return ScenarioResult::fail ("marker scenario is not registered");
        engine.play();
        const auto result = marker->runGui (host, ctx);
        ctx.expect (result && result->status == ScenarioStatus::Skip,
                    "marker-key scenario did not decline a running transport");
        ctx.expect (transport.isPlaying(), "marker-key scenario stopped existing playback");
        return ctx.verdict();
    }
} };

const ScenarioRegistrar markerKeys { Scenario {
    "gui.marker_arrow_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        auto& session = ctx.session();
        auto& transport = ctx.engine().getTransport();
        if (! transport.isStopped())
            return ScenarioResult::skip ("requires a stopped transport");
        ctx.keep (session.lastRecordPointSamples);
        ctx.cleanup ([&session, &transport, markers = session.getMarkers(), at = transport.getPlayhead()]
        {
            session.getMarkers() = markers;
            transport.setPlayhead (at);
        });
        session.getMarkers().clear();
        for (auto at : { 48000, 96000, 144000 }) session.addMarker (at);
        transport.setPlayhead (120000);
        ctx.expect (host.pressKey ("shift + cursor left"), "Shift+Left was not handled");
        ctx.expect (transport.getPlayhead() == 96000, "Shift+Left did not reach the previous marker");
        ctx.expect (host.pressKey ("shift + cursor right"), "Shift+Right was not handled");
        ctx.expect (transport.getPlayhead() == 144000, "Shift+Right did not reach the next marker");
        session.getMarkers().clear();
        session.lastRecordPointSamples.store (72000);
        ctx.expect (host.pressKey ("shift + cursor left"), "empty-session Shift+Left was not handled");
        ctx.expect (transport.getPlayhead() == 0, "Shift+Left did not fall back to session start");
        ctx.expect (host.pressKey ("shift + cursor right"), "empty-session Shift+Right was not handled");
        ctx.expect (transport.getPlayhead() == 72000, "Shift+Right did not fall back to the last record point");
        return ctx.verdict();
    }
} };

std::optional<ScenarioResult> runEditKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires a stopped transport and no modal");
    const bool expanded = host.timelineViewMatches (true);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup (host.preserveKeyboardFocus());
    // Zoom and scroll survive the runner's reset, so an earlier case can leave
    // the fixture off-screen for the clicks below. Start from the unzoomed view
    // at the session start and put back whatever was there.
    const auto view = host.tapeView();
    ctx.cleanup ([&host, view] { host.restoreTapeView (view); });
    if (view.size() == 7)
    {
        auto start = view;
        start[0] = 1.0;
        start[1] = 0.0;
        start[2] = 0.0;
        host.restoreTapeView (start);
    }
    ctx.cleanup ([&host, &engine, &session, &track, expanded, regions = track.regions,
                  mode = session.editMode, at = transport.getPlayhead(),
                  clipboard = engine.getRegionClipboard()]
    {
        track.regions = regions;
        session.editMode = mode;
        engine.getRegionClipboard() = clipboard;
        engine.getUndoManager().clearUndoHistory();
        engine.getTransport().setPlayhead (at);
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    session.editMode = EditMode::Grab;
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    const auto rate = engine.getCurrentSampleRate();
    const auto second = (std::int64_t) std::llround (rate);
    const auto write = [rate] (const std::filesystem::path& path, std::int64_t frames)
    {
        auto writer = dusk::audio::FileWriter::create (path, { rate, 1, 24 });
        if (writer == nullptr) return false;
        std::vector<float> silence ((std::size_t) frames, 0.0f);
        const float* channels[] { silence.data() };
        return writer->write (channels, 1, frames) && writer->flush();
    };
    const auto firstTake = ctx.tempDir() / "edit-take-1.wav";
    if (! ctx.expect (write (firstTake, second * 6), "could not write the edit fixture"))
        return ctx.verdict();
    AudioRegion region;
    using File = std::decay_t<decltype (region.file)>;
    region.file = File (firstTake.u8string().c_str());
    region.timelineStart = second * 2;
    region.lengthInSamples = second * 4;
    track.regions = { region };
    engine.getUndoManager().clearUndoHistory();
    engine.getRegionClipboard() = {};
    if (! expanded) host.pressKey ("T", 't');

    // The keys as the display server sends them: X11 fills the key code with
    // the XLookupString glyph, so a Cmd/Ctrl chord carries a control character
    // as its text.
    const auto command = [&host] (char letter)
    { return host.pressKey ("command + " + keyCodeDescription (letter), controlCharacter (letter)); };
    // The tape strip drops every selection when the undo manager broadcasts, so
    // each operation that needs one gets its own click rather than chaining off
    // the last one's. Everything an operation asserts is read in the same step,
    // before that broadcast lands.
    const auto click = [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0), "could not select the region with a timeline click"); };
    const auto counted = [&ctx, &track] (std::size_t expected, const std::string& what)
    { return ctx.expect (track.regions.size() == expected, what); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 500, click });
    steps->push_back ({ 150, [&ctx, &engine, &track, &transport, command, counted, second]
    {
        ctx.expect (command ('c'), "Copy was not handled");
        const auto& clip = engine.getRegionClipboard();
        ctx.expect (clip.hasContent && clip.sourceTrack == 0
                    && clip.region.timelineStart == second * 2
                    && clip.region.lengthInSamples == second * 4,
                    "Copy did not put the selected region on the clipboard");
        transport.setPlayhead (second * 10);
        ctx.expect (command ('v'), "Paste was not handled");
        if (counted (2, "Paste did not add a region"))
            ctx.expect (track.regions[1].timelineStart == second * 10
                        && track.regions[1].lengthInSamples == second * 4,
                        "Paste did not land the copy at the playhead");
        ctx.expect (command ('z'), "Undo after Paste was not handled");
        counted (1, "Undo did not remove the pasted region");
    } });
    steps->push_back ({ 500, click });
    steps->push_back ({ 150, [&host, &ctx, &track, command, counted, second]
    {
        ctx.expect (command ('d'), "Duplicate was not handled");
        if (counted (2, "Duplicate did not add a region"))
            ctx.expect (track.regions[1].timelineStart == second * 6,
                        "Duplicate did not follow the original");
        ctx.expect (command ('z'), "Undo after Duplicate was not handled");
        counted (1, "Undo did not remove the duplicate");
        ctx.expect (host.pressKey ("command + shift + Z", controlCharacter ('z')), "Redo was not handled");
        counted (2, "Cmd+Shift+Z did not redo the duplicate");
        ctx.expect (command ('z'), "Undo before the second Redo was not handled");
        counted (1, "Undo did not remove the redone duplicate");
        ctx.expect (command ('y'), "Cmd+Y was not handled");
        counted (2, "Cmd+Y did not redo the duplicate");
        ctx.expect (command ('z'), "Undo after Cmd+Y was not handled");
        counted (1, "Undo did not leave a single region");
    } });
    steps->push_back ({ 500, click });
    steps->push_back ({ 150, [&ctx, &track, &transport, command, counted, second]
    {
        transport.setPlayhead (second * 4);
        ctx.expect (command ('e'), "Split was not handled");
        if (counted (2, "Split did not divide the region in two"))
            ctx.expect (track.regions[0].lengthInSamples == second * 2
                        && track.regions[1].timelineStart == second * 4
                        && track.regions[1].lengthInSamples == second * 2,
                        "Split did not cut at the playhead");
        ctx.expect (command ('z'), "Undo after Split was not handled");
        if (counted (1, "Undo did not join the split back together"))
            ctx.expect (track.regions[0].lengthInSamples == second * 4, "Undo left the region trimmed");
    } });
    for (const char* key : { "delete", "backspace" })
    {
        steps->push_back ({ 500, click });
        steps->push_back ({ 150, [&host, &ctx, command, counted, key]
        {
            ctx.expect (host.pressKey (key), std::string (key) + " was not handled");
            counted (0, std::string (key) + " did not remove the selected region");
            ctx.expect (command ('z'), std::string ("Undo after ") + key + " was not handled");
            counted (1, std::string ("Undo after ") + key + " did not restore the region");
        } });
    }
    steps->push_back ({ 500, click });
    steps->push_back ({ 150, [&ctx, &engine, command, counted, second]
    {
        ctx.expect (command ('x'), "Cut was not handled");
        counted (0, "Cut did not remove the selected region");
        const auto& clip = engine.getRegionClipboard();
        ctx.expect (clip.hasContent && clip.region.timelineStart == second * 2,
                    "Cut did not put the region on the clipboard");
        ctx.expect (command ('z'), "Undo after Cut was not handled");
        counted (1, "Undo did not bring the cut region back");
    } });
    steps->push_back ({ 500, click });
    steps->push_back ({ 150, [&host, &ctx, &track, second]
    {
        host.pressKey ("alt + " + keyCodeDescription ('t'), 't');
        host.pressKey ("alt + shift + T", 'T');
        if (ctx.expect (track.regions.size() == 1, "Alt+T changed the region count"))
            ctx.expect (track.regions[0].lengthInSamples == second * 4
                        && track.regions[0].sourceOffset == 0,
                        "Alt+T or Alt+Shift+T changed the audio region");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar editKeys { Scenario {
    "gui.edit_keys", { "gui", "keyboard", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEditKeys (host, ctx); }
} };

// A locked region, audio or MIDI, takes a click on the tape strip but not a
// drag; unlocked, the same drag moves it.
std::optional<ScenarioResult> runTapeLockedRegionsStay (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    auto& midiTrack = session.track (0);
    auto& audioTrack = session.track (1);
    ctx.keep (midiTrack.mode);
    for (auto* track : { &midiTrack, &audioTrack })
    {
        ctx.keep (track->frozen);
        track->frozen.store (false);
    }
    const auto view = host.tapeView();
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, &engine, &session, &midiTrack, &audioTrack, timeline, view, snap = session.snapToGrid,
                  midi = midiTrack.midiRegions.current(), midiAudio = midiTrack.regions, audio = audioTrack.regions]
    {
        midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (midi));
        midiTrack.regions = midiAudio;
        audioTrack.regions = audio;
        session.snapToGrid = snap;
        engine.getUndoManager().clearUndoHistory();
        host.setTimelineShown (timeline);
        host.restoreTapeView (view);
    });
    session.snapToGrid = false;
    midiTrack.mode.store ((int) Track::Mode::Midi);
    midiTrack.regions.clear();
    const double rate = engine.getTimelineSampleRate();
    MidiRegion midi;
    midi.timelineStart = (std::int64_t) std::llround (rate * 2.0);
    midi.lengthInTicks = 3840;
    midi.lengthInSamples = (std::int64_t) std::llround (rate * 2.0);
    midi.notes = { { 1, 60, 100, 0, 480 } };
    midi.locked = true;
    midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    AudioRegion audio;
    audio.timelineStart = midi.timelineStart;
    audio.lengthInSamples = midi.lengthInSamples;
    audio.locked = true;
    audioTrack.regions = { audio };
    engine.getUndoManager().clearUndoHistory();

    const auto start = [&midiTrack, &audioTrack] (bool isMidi)
    {
        if (isMidi) return midiTrack.midiRegions.current().empty() ? (std::int64_t) -1 : midiTrack.midiRegions.current().front().timelineStart;
        return audioTrack.regions.empty() ? (std::int64_t) -1 : audioTrack.regions.front().timelineStart;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("0", '0'), "the zoom-to-fit key was not handled"); } });
    for (const bool isMidi : { true, false })
    {
        const std::string what = isMidi ? "MIDI" : "audio";
        const int track = isMidi ? 0 : 1;
        steps->push_back ({ 300, [&host, &ctx, what, track, isMidi]
        { ctx.expect (host.dragTapeRegion (track, 0, isMidi, 120), "the locked " + what + " region was not on screen to drag"); } });
        steps->push_back ({ 300, [&ctx, &engine, &midiTrack, &audioTrack, start, what, isMidi, from = midi.timelineStart]
        {
            ctx.expect (start (isMidi) == from, "a drag moved the locked " + what + " region to sample " + std::to_string (start (isMidi)));
            ctx.expect (! engine.getUndoManager().canUndo(), "a drag on the locked " + what + " region left an undo step");
            if (isMidi) midiTrack.midiRegions.currentMutable().front().locked = false;
            else        audioTrack.regions.front().locked = false;
        } });
        steps->push_back ({ 300, [&host, &ctx, what, track, isMidi]
        { ctx.expect (host.dragTapeRegion (track, 0, isMidi, 120), "the unlocked " + what + " region was not on screen to drag"); } });
        steps->push_back ({ 300, [&ctx, &engine, start, what, isMidi, from = midi.timelineStart]
        {
            ctx.expect (start (isMidi) > from, "the same drag did not move the " + what + " region once unlocked");
            engine.getUndoManager().clearUndoHistory();
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tapeLockedRegionsStay { Scenario {
    "gui.tape_locked_regions_stay", { "gui", "tape", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTapeLockedRegionsStay (host, ctx); }
} };

// A region that reaches a track with nothing announcing it, as a take committed
// when the device stops does, shows on the tape strip within a tick of its timer.
std::optional<ScenarioResult> runTapeRepaintsUnannouncedRegions (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    auto& midiTrack = session.track (0);
    auto& audioTrack = session.track (1);
    ctx.keep (midiTrack.mode);
    const auto view = host.tapeView();
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, &midiTrack, &audioTrack, timeline, view,
                  midi = midiTrack.midiRegions.current(), audio = audioTrack.regions]
    {
        midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (midi));
        audioTrack.regions = audio;
        host.setTimelineShown (timeline);
        host.restoreTapeView (view);
    });
    midiTrack.mode.store ((int) Track::Mode::Midi);
    const double rate = engine.getTimelineSampleRate();
    MidiRegion midi;
    midi.timelineStart = (std::int64_t) std::llround (rate);
    midi.lengthInTicks = 1920;
    midi.lengthInSamples = (std::int64_t) std::llround (rate);
    midi.notes = { { 1, 60, 100, 0, 480 } };
    midiTrack.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    AudioRegion audio;
    audio.lengthInSamples = (std::int64_t) std::llround (rate);
    audioTrack.regions = { audio };

    const auto paints = std::make_shared<int> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("0", '0'), "the zoom-to-fit key was not handled"); } });
    steps->push_back ({ 500, [&host, paints] { *paints = host.tapePaints(); } });
    steps->push_back ({ 500, [&host, &ctx, &midiTrack, midi, rate, paints]
    {
        if (host.tapePaints() != *paints)
        {
            ctx.complete (ScenarioResult::skip (
                "the tape strip repaints with nothing changed on this display, so an unannounced change cannot be told apart"));
            return;
        }
        auto later = midi;
        later.timelineStart += (std::int64_t) std::llround (rate * 2.0);
        midiTrack.midiRegions.mutate ([&later] (std::vector<MidiRegion>& v) { v.push_back (later); });
    } });
    steps->push_back ({ 300, [&host, &ctx, &audioTrack, audio, rate, paints]
    {
        ctx.expect (host.tapePaints() > *paints, "a MIDI region committed with no broadcast did not repaint the tape strip");
        *paints = host.tapePaints();
        auto later = audio;
        later.timelineStart += (std::int64_t) std::llround (rate * 2.0);
        audioTrack.regions.push_back (later);
    } });
    steps->push_back ({ 300, [&host, &ctx, paints]
    { ctx.expect (host.tapePaints() > *paints, "an audio region committed with no broadcast did not repaint the tape strip"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tapeRepaintsUnannouncedRegions { Scenario {
    "gui.tape_repaints_unannounced_regions", { "gui", "tape" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTapeRepaintsUnannouncedRegions (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
