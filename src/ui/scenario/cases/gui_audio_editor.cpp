#include "../GuiScenarioSupport.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../session/SessionSerializer.h"
#include "../../../session/TakeComp.h"

#include <algorithm>
#include <atomic>
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
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
// Undo and Redo from the editor keep it on the region it was showing, found again by
// what the region is rather than where it sat in the track's list, which a promote
// reorders.
std::optional<ScenarioResult> runAudioEditorUndoKeepsFocus (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    for (const auto& [from, to] : { std::pair<std::int64_t, std::int64_t> { 0, 24000 }, { 24000, 48000 }, { 72000, 96000 } })
        if (auto region = regionFromTake (track.takes[0], from, to))
            track.regions.push_back (*region);
    session.audioEditorSnap = false;
    const auto other = ids->back();
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto focusedStart = [&host, &track]
    {
        const auto selection = host.audioEditorSelection();
        if (selection.size() != 5 || selection[0] < 0 || selection[0] >= (std::int64_t) track.regions.size())
            return std::int64_t { -1 };
        return track.regions[(std::size_t) selection[0]].timelineStart;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, other]
    {
        const auto from = host.audioEditorTakePoint ("lane", other, 20000);
        const auto to = host.audioEditorTakePoint ("lane", other, 52000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "take lane geometry unavailable")) return;
        host.audioEditorPointer (from[0], from[1], true);
        host.audioEditorPointer (to[0], to[1], true);
        host.audioEditorPointer (to[0], to[1], false);
    }, [&host] { return host.audioEditorTakeLanes().size() == 2; }, "the take lanes never showed" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickAudioEditorSample (84000), "the last region is not clickable"); },
    [&track, other] { return ! takeCoverage (track, other).empty(); }, "the drag did not promote the take" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the promote"); },
    [focusedStart] { return focusedStart() == 72000; }, "the click did not focus the last region" });
    steps->push_back ({ 150, [&host, &ctx, &track, focusedStart, other]
    {
        ctx.expect (takeCoverage (track, other).empty(), "Undo did not take the promote back");
        ctx.expect (focusedStart() == 72000, "after Undo the editor shows the region at "
                                                 + std::to_string (focusedStart()) + " rather than the one it showed");
        ctx.expect (host.clickAudioEditorButton ("Redo"), "Redo unavailable after the undo");
    } });
    steps->push_back ({ 150, [&ctx, &track, focusedStart, other]
    {
        ctx.expect (! takeCoverage (track, other).empty(), "Redo did not bring the promote back");
        ctx.expect (focusedStart() == 72000, "after Redo the editor shows the region at "
                                                 + std::to_string (focusedStart()) + " rather than the one it showed");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorUndoKeepsFocus { Scenario {
    "gui.audio_editor_undo_keeps_focus", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorUndoKeepsFocus (host, ctx); }
} };

// With one region focused and another added by Cmd-click, a drag on the added one
// moves both by the same distance, and one Undo puts both back.
std::optional<ScenarioResult> runAudioEditorMoveSelection (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    if (! addLevelTake (ctx, track, "Fixture", 0, kTakeCaseLength, 0.5f))
        return ScenarioResult::fail ("could not write region fixture");
    static constexpr std::int64_t kFirst = 0;
    static constexpr std::int64_t kSecond = 60000;
    for (const auto& [from, to] : { std::pair<std::int64_t, std::int64_t> { kFirst, 36000 }, { kSecond, kTakeCaseLength } })
        if (auto region = regionFromTake (track.takes.front(), from, to))
            track.regions.push_back (*region);
    if (track.regions.size() != 2) return ScenarioResult::fail ("could not cut the region fixture");
    session.audioEditorSnap = false;
    // Opened from the take count, the view spans the whole track rather than the first region.
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    // Found by the audio it plays, which a move leaves alone.
    const auto startOf = [&track] (std::int64_t sourceOffset)
    {
        for (const auto& region : track.regions)
            if (region.sourceOffset == sourceOffset) return region.timelineStart;
        return std::int64_t { -1 };
    };
    const auto point = [&host, &ctx] (std::int64_t sample)
    {
        const auto at = host.audioEditorPoint ("wave", sample);
        ctx.expect (at.size() == 2, "editor geometry unavailable");
        return at.size() == 2 ? at : std::vector<int> { 0, 0 };
    };
    static constexpr int command = 2;
    auto held = std::make_shared<std::vector<int>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, point]
    {
        const auto at = point (78000);
        ctx.expect (host.audioEditorPointer (at[0], at[1], true, command), "the Cmd-click's press failed");
        host.audioEditorPointer (at[0], at[1], false, command);
    }, [&host] { return host.audioEditorTakeLanes().size() == 1; }, "the editor never laid out" });
    // The press lands where the Cmd-click did, so it waits out the double-click time.
    steps->push_back ({ 700, [&host, &ctx, point, held]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[0] == 0, "the Cmd-click took the focus off the first region");
        const auto from = point (78000);
        *held = point (90000);
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the press on the added region failed");
        host.audioEditorPointer ((*held)[0], (*held)[1], true);
    } });
    steps->push_back ({ 100, [&host, held] { host.audioEditorPointer ((*held)[0], (*held)[1], false); },
                        [startOf] { return startOf (kSecond) > kSecond; }, "the added region did not follow the pointer" });
    steps->push_back ({ 200, [&host, &ctx, &engine, startOf]
    {
        const auto moved = startOf (kSecond) - kSecond;
        ctx.expect (startOf (kFirst) - kFirst == moved, "the focused region did not move with the one dragged: it moved "
                                                          + std::to_string (startOf (kFirst) - kFirst) + " samples against "
                                                          + std::to_string (moved));
        ctx.expect (undoDescription (engine) == "Move region", "the move is not the \"Move region\" step");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the move");
    } });
    steps->push_back ({ 200, [&ctx, &engine, startOf]
    {
        ctx.expect (startOf (kFirst) == kFirst && startOf (kSecond) == kSecond, "one Undo did not put both regions back");
        ctx.expect (! engine.getUndoManager().canUndo(), "the move took more than one undo step");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorMoveSelection { Scenario {
    "gui.audio_editor_move_selection", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorMoveSelection (host, ctx); }
} };

// Regions cut from one fixture take over the given timeline spans, with the editor
// open over the whole track and Snap off.
std::optional<ScenarioResult> beginSpansCase (GuiHost& host, ScenarioContext& ctx,
                                              const std::vector<std::pair<std::int64_t, std::int64_t>>& spans)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    if (! addLevelTake (ctx, track, "Fixture", 0, kTakeCaseLength, 0.5f))
        return ScenarioResult::fail ("could not write region fixture");
    for (const auto& [from, to] : spans)
        if (auto region = regionFromTake (track.takes.front(), from, to))
            track.regions.push_back (*region);
    if (track.regions.size() != spans.size()) return ScenarioResult::fail ("could not cut the region fixture");
    session.audioEditorSnap = false;
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");
    return std::nullopt;
}

// The region playing the fixture from `sourceOffset`, which a move leaves alone.
const AudioRegion* regionPlaying (const Track& track, std::int64_t sourceOffset)
{
    for (const auto& region : track.regions)
        if (region.sourceOffset == sourceOffset) return &region;
    return nullptr;
}

void clickEditorWave (GuiHost& host, ScenarioContext& ctx, std::int64_t sample, int modifiers = 0)
{
    const auto at = host.audioEditorPoint ("wave", sample);
    if (! ctx.expect (at.size() == 2, "editor geometry unavailable")) return;
    host.audioEditorPointer (at[0], at[1], true, modifiers);
    host.audioEditorPointer (at[0], at[1], false, modifiers);
}

// With the focused region at 0 and a later one added by Cmd-click, a drag left
// leaves the focused one at 0 and moves the other, still as one "Move region" step
// that one Undo takes back.
std::optional<ScenarioResult> runAudioEditorGroupMoveFromZero (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kFirst = 0;
    static constexpr std::int64_t kSecond = 60000;
    if (auto early = beginSpansCase (host, ctx, { { kFirst, 36000 }, { kSecond, kTakeCaseLength } })) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto startOf = [&track] (std::int64_t sourceOffset)
    {
        const auto* region = regionPlaying (track, sourceOffset);
        return region != nullptr ? region->timelineStart : std::int64_t { -1 };
    };
    static constexpr int command = 2;
    auto held = std::make_shared<std::vector<int>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { clickEditorWave (host, ctx, 78000, command); },
                        [&host] { return host.audioEditorTakeLanes().size() == 1; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx, held]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[0] == 0, "the Cmd-click took the focus off the region at 0");
        const auto from = host.audioEditorPoint ("wave", 24000);
        *held = host.audioEditorPoint ("wave", 12000);
        if (! ctx.expect (from.size() == 2 && held->size() == 2, "editor geometry unavailable")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the press on the focused region failed");
        host.audioEditorPointer ((*held)[0], (*held)[1], true);
    } });
    steps->push_back ({ 100, [&host, held] { if (held->size() == 2) host.audioEditorPointer ((*held)[0], (*held)[1], false); },
                        [startOf] { return startOf (kSecond) < kSecond - 6000; }, "the added region did not follow the pointer" });
    steps->push_back ({ 200, [&host, &ctx, &engine, startOf]
    {
        ctx.expect (startOf (kFirst) == kFirst, "the focused region left 0");
        ctx.expect (undoDescription (engine) == "Move region", "the move with the focused region at 0 recorded no "
                                                               "\"Move region\" step");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the move");
    } });
    steps->push_back ({ 200, [&ctx, &engine, startOf]
    {
        ctx.expect (startOf (kFirst) == kFirst && startOf (kSecond) == kSecond, "one Undo did not put the moved region back");
        ctx.expect (! engine.getUndoManager().canUndo(), "the move took more than one undo step");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorGroupMoveFromZero { Scenario {
    "gui.audio_editor_group_move_from_zero", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorGroupMoveFromZero (host, ctx); }
} };

// A split adds a region to the list ahead of the others selected, and the selection
// stays on the regions the user picked: after Cmd+E, the toolbar's Split, the Cut
// tool or Cmd+E over a range splits the focused region, Delete takes the focused
// region's first piece and the region Cmd-clicked, never a neighbour.
std::optional<ScenarioResult> runAudioEditorSplitKeepsSelection (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kNeighbour = 36000;
    static constexpr std::int64_t kPicked = 72000;
    if (auto early = beginSpansCase (host, ctx, { { 0, 24000 }, { kNeighbour, 60000 }, { kPicked, kTakeCaseLength } }))
        return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    ctx.cleanup ([&session, mode = session.editMode] { session.editMode = mode; });
    const auto before = track.regions;
    static constexpr int shift = 1;
    static constexpr int command = 2;

    struct Split { std::string label; std::size_t pieces; std::vector<std::function<void()>> run; };
    const std::vector<Split> splits {
        { "Cmd+E", 2, { [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("command + E"), "Cmd+E was not handled"); } } },
        { "the toolbar's Split", 2,
          { [&host, &ctx] { ctx.expect (host.clickAudioEditorButton ("Split"), "no Split button"); } } },
        { "the Cut tool", 2, { [&host, &ctx, &session]
          {
              session.editMode = EditMode::Cut;
              clickEditorWave (host, ctx, 16000);
          } } },
        { "Cmd+E over a range", 3, { [&host, &ctx]
          {
              const auto from = host.audioEditorPoint ("wave", 6000);
              const auto to = host.audioEditorPoint ("wave", 18000);
              if (! ctx.expect (from.size() == 2 && to.size() == 2, "editor geometry unavailable")) return;
              host.audioEditorPointer (from[0], from[1], true, shift);
              host.audioEditorPointer (to[0], to[1], true, shift);
              host.audioEditorPointer (to[0], to[1], false, shift);
          },
          [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("command + E"), "Cmd+E over a range was not handled"); } } },
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorTakeLanes().size() == 1; }, "the editor never laid out" });
    for (const auto& split : splits)
    {
        const auto label = split.label;
        const auto pieces = split.pieces;
        steps->push_back ({ 150, [&engine, &session, &track, before]
        {
            session.editMode = EditMode::Grab;
            track.regions = before;
            engine.getUndoManager().clearUndoHistory();
        } });
        steps->push_back ({ 150, [&host, &ctx] { clickEditorWave (host, ctx, 12000); } });
        steps->push_back ({ 150, [&host, &ctx] { clickEditorWave (host, ctx, 84000, command); } });
        for (const auto& run : split.run)
            steps->push_back ({ 150, run });
        steps->push_back ({ 150, [&host, &ctx, label] { ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled"); },
                            [&track, pieces] { return track.regions.size() == 2 + pieces; },
                            label + " did not split the focused region" });
        steps->push_back ({ 200, [&ctx, &track, label, pieces]
        {
            ctx.expect (regionPlaying (track, kPicked) == nullptr, "after " + label + ", Delete left the Cmd-clicked region");
            ctx.expect (regionPlaying (track, kNeighbour) != nullptr,
                        "after " + label + ", Delete took a region that was never selected");
            ctx.expect (regionPlaying (track, 0) == nullptr && track.regions.size() == pieces,
                        "after " + label + ", Delete did not take the focused region's first piece");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorSplitKeepsSelection { Scenario {
    "gui.audio_editor_split_keeps_selection", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorSplitKeepsSelection (host, ctx); }
} };

// A locked region refuses every edit the editor offers it: Delete, Cmd+E, the
// toolbar's Split and Normalize, the menu's Split at edit cursor, Reset gain and
// Reset fades, typed gain and fades, Properties > Delete region, and Join selected
// regions with an unlocked region selected beside it, each leaving the region as it
// was and no undo step. With the unlocked region selected, Delete takes that one alone.
std::optional<ScenarioResult> runAudioEditorLockRefusesEdits (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kLocked = 0;
    static constexpr std::int64_t kFree = 60000;
    if (auto early = beginSpansCase (host, ctx, { { kLocked, 36000 }, { kFree, kTakeCaseLength } })) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    auto& fixture = track.regions.front();
    fixture.locked = true;
    fixture.gainDb = -3.0f;
    fixture.fadeInSamples = 4800;
    fixture.fadeOutSamples = 9600;
    const auto held = fixture;
    auto& transport = engine.getTransport();
    ctx.cleanup ([&transport, enabled = transport.isLoopEnabled(), loopStart = transport.getLoopStart(),
                  loopEnd = transport.getLoopEnd()]
    {
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (enabled);
    });
    transport.setLoopEnabled (false);

    const auto untouched = [&ctx, &engine, &track, held] (const std::string& what)
    {
        const auto* region = regionPlaying (track, kLocked);
        ctx.expect (region != nullptr && region->locked && region->timelineStart == held.timelineStart
                        && region->lengthInSamples == held.lengthInSamples
                        && std::abs (region->gainDb - held.gainDb) < 1.0e-4f
                        && region->fadeInSamples == held.fadeInSamples && region->fadeOutSamples == held.fadeOutSamples,
                    what + " changed the locked region");
        ctx.expect (track.regions.size() == 2, what + " changed the track's regions");
        ctx.expect (! engine.getUndoManager().canUndo(), what + " recorded an undo step");
    };
    const auto refused = [&host, &ctx] (const char* control, const std::string& what)
    { ctx.expect (! host.clickAudioEditorButton (control), what + " was offered on a locked region"); };
    const auto refusedItem = [&host, &ctx] (const char* item)
    { ctx.expect (! host.clickContextMenuItem (item), std::string (item) + " was offered on a locked region"); };
    static constexpr int rightButton = 4;
    static constexpr int command = 2;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { clickEditorWave (host, ctx, 12000); },
                        [&host] { return host.audioEditorTakeLanes().size() == 1; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled"); } });
    steps->push_back ({ 150, [&host, &ctx, untouched]
    {
        untouched ("Delete");
        ctx.expect (host.pressAudioEditorKey ("command + E"), "Cmd+E was not handled");
    } });
    steps->push_back ({ 150, [untouched, refused]
    {
        untouched ("Cmd+E");
        refused ("Split", "the toolbar's Split");
        refused ("Normalize", "the toolbar's Normalize");
        refused ("Gain", "typing a gain");
        refused ("Fades", "typing fades");
    } });
    steps->push_back ({ 150, [&host, &ctx, untouched]
    {
        untouched ("the toolbar and the readouts");
        clickEditorWave (host, ctx, 12000, rightButton);
    } });
    steps->push_back ({ 150, [&host, &ctx, refusedItem]
    {
        refusedItem ("Split at edit cursor");
        refusedItem ("Reset gain (0 dB)");
        refusedItem ("Reset fades");
        ctx.expect (host.clickContextMenuItem ("Loop region"), "the waveform menu did not open");
    } });
    steps->push_back ({ 150, [&host, &ctx, &transport, untouched]
    {
        ctx.expect (transport.isLoopEnabled(), "Loop region did not loop the region");
        untouched ("the waveform menu");
        ctx.expect (host.clickAudioEditorButton ("Properties"), "no Properties button");
    } });
    steps->push_back ({ 150, [&host, &ctx, refusedItem]
    {
        refusedItem ("Delete region");
        ctx.expect (host.clickContextMenuItem ("Color"), "the Properties menu did not open");
    } });
    steps->push_back ({ 150, [&host, &ctx] { clickEditorWave (host, ctx, 30000); } });
    steps->push_back ({ 150, [&host, &ctx, untouched]
    {
        ctx.expect (host.audioEditorOpen(), "Properties > Delete region closed the editor");
        untouched ("Properties > Delete region");
        clickEditorWave (host, ctx, 78000, command);
    } });
    steps->push_back ({ 150, [&host, &ctx] { clickEditorWave (host, ctx, 12000, rightButton); } });
    steps->push_back ({ 150, [&host, &ctx, refusedItem]
    {
        refusedItem ("Join selected regions");
        ctx.expect (host.clickContextMenuItem ("Loop region"), "the waveform menu did not open");
    } });
    steps->push_back ({ 150, [&host, &ctx, untouched]
    {
        untouched ("Join selected regions");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled");
    } });
    steps->push_back ({ 200, [&ctx, &engine, &track, held]
    {
        const auto* region = regionPlaying (track, kLocked);
        ctx.expect (regionPlaying (track, kFree) == nullptr, "Delete with both selected left the unlocked region");
        ctx.expect (track.regions.size() == 1 && region != nullptr && region->lengthInSamples == held.lengthInSamples,
                    "Delete with both selected took the locked region");
        ctx.expect (undoDescription (engine) == "Delete region", "Delete with both selected is not one \"Delete region\" step");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorLockRefusesEdits { Scenario {
    "gui.audio_editor_lock_refuses_edits", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorLockRefusesEdits (host, ctx); }
} };

// A punch committed under a held trim, as a record stop does, ends the trim: the
// release records nothing and Escape puts nothing back, so the regions stay as the
// punch left them. A click on a take's name still waiting out the double-click time
// when a trim starts never promotes the take; the trim alone lands.
std::optional<ScenarioResult> runAudioEditorDragYieldsToReshape (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto whole = addLevelTake (ctx, track, "Take 1", 0, kTakeCaseLength, kWholeTakeLevel);
    const auto punch = addLevelTake (ctx, track, "Take 2", 24000, 24000, kShortTakeLevel);
    if (! whole || ! punch) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto before = track.regions;
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    auto held = std::make_shared<std::vector<int>>();
    const auto pressTrim = [&host, &ctx, held]
    {
        const auto from = host.audioEditorPoint ("start", 0);
        *held = host.audioEditorPoint ("wave", 12000);
        if (! ctx.expect (from.size() == 2 && held->size() == 2, "trim geometry unavailable")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the trim handle did not take the press");
        host.audioEditorPointer ((*held)[0], (*held)[1], true);
    };
    const auto dragOn = [&host, held]
    {
        const auto to = host.audioEditorPoint ("wave", 18000);
        if (to.size() != 2) return;
        *held = to;
        host.audioEditorPointer (to[0], to[1], true);
    };
    const auto release = [&host, held] { if (held->size() == 2) host.audioEditorPointer ((*held)[0], (*held)[1], false); };
    const auto trimmed = [&track] { return ! track.regions.empty() && track.regions[0].timelineStart > 6000; };
    auto reshaped = std::make_shared<std::vector<AudioRegion>>();
    const auto punchIn = [&session, &track, punch, reshaped]
    {
        if (const auto* take = findTake (track, *punch))
        {
            const auto copy = *take;
            promoteTakeRange (session, track, copy, 24000, 48000);
        }
        *reshaped = track.regions;
    };
    const auto keptPunch = [&ctx, &engine, &track, reshaped] (const std::string& what)
    {
        ctx.expect (reshaped->size() == 3 && sameRegions (track.regions, *reshaped),
                    what + " wrote the dragged region over the punch");
        ctx.expect (! engine.getUndoManager().canUndo(), what + " recorded a step for a drag the punch ended");
    };
    const auto reset = [&engine, &track, before]
    {
        track.regions = before;
        engine.getUndoManager().clearUndoHistory();
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, pressTrim, [&host] { return host.audioEditorTakeLanes().size() == 2; },
                        "the take lanes never showed" });
    steps->push_back ({ 100, punchIn, trimmed, "the trim did not follow the pointer" });
    steps->push_back ({ 150, dragOn });
    steps->push_back ({ 150, release });
    steps->push_back ({ 200, [keptPunch, reset]
    {
        keptPunch ("the release after a punch");
        reset();
    } });
    steps->push_back ({ 150, pressTrim });
    steps->push_back ({ 100, punchIn, trimmed, "the second trim did not follow the pointer" });
    steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("escape"), "Escape was not delivered"); } });
    steps->push_back ({ 150, [&host, &ctx, keptPunch, release]
    {
        ctx.expect (host.audioEditorOpen(), "Escape during the trim closed the editor");
        keptPunch ("Escape after a punch");
        release();
    } });
    steps->push_back ({ 200, [keptPunch, reset]
    {
        keptPunch ("the release after a cancelled trim");
        reset();
    } });
    steps->push_back ({ 150, [&host, &ctx, punch]
    { ctx.expect (host.clickAudioEditorButton (takeControl ("name", *punch)), "the take's name is not clickable"); } });
    // The click's release goes in after the next frame; the trim follows it well
    // inside the double-click time, with no delay that a slow renderer would stretch
    // past it.
    steps->push_back ({ 0, pressTrim });
    // Held past the double-click time, when a name click on its own promotes.
    steps->push_back ({ 700, [&ctx, &track, punch, release]
    {
        ctx.expect (takeCoverage (track, *punch).empty(), "the name click promoted the take under the trim");
        release();
    }, trimmed, "the trim after the name click did not follow the pointer" });
    steps->push_back ({ 200, [&ctx, &engine, &track, punch, before]
    {
        ctx.expect (takeCoverage (track, *punch).empty(), "the name click promoted the take after the trim");
        ctx.expect (track.regions.size() == 1 && track.regions[0].timelineStart > 6000,
                    "the trim after the name click did not land");
        ctx.expect (undoDescription (engine) == "Trim start", "the trim after the name click is not one \"Trim start\" step");
        engine.getUndoManager().undo();
        ctx.expect (sameRegions (track.regions, before) && ! engine.getUndoManager().canUndo(),
                    "the trim after the name click took more than one step");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorDragYieldsToReshape { Scenario {
    "gui.audio_editor_drag_yields_to_reshape", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorDragYieldsToReshape (host, ctx); }
} };

// A recording that stops under a region drag on the recorded track, from a Stop sent
// over MIDI or a sync stop, puts the region back as the drag found it before the take
// lands. The take's one "Record" step undoes to the region as it was before the drag,
// and the release records nothing.
std::optional<ScenarioResult> runAudioEditorRecordStopCancelsDrag (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! engine.isAudioCallbackRegistered() || session.deviceCaptureChannels.load() <= 0)
        return ScenarioResult::skip ("requires a running audio device with an input to record from");
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = session.track (0);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    ctx.keep (track.mode);
    ctx.keep (track.inputSource);
    ctx.keep (session.countInEnabled);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
    }
    ctx.cleanup ([&engine, &transport, loop = transport.isLoopEnabled(), punch = transport.isPunchEnabled(),
                  at = transport.getPlayhead()]
    {
        engine.stop();
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
        transport.setPlayhead (at);
    });
    applySessionDirectory (session, ctx.tempDir() / "Record stop session");
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Mono);
    track.inputSource.store (0);
    session.setTrackArmed (0, true);
    if (! track.recordArmed.load())
        return ScenarioResult::fail ("track 1 would not arm on input 1");
    if (! addLevelTake (ctx, track, "Take 1", 0, kTakeCaseLength, kWholeTakeLevel))
        return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto before = track.regions;
    const auto takesBefore = track.takes;
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    struct Pass { std::string label; const char* from; std::int64_t at; std::int64_t to; PendingTransportAction stop; };
    const std::vector<Pass> passes {
        { "a trim stopped over MIDI", "start", 0, 12000, PendingTransportAction::Stop },
        { "a move stopped by sync", "wave", 24000, 36000, PendingTransportAction::SyncStop },
    };

    auto held = std::make_shared<std::vector<int>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorTakeLanes().size() == 1; },
                        "the take lanes never showed" });
    for (const auto& pass : passes)
    {
        steps->push_back ({ 150, [&ctx, &engine, &transport, &track, before, takesBefore, label = pass.label]
        {
            track.regions = before;
            track.takes = takesBefore;
            engine.getUndoManager().clearUndoHistory();
            transport.setPlayhead (48000);
            engine.record();
            ctx.expect (transport.isRecording(), label + ": the armed track did not start recording");
        } });
        steps->push_back ({ 300, [&host, &ctx, held, pass]
        {
            const auto from = host.audioEditorPoint (pass.from, pass.at);
            *held = host.audioEditorPoint ("wave", pass.to);
            if (! ctx.expect (from.size() == 2 && held->size() == 2, pass.label + ": editor geometry unavailable")) return;
            ctx.expect (host.audioEditorPointer (from[0], from[1], true), pass.label + ": the press did not land");
            host.audioEditorPointer ((*held)[0], (*held)[1], true);
        } });
        steps->push_back ({ 100, [&session, stop = pass.stop]
        { session.pendingTransportAction.store ((int) stop, std::memory_order_release); },
                            [&track] { return ! track.regions.empty() && track.regions[0].timelineStart > 6000; },
                            pass.label + ": the drag did not follow the pointer" });
        steps->push_back ({ 100, [&host, &ctx, &engine, &track, held, label = pass.label]
        {
            ctx.expect (undoDescription (engine) == "Record", label + ": the stop did not record one \"Record\" step");
            ctx.expect (track.takes.size() == 2, label + ": the stop did not land the take");
            if (held->size() == 2) host.audioEditorPointer ((*held)[0], (*held)[1], false);
        },
        [&transport] { return transport.isStopped(); }, pass.label + ": the stop request did not stop the take" });
        steps->push_back ({ 200, [&ctx, &engine, &track, before, label = pass.label]
        {
            ctx.expect (undoDescription (engine) == "Record", label + ": the release after the stop recorded a step");
            engine.getUndoManager().undo();
            ctx.expect (sameRegions (track.regions, before),
                        label + ": undoing the take did not put the region back as it was before the drag");
            ctx.expect (! engine.getUndoManager().canUndo(), label + ": the stop left more than the take to undo");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorRecordStopCancelsDrag { Scenario {
    "gui.audio_editor_record_stop_cancels_drag", { "gui", "editor", "transport", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorRecordStopCancelsDrag (host, ctx); }
} };

// Three regions of one take end to end on track 1, at 0, 24000 and 48000, with the
// editor open on one of them.
std::optional<ScenarioResult> beginEditorFocusCase (GuiHost& host, ScenarioContext& ctx, int focused)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    if (! addLevelTake (ctx, track, "Take 1", 0, kTakeCaseLength, kWholeTakeLevel))
        return ScenarioResult::fail ("could not write take fixture");
    for (const auto& [from, to] : { std::pair<std::int64_t, std::int64_t> { 0, 24000 }, { 24000, 48000 }, { 48000, 96000 } })
        if (auto region = regionFromTake (track.takes[0], from, to))
            track.regions.push_back (*region);
    if (track.regions.size() != 3) return ScenarioResult::fail ("the setup did not leave three regions");
    ctx.session().audioEditorSnap = false;
    if (! host.openAudioEditor (0, focused)) return ScenarioResult::fail ("audio editor unavailable");
    return std::nullopt;
}

// The region the editor has in focus, or nullopt when it has none or names one the
// track does not hold.
std::optional<AudioRegion> editorFocus (GuiHost& host, const Track& track)
{
    const auto selection = host.audioEditorSelection();
    if (selection.size() != 5 || selection[0] < 0 || selection[0] >= (std::int64_t) track.regions.size())
        return std::nullopt;
    return track.regions[(std::size_t) selection[0]];
}

// True once the editor has drawn and has the region starting at `start` in focus.
bool editorShows (GuiHost& host, const Track& track, std::int64_t start)
{
    const auto focus = editorFocus (host, track);
    return focus && focus->timelineStart == start && host.audioEditorPoint ("wave", start).size() == 2;
}

// With the editor on the last of three regions: a take over the first region alone
// moves the focused region down the list and the editor stays on it; a take that cuts
// off its head leaves the editor on what is left of it, not on the take; and a take
// over all of it, with a range drawn and a second region selected, leaves the editor
// on the take's region with neither. A click on the first region then focuses that
// one, and Delete removes it alone.
std::optional<ScenarioResult> runAudioEditorRecordStopRefocuses (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorFocusCase (host, ctx, 2)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto original = track.takes[0].id;
    const auto landed = std::make_shared<TakeId> (0);
    const auto frameAfterLanding = std::make_shared<std::uint64_t> (0);
    static constexpr int shift = 1;
    static constexpr int command = 2;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &track, frameAfterLanding]
    {
        ctx.expect (landTake (ctx, track, 0, 24000, true).has_value() && track.regions.size() == 3
                        && track.regions[1].timelineStart == 48000,
                    "a take over the first region did not move the last region down the list");
        *frameAfterLanding = host.audioEditorCompletedFrames();
    }, [&host, &track] { return editorShows (host, track, 48000); }, "the editor never showed the last region" });
    steps->push_back ({ 100, [&host, &ctx, &track, frameAfterLanding]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[0] == 1, "the editor did not follow its region to its new index");
        ctx.expect (landTake (ctx, track, 42000, 12000, false).has_value() && track.regions.size() == 4,
                    "a take across the seam did not cut into the regions either side");
        *frameAfterLanding = host.audioEditorCompletedFrames();
    }, [&host, &track, original, frameAfterLanding]
    {
        const auto focus = editorFocus (host, track);
        return host.audioEditorCompletedFrames() > *frameAfterLanding
            && focus && focus->takeId == original && focus->timelineStart == 48000;
    },
                        "a take over another region took the editor off the one it had in focus" });
    steps->push_back ({ 100, [&host, &ctx] { clickEditorWave (host, ctx, 30000, command); },
                        [&host, &track, original, frameAfterLanding]
    {
        const auto focus = editorFocus (host, track);
        return host.audioEditorCompletedFrames() > *frameAfterLanding
            && focus && focus->takeId == original && focus->timelineStart > 48000;
    },
                        "a take that cut the head off the focused region took the editor off what is left of it" });
    steps->push_back ({ 150, [&host, &ctx]
    {
        const auto from = host.audioEditorPoint ("wave", 66000);
        const auto to = host.audioEditorPoint ("wave", 78000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "editor geometry unavailable")) return;
        host.audioEditorPointer (from[0], from[1], true, shift);
        host.audioEditorPointer (to[0], to[1], true, shift);
        host.audioEditorPointer (to[0], to[1], false, shift);
    }, [&host] { const auto selection = host.audioEditorSelection(); return selection.size() == 5 && selection[4] == 1; },
                        "the Cmd-click did not select a second region" });
    steps->push_back ({ 100, [&host, &ctx, &track, landed, frameAfterLanding]
    {
        if (const auto id = landTake (ctx, track, 24000, 72000, true))
            *landed = *id;
        ctx.expect (*landed != 0 && track.regions.size() == 2, "the take did not replace every region past the first");
        *frameAfterLanding = host.audioEditorCompletedFrames();
    }, [&host] { const auto selection = host.audioEditorSelection(); return selection.size() == 5 && selection[1] == 1 && selection[4] == 1; },
                        "the Shift drag drew no range beside the selected region" });
    steps->push_back ({ 100, [&host, &ctx]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[1] == 0, "the take replaced the focused region and its range stayed");
        ctx.expect (selection.size() == 5 && selection[4] == 0,
                    "the take replaced the focused region and another stayed selected");
        ctx.expect (host.clickAudioEditorSample (12000), "the first region is not clickable");
    }, [&host, &track, landed, frameAfterLanding]
    {
        const auto focus = editorFocus (host, track);
        const auto selection = host.audioEditorSelection();
        return host.audioEditorCompletedFrames() > *frameAfterLanding
            && focus && focus->takeId == *landed && selection.size() == 5 && selection[1] == 0 && selection[4] == 0;
    }, "the editor did not follow the landed take and clear its previous selection" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled"); },
                        [&host, &track] { const auto focus = editorFocus (host, track); return focus && focus->timelineStart == 0; },
                        "the click did not focus the first region" });
    steps->push_back ({ 150, [&host, &ctx, &engine, &track, landed]
    {
        ctx.expect (track.regions.size() == 1 && track.regions[0].takeId == *landed,
                    "Delete did not remove the first region alone");
        ctx.expect (undoDescription (engine) == "Delete region", "the delete is not one \"Delete region\" step");
        const auto focus = editorFocus (host, track);
        ctx.expect (focus && focus->takeId == *landed, "the delete did not leave the editor on the take's region");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorRecordStopRefocuses { Scenario {
    "gui.audio_editor_record_stop_refocuses", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorRecordStopRefocuses (host, ctx); }
} };

// A take that replaces the first region and cuts into the second while the editor
// shows the first leaves the editor on the take's region, not on what is left of the
// second, whether the editor was told the take was coming, was not, or had yet to draw
// its first frame. Delete then removes the take's region.
std::optional<ScenarioResult> runAudioEditorTakeReplacesFocus (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorFocusCase (host, ctx, 0)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto before = track.regions;
    const auto takesBefore = track.takes;
    const auto landed = std::make_shared<TakeId> (0);

    enum class Landing { announced, unannounced, beforeFirstFrame };
    static constexpr Landing kLandings[] = { Landing::announced, Landing::unannounced, Landing::beforeFirstFrame };
    const auto labelOf = [] (Landing landing) -> std::string
    {
        return landing == Landing::announced ? "a take the editor was told of"
             : landing == Landing::unannounced ? "a take landing unannounced"
                                               : "a take landing before the editor's first frame";
    };
    const auto land = [&ctx, &track, landed, labelOf] (Landing landing)
    {
        *landed = landTake (ctx, track, 0, 36000, landing == Landing::announced).value_or (0);
        ctx.expect (*landed != 0 && track.regions.size() == 3 && track.regions[0].timelineStart > 24000,
                    labelOf (landing) + ": the take did not replace the first region and cut into the second");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    for (std::size_t i = 0; i < std::size (kLandings); ++i)
    {
        const auto landing = kLandings[i];
        const auto label = labelOf (landing);
        if (landing != Landing::beforeFirstFrame)
            steps->push_back ({ 100, [land, landing] { land (landing); },
                                [&host, &track] { return editorShows (host, track, 0); },
                                label + ": the editor never showed the first region" });
        steps->push_back ({ 100, [&host, &ctx, label]
        { ctx.expect (host.pressAudioEditorKey ("delete"), label + ": Delete was not handled"); },
                            [&host, &track, landed] { const auto focus = editorFocus (host, track); return focus && focus->takeId == *landed; },
                            label + ": the editor is not on the take's region" });
        const auto next = i + 1 < std::size (kLandings) ? std::optional<Landing> (kLandings[i + 1]) : std::nullopt;
        steps->push_back ({ 150, [&host, &ctx, &engine, &track, before, takesBefore, landed, label, land, next]
        {
            ctx.expect (track.regions.size() == 2
                        && std::none_of (track.regions.begin(), track.regions.end(),
                                         [landed] (const AudioRegion& r) { return r.takeId == *landed; }),
                        label + ": Delete did not remove the take's region alone");
            ctx.expect (host.audioEditorOpen(), label + ": the delete closed the editor");
            if (! next) return;
            host.closeAudioEditor();
            track.regions = before;
            track.takes = takesBefore;
            engine.getUndoManager().clearUndoHistory();
            if (! ctx.expect (host.openAudioEditor (0, 0), label + ": the editor did not reopen")) return;
            if (*next == Landing::beforeFirstFrame)
                land (*next);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorTakeReplacesFocus { Scenario {
    "gui.audio_editor_take_replaces_focus", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorTakeReplacesFocus (host, ctx); }
} };

// With the focused region gone from under the editor and nothing in focus, a click on
// a region that is left focuses it, and Delete then removes it.
std::optional<ScenarioResult> runAudioEditorClickFocusesUnfocused (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorFocusCase (host, ctx, 2)) return early;
    auto& track = ctx.session().track (0);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&track] { track.regions.erase (track.regions.begin() + 1, track.regions.end()); },
                        [&host, &track] { return editorShows (host, track, 48000); },
                        "the editor never showed the last region" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickAudioEditorSample (12000), "the region that is left is not clickable"); },
                        [&host] { const auto selection = host.audioEditorSelection(); return selection.size() == 5 && selection[0] == -1; },
                        "the editor kept a focus on a region that is gone" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled"); },
                        [&host, &track] { const auto focus = editorFocus (host, track); return focus && focus->timelineStart == 0; },
                        "a click with nothing in focus did not focus the region" });
    steps->push_back ({ 150, [&host, &ctx, &track]
    {
        ctx.expect (track.regions.empty(), "Delete did not remove the region the click focused");
        ctx.expect (host.audioEditorOpen(), "the editor closed with takes still on the track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorClickFocusesUnfocused { Scenario {
    "gui.audio_editor_click_focuses_unfocused", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorClickFocusesUnfocused (host, ctx); }
} };

// With every region deleted and the takes left, Paste puts the copied region on the
// track in focus, and Delete then removes it.
std::optional<ScenarioResult> runAudioEditorPasteFocuses (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorFocusCase (host, ctx, 0)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto clipboard = engine.getRegionClipboard();
    ctx.cleanup ([&engine, clipboard] { engine.getRegionClipboard() = clipboard; });
    ctx.cleanup ([&engine, at = engine.getTransport().getPlayhead()] { engine.getTransport().setPlayhead (at); });
    engine.getTransport().setPlayhead (12000);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("command + C"), "Copy was not handled"); },
                        [&host, &track] { const auto focus = editorFocus (host, track); return focus && focus->timelineStart == 0; },
                        "the editor never showed the first region" });
    for (std::size_t left = 3; left > 0; --left)
        steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled"); },
                            [&track, left] { return track.regions.size() == left; }, "Delete did not remove one region" });
    steps->push_back ({ 100, [&host, &ctx]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (host.audioEditorOpen() && selection.size() == 5 && selection[0] == -1,
                    "deleting every region did not leave the editor up with none in focus");
        ctx.expect (host.pressAudioEditorKey ("command + V"), "Paste was not handled");
    }, [&track] { return track.regions.empty(); }, "Delete did not remove the last region" });
    steps->push_back ({ 100, [&host, &ctx, &engine]
    {
        ctx.expect (undoDescription (engine) == "Paste region", "the paste is not one \"Paste region\" step");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled");
    }, [&host, &track] { const auto focus = editorFocus (host, track); return focus && focus->timelineStart == 12000; },
                        "Paste with no region in focus did not focus the pasted region" });
    steps->push_back ({ 150, [&host, &ctx, &engine, &track]
    {
        ctx.expect (track.regions.empty(), "Delete did not remove the pasted region");
        ctx.expect (undoDescription (engine) == "Delete region", "the delete is not one \"Delete region\" step");
        ctx.expect (host.audioEditorOpen(), "the editor closed with takes still on the track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorPasteFocuses { Scenario {
    "gui.audio_editor_paste_focuses", { "gui", "keyboard", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorPasteFocuses (host, ctx); }
} };

// Joining the focused region to an earlier one that sits later in the track's list,
// with an unselected region between them in the list, leaves the editor on the joined
// region, and Delete then removes that one, not the region between.
std::optional<ScenarioResult> runAudioEditorJoinFocusesJoined (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kJoinedEnd = 72000;
    if (auto early = beginSpansCase (host, ctx, { { 48000, kJoinedEnd }, { kJoinedEnd, kTakeCaseLength }, { 0, 48000 } }))
        return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    static constexpr int rightButton = 4;
    static constexpr int command = 2;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { clickEditorWave (host, ctx, 60000); },
                        [&host] { return host.audioEditorPoint ("wave", 60000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx] { clickEditorWave (host, ctx, 24000, command); },
                        [&host, &track] { return editorShows (host, track, 48000); }, "the click did not focus the first-listed region" });
    steps->push_back ({ 150, [&host, &ctx] { clickEditorWave (host, ctx, 60000, rightButton); },
                        [&host] { const auto selection = host.audioEditorSelection(); return selection.size() == 5 && selection[4] == 1; },
                        "the Cmd-click did not select the earlier region" });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Join selected regions"), "the menu has no Join selected regions"); } });
    steps->push_back ({ 150, [&host, &ctx, &track]
    {
        const auto focus = editorFocus (host, track);
        ctx.expect (focus && focus->timelineStart == 0 && focus->lengthInSamples == kJoinedEnd,
                    "the join left the editor on a region other than the joined one");
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[4] == 0, "the join left a region selected beside the joined one");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled");
    }, [&track] { return track.regions.size() == 2; }, "Join selected regions did not join the two regions" });
    steps->push_back ({ 150, [&ctx, &engine, &track]
    {
        ctx.expect (track.regions.size() == 1 && track.regions[0].timelineStart == kJoinedEnd,
                    "Delete after the join did not remove the joined region alone");
        ctx.expect (undoDescription (engine) == "Delete region", "the delete is not one \"Delete region\" step");
    }, [&track] { return track.regions.size() == 1; }, "Delete after the join removed nothing" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorJoinFocusesJoined { Scenario {
    "gui.audio_editor_join_focuses_joined", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorJoinFocusesJoined (host, ctx); }
} };

// The edit cursor keeps its place on the timeline when the focused region comes to
// play another file, and when the Cut tool moves the focus: Reverse leaves a paste
// where the cursor was, and a Cut click on a region moved ahead of its audio leaves it
// inside that region, never before the timeline's start.
std::optional<ScenarioResult> runAudioEditorCursorKeepsItsPlace (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kReversedStart = 24000;
    static constexpr std::int64_t kMovedOffset = 72000;
    static constexpr std::int64_t kCutAt = 12000;
    if (auto early = beginSpansCase (host, ctx, { { kReversedStart, 48000 }, { kMovedOffset, kTakeCaseLength } }))
        return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    // The second region plays the end of the take from the timeline's start.
    track.regions[1].timelineStart = 0;
    const auto folder = ctx.tempDir() / "cursor";
    std::filesystem::create_directories (folder);
    applySessionDirectory (session, folder);
    ctx.cleanup ([&session, mode = session.editMode] { session.editMode = mode; });
    const auto clipboard = engine.getRegionClipboard();
    ctx.cleanup ([&engine, clipboard] { engine.getRegionClipboard() = clipboard; });
    const auto cursor = std::make_shared<std::int64_t> (0);
    const auto press = [&host, &ctx] (const char* key)
    { ctx.expect (host.pressAudioEditorKey (key), std::string (key) + " was not handled"); };
    const auto pastedAt = [&track] (std::size_t count)
    { return track.regions.size() == count ? track.regions.back().timelineStart : std::int64_t { -1 }; };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { clickEditorWave (host, ctx, 36000); },
                        [&host] { return host.audioEditorPoint ("wave", 36000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx, cursor]
    {
        const auto view = host.audioEditorView();
        if (! ctx.expect (view.size() == 3 && std::abs (view[2] - 36000.0) < 1000.0,
                          "the click did not put the edit cursor in the first region")) return;
        *cursor = (std::int64_t) view[2];
        ctx.expect (host.clickAudioEditorButton ("Reverse"), "no Reverse button");
    }, [&host, &track] { return editorShows (host, track, kReversedStart); }, "the click did not focus the first region" });
    steps->push_back ({ 150, [&host, &ctx, cursor, press]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && (std::int64_t) view[2] == *cursor - kReversedStart,
                    "Reverse left the edit cursor on the old file's sample");
        press ("command + C");
        press ("command + V");
    }, [&track] { return track.regions[0].reversedFrom.has_value(); }, "Reverse did not reverse the region" });
    steps->push_back ({ 150, [&ctx, &host, &session, &engine, cursor, pastedAt]
    {
        ctx.expect (pastedAt (3) == *cursor, "a paste after Reverse did not land where the edit cursor stood");
        ctx.expect (undoDescription (engine) == "Paste region", "the paste is not one \"Paste region\" step");
        session.editMode = EditMode::Cut;
        clickEditorWave (host, ctx, kCutAt);
    }, [&track] { return track.regions.size() == 3; }, "Cmd+V after Reverse pasted nothing" });
    // The cursor stood past the piece the Cut click focuses, so it goes to that piece's end.
    const auto pieceEnd = std::make_shared<std::int64_t> (-1);
    steps->push_back ({ 150, [&ctx, &host, &session, &track, press, pieceEnd]
    {
        session.editMode = EditMode::Grab;
        const auto focus = editorFocus (host, track);
        if (! ctx.expect (focus && focus->lengthInSamples > 0 && focus->lengthInSamples < 24000,
                          "the Cut click did not split the moved region")) return;
        *pieceEnd = focus->lengthInSamples;
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && (std::int64_t) view[2] == kMovedOffset + *pieceEnd,
                    "the Cut tool's focus change did not keep the edit cursor inside the region it focused");
        press ("command + V");
    }, [&host, &track] { return track.regions.size() == 4 && editorShows (host, track, 0); },
       "the Cut click did not split the moved region and focus its first piece" });
    steps->push_back ({ 150, [&ctx, pastedAt, pieceEnd]
    { ctx.expect (pastedAt (5) == *pieceEnd, "a paste after the Cut tool's focus change did not land at the edit cursor"); },
       [&track] { return track.regions.size() == 5; }, "Cmd+V after the Cut click pasted nothing" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorCursorKeepsItsPlace { Scenario {
    "gui.audio_editor_cursor_keeps_its_place", { "gui", "keyboard", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorCursorKeepsItsPlace (host, ctx); }
} };

// A region fixture for the editor cases below: one mono region of a steady level over
// the first two seconds of track 1, its own file, naming no take.
std::optional<ScenarioResult> beginEditorRegionCase (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    const auto id = addLevelTake (ctx, track, "Fixture", 0, kTakeCaseLength, 0.5f);
    if (! id) return ScenarioResult::fail ("could not write region fixture");
    auto region = regionFromTake (track.takes.front(), 0, kTakeCaseLength);
    track.takes.clear();
    if (! region) return ScenarioResult::fail ("could not cut the region fixture");
    region->takeId = 0;
    track.regions.push_back (*region);
    ctx.session().audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");
    return std::nullopt;
}

// Double-clicking the title types a label for the region, and double-clicking the gain
// or the fade readout types a value for it; each is one undo step.
std::optional<ScenarioResult> runAudioEditorFields (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto region = [&track] { return track.regions.empty() ? AudioRegion() : track.regions[0]; };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto typeInto = [&host, &ctx, &steps] (const char* control, const char* text)
    {
        const auto click = [&host, &ctx, control]
        { ctx.expect (host.doubleClickAudioEditorButton (control), std::string ("no ") + control + " to double-click"); };
        steps->push_back ({ 100, click });
        steps->push_back ({ 150, [&host, &ctx, text] { ctx.expect (host.typeInAudioEditor (text), "typing into the field failed"); } });
        steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("return"), "Enter was not delivered"); } });
    };
    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; },
                        "the editor never laid out" });
    typeInto ("Title", "Verse");
    steps->push_back ({ 150, [&ctx, &engine, region]
    {
        ctx.expect (region().label.toStdString() == "Verse", "the title did not become the region's label");
        ctx.expect (undoDescription (engine) == "Rename region", "the label is not the \"Rename region\" step");
    } });
    typeInto ("Gain", "-6");
    steps->push_back ({ 150, [&ctx, &engine, region]
    {
        ctx.expect (std::abs (region().gainDb + 6.0f) < 1.0e-4f, "the typed gain did not set -6 dB");
        ctx.expect (undoDescription (engine) == "Set region gain", "the gain is not the \"Set region gain\" step");
    } });
    typeInto ("Fades", "10 / 20");
    steps->push_back ({ 150, [&host, &ctx, &engine, region]
    {
        const auto r = region();
        ctx.expect ((r.fadeInSamples == 480 || r.fadeInSamples == 441) && r.fadeOutSamples == 2 * r.fadeInSamples,
                    "the typed fades did not set 10 ms in and 20 ms out");
        ctx.expect (undoDescription (engine) == "Set region fades", "the fades are not the \"Set region fades\" step");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the fades");
    } });
    steps->push_back ({ 150, [&host, &ctx, region]
    {
        ctx.expect (region().fadeInSamples == 0 && region().fadeOutSamples == 0, "Undo did not take the fades back");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the gain");
    } });
    steps->push_back ({ 150, [&host, &ctx, region]
    {
        ctx.expect (std::abs (region().gainDb) < 1.0e-4f, "Undo did not take the gain back");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable after the label");
    } });
    steps->push_back ({ 150, [&ctx, region]
    { ctx.expect (region().label.toStdString().empty(), "Undo did not take the label back"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorFields { Scenario {
    "gui.audio_editor_fields", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorFields (host, ctx); }
} };

std::optional<ScenarioResult> runAudioEditorNumericFields (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    track.regions[0].gainDb = -6.0f;
    track.regions[0].fadeInSamples = 480;
    track.regions[0].fadeOutSamples = 960;
    const auto original = track.regions;
    const auto region = [&track] { return track.regions.empty() ? AudioRegion() : track.regions[0]; };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto typeInto = [&host, &ctx, steps] (const char* control, const char* text)
    {
        steps->push_back ({ 100, [&host, &ctx, control]
        { ctx.expect (host.doubleClickAudioEditorButton (control), std::string ("no ") + control + " to double-click"); } });
        steps->push_back ({ 100, [&host, &ctx, text]
        { ctx.expect (host.typeInAudioEditor (text), "typing into the numeric field failed"); } });
        steps->push_back ({ 100, [&host, &ctx]
        { ctx.expect (host.pressAudioEditorKey ("return"), "Enter was not delivered to the numeric field"); } });
    };
    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; },
                        "the editor never laid out" });
    typeInto ("Title", "Sentinel");
    steps->push_back ({ 100, [&host, &ctx, &engine]
    {
        ctx.expect (undoDescription (engine) == "Rename region", "the undo sentinel was not recorded");
        ctx.expect (host.clickAudioEditorButton ("Undo"), "the undo sentinel could not be undone");
    } });
    for (const auto& [control, text] : std::vector<std::pair<const char*, const char*>> {
             { "Gain", "-" }, { "Gain", "." }, { "Gain", "1..2" }, { "Gain", "junk-12junk" },
             { "Gain", "-6 dB tail" }, { "Gain", "1e309" }, { "Gain", "nan" }, { "Gain", "inf" },
             { "Gain", "1,5" }, { "Gain", "dB-3" },
             { "Fades", "-" }, { "Fades", "." }, { "Fades", "1..2 / 20" }, { "Fades", "junk / 20" },
             { "Fades", "10 / junk" }, { "Fades", "10 / 20 / 30" }, { "Fades", "/ 20" },
             { "Fades", "10 /" }, { "Fades", "10 / nan" }, { "Fades", "10 / inf" },
             { "Fades", "1e309 / 20" }, { "Fades", "10 / 20 tail" },
             { "Fades", "fadefade 10 / 20 ms" }, { "Fades", "10 / 20 msms" } })
    {
        typeInto (control, text);
        steps->push_back ({ 100, [&ctx, &engine, &track, original, control, text]
        {
            const auto label = std::string (control) + " '" + text + "'";
            ctx.expect (sameRegions (track.regions, original), label + " changed the region");
            ctx.expect (! track.regions.empty()
                            && std::abs (track.regions[0].gainDb - original[0].gainDb) < 1.0e-4f
                            && track.regions[0].fadeInSamples == original[0].fadeInSamples
                            && track.regions[0].fadeOutSamples == original[0].fadeOutSamples,
                        label + " changed the gain or fades");
            auto& undo = engine.getUndoManager();
            ctx.expect (! undo.canUndo(), label + " added an undo step");
            ctx.expect (undo.canRedo() && undo.getRedoDescription().toStdString() == "Rename region",
                        label + " changed the redo history");
        } });
    }
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickAudioEditorButton ("Redo"), "the redo sentinel was lost after invalid input"); } });
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (region().label.toStdString() == "Sentinel", "the preserved redo no longer renames the region"); } });
    typeInto ("Gain", " -3.0 dB ");
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (std::abs (region().gainDb + 3.0f) < 1.0e-4f, "the gain readout's dB syntax was refused"); } });
    typeInto ("Fades", " fade 15 / 25 ms ");
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (region().fadeInSamples == 720 && region().fadeOutSamples == 1200,
                  "the fade readout's unit syntax was refused"); } });
    typeInto ("Fades", "0.0312499999");
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (region().fadeInSamples == 1 && region().fadeOutSamples == 0,
                  "a single fade value lost double precision or kept the old fade-out"); } });
    typeInto ("Fades", "-10 / -20");
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (region().fadeInSamples == 0 && region().fadeOutSamples == 0, "negative fades were not clamped to zero"); } });
    typeInto ("Fades", "1e300 / 20");
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (region().fadeInSamples == kTakeCaseLength && region().fadeOutSamples == 0,
                  "a huge finite fade-in did not clamp to the region length"); } });
    typeInto ("Fades", "10 / 1e300");
    steps->push_back ({ 100, [&ctx, region]
    { ctx.expect (region().fadeInSamples == 480 && region().fadeOutSamples == kTakeCaseLength - 480,
                  "a huge finite fade-out did not clamp to the remaining region length"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorNumericFields { Scenario {
    "gui.audio_editor_numeric_fields", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorNumericFields (host, ctx); }
} };

// The waveform's right-click menu, clicked through the editor: split, cut range and
// join, gain and fades, mute, lock and what it disables, and Reverse from the menu
// and from the toolbar.
std::optional<ScenarioResult> runAudioEditorMenus (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto folder = ctx.tempDir() / "menus";
    std::filesystem::create_directories (folder);
    applySessionDirectory (session, folder);
    ctx.cleanup ([&session, mode = session.editMode] { session.editMode = mode; });
    const auto original = track.regions.front();
    const auto region = [&track] { return track.regions.empty() ? AudioRegion() : track.regions[0]; };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto rightClick = [&host, &ctx] (std::int64_t sample)
    {
        return [&host, &ctx, sample]
        {
            const auto at = host.audioEditorPoint ("wave", sample);
            if (! ctx.expect (at.size() == 2, "editor geometry unavailable")) return;
            constexpr int rightButton = 4;
            host.audioEditorPointer (at[0], at[1], true, rightButton);
            host.audioEditorPointer (at[0], at[1], false, rightButton);
        };
    };
    const auto pick = [&host, &ctx] (std::string item)
    { return [&host, &ctx, item] { ctx.expect (host.clickContextMenuItem (item), "the menu has no " + item); }; };
    const auto undone = [&host, &ctx] { ctx.expect (host.clickAudioEditorButton ("Undo"), "Undo unavailable"); };
    const auto stepIs = [&engine] (std::string name) { return [&engine, name] { return undoDescription (engine) == name; }; };
    const auto menuItem = [&steps, rightClick, pick] (std::int64_t sample, const char* item)
    {
        steps->push_back ({ 150, rightClick (sample) });
        steps->push_back ({ 150, pick (item) });
    };

    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; },
                        "the editor never laid out" });
    menuItem (24000, "Split at edit cursor");
    steps->push_back ({ 150, undone, [&track] { return track.regions.size() == 2; }, "Split at edit cursor did not split the region" });
    steps->push_back ({ 150, [&track] { track.regions.front().gainDb = -3.0f; },
                        [&track] { return track.regions.size() == 1; }, "Undo did not join the split back" });
    menuItem (24000, "Reset gain (0 dB)");
    steps->push_back ({ 150, [&track]
    {
        track.regions.front().fadeInSamples = 4800;
        track.regions.front().fadeOutSamples = 9600;
    }, [region] { return std::abs (region().gainDb) < 1.0e-4f; }, "Reset gain did not bring the gain to 0 dB" });
    menuItem (24000, "Reset fades");
    steps->push_back ({ 150, [] {}, [region] { return region().fadeInSamples == 0 && region().fadeOutSamples == 0; },
                        "Reset fades did not clear the fades" });
    menuItem (24000, "Mute");
    steps->push_back ({ 150, [] {}, [region] { return region().muted; }, "Mute did not mute the region" });
    menuItem (24000, "Unmute");
    steps->push_back ({ 150, [] {}, [region] { return ! region().muted; }, "Unmute did not unmute the region" });
    menuItem (24000, "Lock");
    steps->push_back ({ 150, rightClick (24000), [region] { return region().locked; }, "Lock did not lock the region" });
    steps->push_back ({ 150, [&host, &ctx]
    {
        ctx.expect (! host.clickContextMenuItem ("Reverse"), "Reverse was offered on a locked region");
        ctx.expect (host.clickContextMenuItem ("Unlock"), "the menu has no Unlock");
    } });
    steps->push_back ({ 150, [] {}, [region] { return ! region().locked; }, "Unlock did not unlock the region" });
    menuItem (24000, "Reverse");
    steps->push_back ({ 150, undone, [region, original] { return region().file != original.file; },
                        "Reverse from the menu did not give the region a reversed file" });
    steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.clickAudioEditorButton ("Reverse"), "no Reverse button"); },
                        [region, original] { return region().file == original.file; }, "Undo did not take the reverse back" });
    steps->push_back ({ 150, undone, [stepIs, region, original] { return stepIs ("Reverse region")() && region().file != original.file; },
                        "the Reverse button did not reverse the region" });
    steps->push_back ({ 150, [&host, &ctx, &session]
    {
        session.editMode = EditMode::Range;
        const auto from = host.audioEditorPoint ("wave", 24000);
        const auto to = host.audioEditorPoint ("wave", 48000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "editor geometry unavailable")) return;
        host.audioEditorPointer (from[0], from[1], true);
        host.audioEditorPointer (to[0], to[1], true);
        host.audioEditorPointer (to[0], to[1], false);
    }, [region, original] { return region().file == original.file; }, "Undo did not take the second reverse back" });
    menuItem (36000, "Cut range");
    steps->push_back ({ 150, [&session, undone]
    {
        session.editMode = EditMode::Grab;
        undone();
    }, [&track] { return track.regions.size() == 2 && track.regions[0].lengthInSamples + track.regions[1].lengthInSamples < 80000; },
       "Cut range did not cut the range out of the region" });
    menuItem (24000, "Split at edit cursor");
    steps->push_back ({ 150, [&host, &ctx]
    {
        const auto at = host.audioEditorPoint ("wave", 72000);
        if (! ctx.expect (at.size() == 2, "editor geometry unavailable")) return;
        constexpr int command = 2;
        host.audioEditorPointer (at[0], at[1], true, command);
        host.audioEditorPointer (at[0], at[1], false, command);
    }, [&track] { return track.regions.size() == 2 && track.regions[0].lengthInSamples + track.regions[1].lengthInSamples == kTakeCaseLength; },
       "Undo did not bring the cut range back, or the split did not split" });
    menuItem (12000, "Join selected regions");
    steps->push_back ({ 150, [] {}, [&track] { return track.regions.size() == 1 && track.regions[0].lengthInSamples == kTakeCaseLength; },
                        "Join selected regions did not join the two halves" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorMenus { Scenario {
    "gui.audio_editor_menus", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorMenus (host, ctx); }
} };

// The Properties menu, clicked through the editor: a label, lock and unlock, a colour
// and Delete region.
std::optional<ScenarioResult> runAudioEditorPropertiesMenu (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    const auto region = [&track] { return track.regions.empty() ? AudioRegion() : track.regions[0]; };
    const auto pick = [&host, &ctx] (std::string item)
    { return [&host, &ctx, item] { ctx.expect (host.clickContextMenuItem (item), "the menu has no " + item); }; };
    auto steps = std::make_shared<std::vector<Step>>();
    const auto properties = [&steps, &host, &ctx, pick] (const char* item)
    {
        steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.clickAudioEditorButton ("Properties"), "no Properties button"); } });
        steps->push_back ({ 150, pick (item) });
    };

    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; },
                        "the editor never laid out" });
    properties ("Add label...");
    steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.typeInAudioEditor ("Hook"), "typing into the label field failed"); } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("return"), "Enter was not delivered"); } });
    properties ("Lock region");
    steps->push_back ({ 150, [] {}, [region] { return region().label.toStdString() == "Hook" && region().locked; },
                        "Add label... and Lock region did not label and lock the region" });
    properties ("Unlock region");
    properties ("Color");
    steps->push_back ({ 150, pick ("Red"), [region] { return ! region().locked; }, "Unlock region did not unlock the region" });
    steps->push_back ({ 150, [] {}, [region] { return region().customColour.getARGB() == 0xffd05f5fu; }, "Color > Red did not colour the region" });
    properties ("Delete region");
    steps->push_back ({ 150, [&ctx, &track]
    { ctx.expect (track.regions.empty(), "Delete region left the region on the track"); },
    [&host] { return ! host.audioEditorOpen(); }, "deleting the only region did not close the editor" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorPropertiesMenu { Scenario {
    "gui.audio_editor_properties_menu", { "gui", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorPropertiesMenu (host, ctx); }
} };

// The waveform's right-click menu loops what is selected: the focused region alone
// ("Loop region"), or a range when one is drawn ("Loop selection"). Either turns
// looping on and puts the playhead at the loop's start.
std::optional<ScenarioResult> runAudioEditorLoopSelection (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&session, &transport, mode = session.editMode, enabled = transport.isLoopEnabled(),
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  playhead = transport.getPlayhead()]
    {
        session.editMode = mode;
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (enabled);
        transport.setPlayhead (playhead);
    });
    transport.setLoopEnabled (false);
    transport.setPlayhead (60000);
    session.editMode = EditMode::Grab;

    const auto rightClick = [&host, &ctx] (std::int64_t sample)
    {
        return [&host, &ctx, sample]
        {
            const auto at = host.audioEditorPoint ("wave", sample);
            if (! ctx.expect (at.size() == 2, "editor geometry unavailable")) return;
            constexpr int rightButton = 4;
            host.audioEditorPointer (at[0], at[1], true, rightButton);
            host.audioEditorPointer (at[0], at[1], false, rightButton);
        };
    };
    const auto pick = [&host, &ctx] (std::string item)
    { return [&host, &ctx, item] { ctx.expect (host.clickContextMenuItem (item), "the menu has no " + item); }; };
    const auto loops = [&transport] (std::int64_t from, std::int64_t to)
    {
        return [&transport, from, to]
        {
            return transport.isLoopEnabled() && std::abs (transport.getLoopStart() - from) < 256
                && std::abs (transport.getLoopEnd() - to) < 256 && transport.getPlayhead() == transport.getLoopStart();
        };
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, rightClick (24000),
                        [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, pick ("Loop region") });
    steps->push_back ({ 150, [&host, &ctx, &session]
    {
        session.editMode = EditMode::Range;
        const auto from = host.audioEditorPoint ("wave", 24000);
        const auto to = host.audioEditorPoint ("wave", 48000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "editor geometry unavailable")) return;
        host.audioEditorPointer (from[0], from[1], true);
        host.audioEditorPointer (to[0], to[1], true);
        host.audioEditorPointer (to[0], to[1], false);
    }, loops (0, kTakeCaseLength), "Loop region did not loop the region with the playhead at its start" });
    steps->push_back ({ 150, rightClick (36000) });
    steps->push_back ({ 150, pick ("Loop selection") });
    steps->push_back ({ 150, [] {}, loops (24000, 48000), "Loop selection did not loop the range" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorLoopSelection { Scenario {
    "gui.audio_editor_loop_selection", { "gui", "editor", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorLoopSelection (host, ctx); }
} };

// = and - zoom the editor in and out a step about the edit cursor and 0 fits the track,
// typed at the editor's child.
std::optional<ScenarioResult> runAudioEditorZoomKeys (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    auto fitted = std::make_shared<std::vector<double>>();
    const auto zoom = [&host] { const auto view = host.audioEditorView(); return view.size() == 3 ? view[0] : 0.0; };
    const auto press = [&host, &ctx] (const char* key)
    { return [&host, &ctx, key] { ctx.expect (host.pressAudioEditorKey (key), std::string (key) + " was not delivered"); }; };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, fitted, press]
    {
        *fitted = host.audioEditorView();
        press ("=")();
    }, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, press ("="), [zoom, fitted] { return fitted->size() == 3 && zoom() > (*fitted)[0] * 1.1; },
                        "= did not zoom in" });
    steps->push_back ({ 150, press ("-"), [zoom, fitted] { return zoom() > (*fitted)[0] * 1.3; }, "a second = did not zoom in further" });
    steps->push_back ({ 150, press ("0"), [zoom, fitted] { return zoom() < (*fitted)[0] * 1.2 && zoom() > (*fitted)[0] * 1.1; },
                        "- did not zoom back out a step" });
    steps->push_back ({ 150, [&ctx, &host, fitted]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && std::abs (view[0] - (*fitted)[0]) < 1.0e-9 && std::abs (view[1] - (*fitted)[1]) < 0.5,
                    "0 did not fit the track again");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorZoomKeys { Scenario {
    "gui.audio_editor_zoom_keys", { "gui", "keyboard", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorZoomKeys (host, ctx); }
} };

// The editor never zooms past one sample a pixel: it opens on a region a few samples
// long at that zoom, and = holds it there.
std::optional<ScenarioResult> runAudioEditorTinyRegionZoom (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    if (! addLevelTake (ctx, track, "Fixture", 0, kTakeCaseLength, 0.5f))
        return ScenarioResult::fail ("could not write region fixture");
    auto region = regionFromTake (track.takes.front(), 1000, 1008);
    track.takes.clear();
    if (! region) return ScenarioResult::fail ("could not cut the region fixture");
    region->takeId = 0;
    track.regions.push_back (*region);
    ctx.session().audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto zoomAtMost = [&host, &ctx] (const std::string& when)
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && view[0] > 0.0 && view[0] <= 1.0 + 1.0e-6,
                    when + " the editor shows " + (view.size() == 3 ? std::to_string (view[0]) : std::string ("no"))
                        + " pixels a sample");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, zoomAtMost]
    {
        zoomAtMost ("opened on an 8-sample region,");
        ctx.expect (host.pressAudioEditorKey ("="), "= was not delivered");
    }, [&host] { return host.audioEditorPoint ("wave", 1004).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, [zoomAtMost] { zoomAtMost ("after =,"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorTinyRegionZoom { Scenario {
    "gui.audio_editor_tiny_region_zoom", { "gui", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorTinyRegionZoom (host, ctx); }
} };

// A region two hours long, past where zoom out stops, still fits the editor's width
// whole, as wide as a two-second one does; - holds that fit and 0 comes back to it.
std::optional<ScenarioResult> runAudioEditorLongRegionFits (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kLong = 48000LL * 60 * 60 * 2;
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& track = ctx.session().track (0);
    if (! addLevelTake (ctx, track, "Fixture", 0, kTakeCaseLength, 0.5f))
        return ScenarioResult::fail ("could not write region fixture");
    auto region = regionFromTake (track.takes.front(), 0, kTakeCaseLength);
    track.takes.clear();
    if (! region) return ScenarioResult::fail ("could not cut the region fixture");
    region->takeId = 0;
    auto longRegion = *region;
    longRegion.timelineStart = kTakeCaseLength;
    longRegion.lengthInSamples = kLong;
    track.regions = { *region, longRegion };
    ctx.session().audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto fitWidth = std::make_shared<double> (0.0);
    const auto fitsWhole = [&host, &ctx, fitWidth] (const std::string& when)
    {
        const auto view = host.audioEditorView();
        if (! ctx.expect (view.size() == 3, "editor view unavailable")) return;
        const auto width = view[0] * static_cast<double> (kLong);
        ctx.expect (std::abs (width / *fitWidth - 1.0) < 0.01,
                    when + " the two-hour region spans " + std::to_string (width) + " pixels, not the "
                        + std::to_string (*fitWidth) + " a short region fits");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, fitWidth]
    {
        const auto view = host.audioEditorView();
        if (! ctx.expect (view.size() == 3 && view[0] > 0.0, "editor view unavailable")) return;
        *fitWidth = view[0] * static_cast<double> (kTakeCaseLength);
        host.closeAudioEditor();
        ctx.expect (host.openAudioEditor (0, 1), "the editor did not open on the long region");
    }, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx, fitsWhole]
    {
        fitsWhole ("opened on it,");
        ctx.expect (host.pressAudioEditorKey ("-"), "- was not delivered");
    }, [&host] { return host.audioEditorPoint ("wave", kTakeCaseLength + kLong / 2).size() == 2; },
       "the editor never laid out on the long region" });
    steps->push_back ({ 150, [&host, &ctx, fitsWhole]
    {
        fitsWhole ("after -,");
        ctx.expect (host.pressAudioEditorKey ("="), "= was not delivered");
    } });
    steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.pressAudioEditorKey ("0"), "0 was not delivered"); } });
    steps->push_back ({ 150, [fitsWhole] { fitsWhole ("after = and 0,"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorLongRegionFits { Scenario {
    "gui.audio_editor_long_region_fits", { "gui", "keyboard", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorLongRegionFits (host, ctx); }
} };

// Resizing the window keeps the editor's zoom and edit cursor; 0 then fits the
// region to the new width.
std::optional<ScenarioResult> runAudioEditorResizeKeepsZoom (GuiHost& host, ScenarioContext& ctx)
{
    const auto window = host.mainWindowSize();
    if (window.size() < 4) return ScenarioResult::fail ("the main window reported no size");
    if (auto early = beginEditorRegionCase (host, ctx)) return early;
    ctx.cleanup ([&host, window] { host.resizeMainWindow (window[0], window[1]); });
    auto fitted = std::make_shared<std::vector<double>>();
    auto zoomed = std::make_shared<std::vector<double>>();
    const auto press = [&host, &ctx] (const char* key)
    { return [&host, &ctx, key] { ctx.expect (host.pressAudioEditorKey (key), std::string (key) + " was not delivered"); }; };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, fitted]
    {
        *fitted = host.audioEditorView();
        clickEditorWave (host, ctx, 60000);
    }, [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 150, press ("=") });
    steps->push_back ({ 150, [&host, &ctx, fitted, zoomed, window]
    {
        *zoomed = host.audioEditorView();
        if (! ctx.expect (fitted->size() == 3 && zoomed->size() == 3 && (*zoomed)[0] > (*fitted)[0] * 1.1
                              && std::abs ((*zoomed)[2] - 60000.0) < 512.0,
                          "the click and = did not move the edit cursor and zoom in")) return;
        const int width = window[0] - 240 >= window[2] ? window[0] - 240 : window[0] + 240;
        ctx.expect (host.resizeMainWindow (width, window[1]), "the main window did not take the new width");
    } });
    steps->push_back ({ 400, [&host, &ctx, zoomed, press]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && std::abs (view[0] - (*zoomed)[0]) <= (*zoomed)[0] * 1.0e-6,
                    "resizing the window changed the zoom");
        ctx.expect (view.size() == 3 && std::abs (view[2] - (*zoomed)[2]) < 0.5, "resizing the window moved the edit cursor");
        press ("0")();
    } });
    steps->push_back ({ 150, [&host, &ctx, fitted]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && std::abs (view[0] / (*fitted)[0] - 1.0) > 0.02,
                    "0 after the resize did not fit the region to the new width");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorResizeKeepsZoom { Scenario {
    "gui.audio_editor_resize_keeps_zoom", { "gui", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorResizeKeepsZoom (host, ctx); }
} };

std::optional<ScenarioResult> runAudioEditorGestures (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        host.closeAudioEditor();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto source = ctx.tempDir() / "Gestures.wav";
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
    region.timelineStart = 96000;
    track.regions.push_back (region);
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");
    const auto drag = [&host, &ctx] (const std::string& kind, std::int64_t from, std::int64_t to, int dy, bool shift)
    {
        const auto start = host.audioEditorPoint (kind, from);
        auto end = host.audioEditorPoint ("wave", to);
        if (! ctx.expect (start.size() == 2 && end.size() == 2, "gesture geometry unavailable")) return;
        if (kind == "gain") end = { start[0], start[1] + dy };
        ctx.expect (host.audioEditorPointer (start[0], start[1], true, shift), "gesture down failed");
        host.audioEditorPointer (end[0], end[1], true, shift);
        host.audioEditorPointer (end[0], end[1], false, shift);
    };
    const auto checkRegion = [&ctx, &session] (std::int64_t offset, std::int64_t length, float gain)
    {
        const auto& regions = session.track (0).regions;
        if (! ctx.expect (regions.size() == 2, "gesture unexpectedly changed region count")) return;
        ctx.expect (std::abs (regions[0].sourceOffset - offset) < 128
                    && std::abs (regions[0].timelineStart - offset) < 128
                    && std::abs (regions[0].lengthInSamples - length) < 256, "trim did not preserve the source slice");
        ctx.expect (std::abs (regions[0].gainDb - gain) < 0.01f, "gain drag produced wrong level");
    };
    const auto navigate = [&host, &ctx] (bool next)
    {
       #if defined (__APPLE__)
        const std::string modifier = "command + ";
       #else
        const std::string modifier = "ctrl + ";
       #endif
        ctx.expect (host.pressPeerKey (modifier + (next ? "]" : "["), next ? ']' : '['), "navigation key was not handled");
    };
    const auto selected = [&host, &ctx] (int expected)
    {
        const auto state = host.audioEditorSelection();
        ctx.expect (state.size() == 5 && state[0] == expected, "navigation did not select expected region");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [drag] { drag ("start", 0, 12000, 0, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 36000, 0); drag ("end", 48000, 36000, 0, false); } });
    // A handle goes back out past where an earlier trim left it.
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 0); drag ("end", 36000, 42000, 0, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 30000, 0); drag ("end", 42000, 36000, 0, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 0); drag ("gain", 24000, 24000, -60, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 6); drag ("gain", 24000, 24000, -100, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 12); drag ("gain", 24000, 24000, 400, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, -24); drag ("wave", 18000, 30000, 0, true); } });
    steps->push_back ({ 150, [&host, &ctx, navigate]
    {
        const auto state = host.audioEditorSelection();
        ctx.expect (state.size() == 5 && state[1] == 1 && std::abs (state[2] - 18000) < 128
                    && std::abs (state[3] - 30000) < 128, "Shift drag did not select expected range");
        navigate (true);
    } });
    steps->push_back ({ 350, [selected, navigate] { selected (1); navigate (false); } });
    steps->push_back ({ 350, [selected, &host, &ctx]
    { selected (0); ctx.expect (host.pressPeerKey ("delete", 0), "Delete key was not handled"); } });
    steps->push_back ({ 300, [&session, &ctx, &host, read, source, originalBytes]
    {
        const auto& regions = session.track (0).regions;
        ctx.expect (regions.size() == 1 && regions[0].timelineStart == 96000, "Delete did not remove focused region");
        ctx.expect (read (source) == originalBytes, "gestures modified source audio");
        ctx.expect (host.pressPeerKey ("delete", 0), "second Delete key was not handled");
    } });
    steps->push_back ({ 350, [&session, &ctx, &host]
    {
        ctx.expect (session.track (0).regions.empty(), "Delete did not remove final region");
        ctx.expect (host.audioEditorSelection().empty(), "empty editor did not close");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorGestures { Scenario {
    "gui.audio_editor_gestures", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorGestures (host, ctx); }
} };

// A range belongs to the region it was taken on: a Grab or Cut click that focuses
// another region drops it rather than carrying it over.
std::optional<ScenarioResult> runAudioEditorFocusDropsRange (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto modeWas = session.editMode;
    ctx.cleanup ([&host, &session, originalDir, restore, modeWas]
    {
        drainModals (host);
        host.closeAudioEditor();
        session.editMode = modeWas;
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto source = ctx.tempDir() / "FocusRange.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    std::vector<float> signal (48000, 0.5f);
    const float* channels[] = { signal.data() };
    if (! writer || ! writer->write (channels, 1, 48000) || ! writer->flush())
        return ScenarioResult::fail ("could not write editor fixture");
    writer.reset();
    auto& track = session.track (0);
    track.frozen.store (false);
    track.regions.clear();
    AudioRegion region;
    region.file = decltype (region.file) (source.string());
    region.lengthInSamples = 48000;
    track.regions.push_back (region);
    region.timelineStart = 60000;
    track.regions.push_back (region);
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto press = [&host, &ctx] (std::int64_t from, std::int64_t to)
    {
        const auto start = host.audioEditorPoint ("wave", from);
        const auto end = host.audioEditorPoint ("wave", to);
        if (! ctx.expect (start.size() == 2 && end.size() == 2, "the waveform has no geometry")) return;
        ctx.expect (host.audioEditorPointer (start[0], start[1], true), "the press was not taken");
        host.audioEditorPointer (end[0], end[1], true);
        host.audioEditorPointer (end[0], end[1], false);
    };
    const auto selection = [&host, &ctx] (int focused, bool range, const std::string& failure)
    {
        const auto state = host.audioEditorSelection();
        const auto view = host.audioEditorView();
        ctx.expect (state.size() == 5 && state[0] == focused && (state[1] != 0) == range,
                    failure + " (focused " + (state.size() == 5 ? std::to_string (state[0]) : "-") + ", range "
                        + (state.size() == 5 ? std::to_string (state[1]) : "-") + ", pixels per sample "
                        + (view.size() == 3 ? std::to_string (view[0]) : "-") + ")");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    // Zoomed out far enough that both regions are on screen, a key a frame: presses
    // within one frame read as one.
    for (int i = 0; i < 6; ++i)
        steps->push_back ({ i == 0 ? 400 : 80, [&host, &ctx]
        { ctx.expect (host.pressAudioEditorKey ("-"), "the zoom-out key was not handled"); } });
    steps->push_back ({ 150, [&session, press] { session.editMode = EditMode::Range; press (12000, 30000); } });
    steps->push_back ({ 150, [&session, selection, press]
    {
        selection (0, true, "the Range drag did not select a range on the first region");
        session.editMode = EditMode::Grab;
        press (80000, 80000);
    } });
    steps->push_back ({ 150, [&session, selection, press]
    {
        selection (1, false, "a Grab click on another region kept the first region's range");
        session.editMode = EditMode::Range;
        press (70000, 90000);
    } });
    steps->push_back ({ 150, [&session, selection, press]
    {
        selection (1, true, "the Range drag did not select a range on the second region");
        session.editMode = EditMode::Cut;
        press (24000, 24000);
    } });
    steps->push_back ({ 150, [&session, &ctx, selection]
    {
        ctx.expect (session.track (0).regions.size() == 3, "the Cut click did not split the first region");
        selection (0, false, "a Cut click on another region kept the second region's range");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorFocusDropsRange { Scenario {
    "gui.audio_editor_focus_drops_range", { "gui", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorFocusDropsRange (host, ctx); }
} };

// A Shift drag over [from, to) of the focused region, as one range selection.
void selectEditorRange (GuiHost& host, ScenarioContext& ctx, std::int64_t from, std::int64_t to)
{
    static constexpr int shift = 1;
    const auto start = host.audioEditorPoint ("wave", from);
    const auto end = host.audioEditorPoint ("wave", to);
    if (! ctx.expect (start.size() == 2 && end.size() == 2, "editor geometry unavailable")) return;
    host.audioEditorPointer (start[0], start[1], true, shift);
    host.audioEditorPointer (end[0], end[1], true, shift);
    host.audioEditorPointer (end[0], end[1], false, shift);
}

void expectEditorRange (GuiHost& host, ScenarioContext& ctx, bool range, const std::string& failure)
{
    const auto selection = host.audioEditorSelection();
    ctx.expect (selection.size() == 5 && selection[0] == 0 && (selection[1] != 0) == range, failure);
}

// A range goes once its region covers none of it. The menu's Split at edit cursor,
// left of a range, leaves the range's audio in the new piece beside the focused one:
// the range is dropped, and Delete takes the focused piece, never the one beside it.
std::optional<ScenarioResult> runAudioEditorSplitDropsRange (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginSpansCase (host, ctx, { { 0, kTakeCaseLength } })) return early;
    auto& track = ctx.session().track (0);
    static constexpr int rightButton = 4;
    auto split = std::make_shared<std::int64_t> (0);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { selectEditorRange (host, ctx, 60000, 72000); },
                        [&host] { return host.audioEditorTakeLanes().size() == 1; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx]
    {
        expectEditorRange (host, ctx, true, "the Shift drag did not select a range");
        clickEditorWave (host, ctx, 24000, rightButton);
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Split at edit cursor"), "the waveform menu has no Split at edit cursor"); } });
    steps->push_back ({ 150, [&host, &ctx, &track, split]
    {
        if (ctx.expect (track.regions.size() == 2, "Split at edit cursor did not split the region"))
            *split = track.regions[1].sourceOffset;
        expectEditorRange (host, ctx, false, "the split left the range on a piece that covers none of it");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled");
    } });
    steps->push_back ({ 200, [&ctx, &track, split]
    {
        ctx.expect (track.regions.size() == 1 && track.regions[0].sourceOffset == *split
                        && track.regions[0].lengthInSamples == kTakeCaseLength - *split,
                    "Delete after the split took the piece the range had been on");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorSplitDropsRange { Scenario {
    "gui.audio_editor_split_drops_range", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorSplitDropsRange (host, ctx); }
} };

// A trim that takes the region's end back past a range drops the range, so Delete
// takes the focused region and leaves the locked region stored after it alone.
std::optional<ScenarioResult> runAudioEditorTrimDropsRange (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr std::int64_t kFocused = 0;
    static constexpr std::int64_t kLocked = 60000;
    if (auto early = beginSpansCase (host, ctx, { { kFocused, 48000 }, { kLocked, kTakeCaseLength } })) return early;
    auto& track = ctx.session().track (0);
    track.regions[1].locked = true;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx] { selectEditorRange (host, ctx, 36000, 44000); },
                        [&host] { return host.audioEditorTakeLanes().size() == 1; }, "the editor never laid out" });
    steps->push_back ({ 150, [&host, &ctx]
    {
        expectEditorRange (host, ctx, true, "the Shift drag did not select a range");
        const auto from = host.audioEditorPoint ("end", 48000);
        const auto to = host.audioEditorPoint ("wave", 24000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "trim geometry unavailable")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the trim handle did not take the press");
        host.audioEditorPointer (to[0], to[1], true);
        host.audioEditorPointer (to[0], to[1], false);
    } });
    steps->push_back ({ 150, [&host, &ctx, &track]
    {
        const auto* focused = regionPlaying (track, kFocused);
        ctx.expect (focused != nullptr && std::abs (focused->lengthInSamples - 24000) < 256,
                    "the trim did not take the region's end back past the range");
        expectEditorRange (host, ctx, false, "the trim left the range past the region's end");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not handled");
    } });
    steps->push_back ({ 200, [&ctx, &track]
    {
        const auto* locked = regionPlaying (track, kLocked);
        ctx.expect (locked != nullptr && locked->locked && locked->lengthInSamples == kTakeCaseLength - kLocked,
                    "Delete after the trim took the locked region stored after the focused one");
        ctx.expect (regionPlaying (track, kFocused) == nullptr && track.regions.size() == 1,
                    "Delete after the trim did not take the focused region");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorTrimDropsRange { Scenario {
    "gui.audio_editor_trim_drops_range", { "gui", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorTrimDropsRange (host, ctx); }
} };

// With no audio device running the editor still reads time at a real rate, the one
// the device last ran at, and its ruler numbers only as many bars or stamps as it has
// room for: over a track of takes alone, ten minutes end to end, a few of each rather
// than one a bar, also where a tempo map runs four times as fast as the session tempo.
std::optional<ScenarioResult> runAudioEditorRulerWithoutDevice (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const double rate = engine.getCurrentSampleRate();
    if (host.engineDetached() || ! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    auto& track = session.track (0);
    const auto span = (std::int64_t) rate * 600;
    if (! addLevelTake (ctx, track, "Take 1", 0, kTakeCaseLength, kWholeTakeLevel)
        || ! addLevelTake (ctx, track, "Take 2", span - kTakeCaseLength, kTakeCaseLength, kShortTakeLevel))
        return ScenarioResult::fail ("could not write take fixture");
    ctx.keep (session.timeDisplayMode);
    ctx.keep (session.tempoBpm);
    ctx.cleanup ([&session, points = session.tempoMap.points()] { session.tempoMap.setPoints (points); });
    session.timeDisplayMode.store ((int) TimeDisplayMode::Bars);
    session.tempoMap.setPoints ({});
    ctx.cleanup ([&engine] { if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback(); });
    engine.detachAudioCallback();
    if (engine.getCurrentSampleRate() > 0.0)
        return ScenarioResult::fail ("a detached engine still reads a running rate");
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto checkRuler = [&host, &ctx, rate, span] (const std::string& marks)
    {
        const auto ruler = host.audioEditorRuler();
        const auto view = host.audioEditorView();
        if (! ctx.expect (ruler.size() == 2 && view.size() == 3, "the editor reported no ruler")) return;
        ctx.expect (std::abs (ruler[1] - rate) < 0.5,
                    "with no device running the editor reads time at " + std::to_string (ruler[1])
                        + " Hz, not the " + std::to_string (rate) + " Hz the device last ran at");
        // The whole track is in view, and a mark needs 64 design pixels of it.
        const double room = view[0] * (double) span / 64.0 + 2.0;
        ctx.expect (ruler[0] >= 2.0 && ruler[0] <= room,
                    "the ruler drew " + std::to_string ((int) ruler[0]) + " " + marks + " where "
                        + std::to_string ((int) room) + " fit");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&session, checkRuler]
    {
        checkRuler ("bar numbers");
        session.timeDisplayMode.store ((int) TimeDisplayMode::Time);
    }, [&host] { return host.audioEditorTakeLanes().size() == 2; }, "the take lanes never showed" });
    steps->push_back ({ 300, [&session, checkRuler, span]
    {
        checkRuler ("time stamps");
        session.timeDisplayMode.store ((int) TimeDisplayMode::Bars);
        session.tempoBpm.store (60.0f);
        session.tempoMap.setPoints ({ { 0, 60.0f }, { span / 2, 240.0f } });
    } });
    steps->push_back ({ 300, [checkRuler] { checkRuler ("bar numbers under a faster tempo map"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorRulerWithoutDevice { Scenario {
    "gui.audio_editor_ruler_without_device", { "gui", "editor", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorRulerWithoutDevice (host, ctx); }
} };

std::optional<ScenarioResult> runAudioEditorToolbar (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        host.closeAudioEditor();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto source = ctx.tempDir() / "Toolbar.wav";
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
    auto initialView = std::make_shared<std::vector<double>>();
    auto split = std::make_shared<std::int64_t> (0);
    const auto gain = [&ctx, &session] (float expected)
    {
        const auto& regions = session.track (0).regions;
        if (ctx.expect (regions.size() == 1, "expected one region"))
            ctx.expect (std::abs (regions[0].gainDb - expected) < 0.001f, "toolbar did not apply expected gain");
    };
    const auto click = [&host, &ctx] (const std::string& name)
    { ctx.expect (host.clickAudioEditorButton (name), "audio editor button unavailable: " + name); };
    const float normalized = 20.0f * std::log10 (0.99f / 0.5f);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, initialView, click]
    { *initialView = host.audioEditorView(); click ("Normalize"); } });
    steps->push_back ({ 150, [gain, normalized, click] { gain (normalized); click ("Undo"); } });
    steps->push_back ({ 150, [gain, click] { gain (0.0f); click ("Redo"); } });
    steps->push_back ({ 150, [gain, normalized, click] { gain (normalized); click ("Normalize"); } });
    steps->push_back ({ 150, [gain, normalized, &host, &ctx, &session]
    {
        const auto& regions = session.track (0).regions;
        ctx.expect (regions.size() == 1 && std::abs (regions[0].gainDb - normalized) < 0.001f,
                    "a second Normalize changed the gain the first one set");
        gain (normalized);
        ctx.expect (host.clickAudioEditorSample (24000), "waveform click failed");
    } });
    steps->push_back ({ 150, [&host, &ctx, split, click]
    {
        const auto view = host.audioEditorView();
        if (ctx.expect (view.size() == 3, "editor view unavailable")) *split = (std::int64_t) view[2];
        ctx.expect (*split > 20000 && *split < 28000, "waveform did not place an interior cursor");
        click ("Split");
    } });
    steps->push_back ({ 150, [&session, &ctx, split, click]
    {
        const auto& regions = session.track (0).regions;
        if (ctx.expect (regions.size() == 2, "split button did not split region"))
            ctx.expect (regions[0].lengthInSamples == *split && regions[1].timelineStart == *split
                        && regions[1].sourceOffset == *split && regions[1].lengthInSamples == 48000 - *split,
                        "split did not preserve contiguous source slices");
        click ("Undo");
    } });
    steps->push_back ({ 150, [gain, normalized, click] { gain (normalized); click ("Properties"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Mute region"), "Properties did not expose mute"); } });
    steps->push_back ({ 150, [&session, &ctx, click]
    { ctx.expect (! session.track (0).regions.empty() && session.track (0).regions[0].muted, "Properties mute failed"); click ("Undo"); } });
    steps->push_back ({ 150, [click] { click ("Zoom in"); } });
    steps->push_back ({ 150, [&host, &ctx, initialView, click]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && initialView->size() == 3 && view[0] > (*initialView)[0], "zoom in did not enlarge waveform");
        click ("Zoom out");
    } });
    steps->push_back ({ 150, [&host, &ctx, initialView, click]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && initialView->size() == 3 && std::abs (view[0] - (*initialView)[0]) < 0.00001,
                    "zoom out did not reverse zoom in");
        click ("Zoom in");
    } });
    steps->push_back ({ 150, [click] { click ("Zoom fit"); } });
    steps->push_back ({ 150, [&host, &ctx, &session, initialView, read, source, originalBytes]
    {
        const auto view = host.audioEditorView();
        ctx.expect (view.size() == 3 && initialView->size() == 3 && std::abs (view[0] - (*initialView)[0]) < 0.00001
                    && std::abs (view[1] - (*initialView)[1]) < 0.5, "zoom fit did not restore fitted view");
        ctx.expect (! session.track (0).regions.empty() && ! session.track (0).regions[0].muted, "Undo did not restore mute");
        ctx.expect (read (source) == originalBytes, "editor modified the source file");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorToolbar { Scenario {
    "gui.audio_editor_toolbar", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorToolbar (host, ctx); }
} };

std::optional<ScenarioResult> runAudioEditorLifecycle (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    const bool expanded = host.timelineViewMatches (true);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &track, expanded, regions = track.regions]
    {
        host.closeAudioEditor();
        track.regions = regions;
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    ctx.cleanup (host.preserveKeyboardFocus());
    const auto path = ctx.tempDir() / "editor.wav";
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the audio editor fixture")) return ctx.verdict();
    std::vector<float> silence (192000, 0.0f);
    const float* channels[] { silence.data() };
    if (! ctx.expect (writer->write (channels, 1, 192000), "could not write the audio editor fixture"))
        return ctx.verdict();
    writer.reset();
    AudioRegion region;
    region.file = decltype (region.file) (path.u8string().c_str());
    region.lengthInSamples = 192000;
    track.regions = { region };
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    if (! expanded) host.pressKey ("T", 't');
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool escape : { true, false })
    {
        steps->push_back ({ 100, [&host, &ctx]
        { ctx.expect (host.doubleClickAudioRegion (0, 0), "the audio region is not visible for double-click"); } });
        steps->push_back ({ 300, [&host, &ctx, escape]
        {
            ctx.expect (host.audioEditorOpen(), "double-click did not open the audio editor");
            ctx.expect (escape ? host.pressAudioEditorKey ("escape") : host.clickOutsideAudioEditor(),
                        "the audio editor dismissal gesture was unavailable");
        } });
        steps->push_back ({ 300, [&host, &ctx]
        { ctx.expect (! host.audioEditorOpen(), "the dismissal gesture left the audio editor open"); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorLifecycle { Scenario {
    "gui.audio_editor_lifecycle", { "gui", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorLifecycle (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
