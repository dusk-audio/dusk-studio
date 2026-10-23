#include "../GuiScenarioSupport.h"
#include "../../FourKColours.h"
#include "../../../engine/PluginSlot.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/ParamEditAction.h"
#include "../../../session/RegionEditActions.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
// Double-clicking the strip's name renames the track; right-clicking it offers
// the track colours, and the one picked also colours the track's regions.
std::optional<ScenarioResult> runTrackNameAndColour (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    auto& track = ctx.session().track (0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Channel, 0, "name", 2, false),
                    "the strip name did not take a double-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey ("command + A"), "the name editor did not accept Select All");
        for (const char character : std::string ("vox"))
            ctx.expect (host.pressPeerKey (std::string (1, character), character), "the name editor rejected a character");
        ctx.expect (host.pressPeerKey ("return"), "the name editor did not accept Return");
    } });
    steps->push_back ({ 200, [&host, &ctx, &track]
    {
        ctx.expect (track.name == "vox", "double-clicking the name did not rename the track");
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Channel, 0, "name", 1, true),
                    "the strip name did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        const std::vector<std::string> colours { "Red", "Orange", "Amber", "Green", "Cyan", "Blue", "Purple", "Tan" };
        const auto items = host.contextMenuItems();
        const auto header = std::find (items.begin(), items.end(), "Track colour");
        ctx.expect (items.end() - header > 9 && std::vector<std::string> (header + 1, header + 9) == colours
                    && header[9] == "-",
                    "the colour menu does not offer exactly the eight track colours");
        ctx.expect (host.clickContextMenuItem ("Blue"), "the track colour menu has no Blue");
    } });
    steps->push_back ({ 200, [&host, &ctx, &track]
    {
        ctx.expect (track.colour.getARGB() == fourKColors::kHpfBlue, "Blue did not colour the track");
        ctx.expect (host.tapeRegionColour (0, 0) == fourKColors::kHpfBlue, "the track's region did not take its colour");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar trackNameAndColour { Scenario {
    "gui.track_name_and_colour", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTrackNameAndColour (host, ctx); }
} };

// Clicking a track's name in the tape strip selects the track at once and,
// once the click cannot become a double-click, brings its strip into the
// mixer on its own page. Double-clicking a name renames the track in place: a
// click on another name, Return and an emptied name commit, Escape does not,
// and the rename undoes.
std::optional<ScenarioResult> runTimelineTrackName (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    // The page has to go back before seedTapeRegion's session reopen replaces
    // the console: the new one would otherwise follow the surface bank this
    // case published, one poll after that cleanup restored the page.
    ctx.cleanup (host.preserveKeyboardFocus());
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    const int pages = host.consolePageCount();
    if (pages < 2) return ScenarioResult::skip ("requires a console with more than one page");
    static constexpr int first = Session::kNumTracks - 2;
    static constexpr int last = Session::kNumTracks - 1;
    auto& session = ctx.session();
    // Content keeps both rows in the strip whichever page is up. On the first
    // page they follow that page's tracks; on their own page they follow track
    // 1 and whatever of that page comes before them, which is never the same
    // number of rows, so paging moves them.
    session.track (first).regions = { region };
    session.track (last).regions = { region };
    session.track (first).name = "keys";
    const auto lastName = session.track (last).name.toStdString();
    auto view = host.tapeView();
    if (view.size() != 7) return ScenarioResult::fail ("the tape strip is missing");
    view[5] = 0.0;
    host.restoreTapeView (view);
    const auto firstPageY = std::make_shared<int> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.tapeView()[5] < 0.5, "the tape strip still shows every track");
        // Content set straight into the session reaches the strip's rows on its
        // next poll, but its height only on the window's next layout, which a
        // page change runs.
        ctx.expect (host.pressKey ("2", '2') && host.pressKey ("1", '1') && host.consolePageMatches (0),
                    "the console did not go to its first page");
        ctx.expect (host.clickTapeTrackName (first, 1), "the track's name is not in the tape strip");
        ctx.expect (host.tapeSelectedTrack() == first, "clicking the name did not select the track");
        ctx.expect (host.consolePageMatches (0), "the click paged the console before it could become a double-click");
    } });
    steps->push_back ({ 700, [&host, &ctx, pages, firstPageY]
    {
        ctx.expect (host.consolePageMatches (pages - 1), "clicking the name did not show the track's console page");
        ctx.expect (host.consoleFocusedStrip() == first, "clicking the name did not focus the track's strip");
        ctx.expect (host.tapeNameEditorTrack() < 0, "a single click opened the name editor");
        ctx.expect (host.pressKey ("1", '1') && host.consolePageMatches (0), "the console did not go back to its first page");
        *firstPageY = host.tapeTrackRowY (last);
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickTapeTrackName (last, 2), "the name did not take a double-click"); } });
    steps->push_back ({ 200, [&host, &ctx, pages, firstPageY]
    {
        ctx.expect (host.tapeSelectedTrack() == last && host.consolePageMatches (pages - 1)
                    && host.consoleFocusedStrip() == last,
                    "the double-click did not select the track and page the console to it");
        ctx.expect (*firstPageY >= 0 && host.tapeTrackRowY (last) >= 0 && host.tapeTrackRowY (last) != *firstPageY,
                    "paging did not move the double-clicked row, so the layout proves nothing");
        if (! ctx.expect (host.tapeNameEditorTrack() == last, "the double-click did not open the editor on its row")) return;
        ctx.expect (host.pressPeerKey ("command + A"), "the name editor did not accept Select All");
        for (const char character : std::string ("bass"))
            ctx.expect (host.pressPeerKey (std::string (1, character), character), "the name editor rejected a character");
        ctx.expect (host.clickTapeTrackName (first, 1), "another name did not take a click during the edit");
    } });
    // Checked before the click pages the console, whose focus would select
    // the track again and hide a selection the rename's undo step dropped.
    steps->push_back ({ 200, [&host, &ctx, &session]
    {
        ctx.expect (session.track (last).name == "bass", "clicking another name did not commit the rename");
        ctx.expect (host.tapeNameEditorTrack() < 0, "clicking another name left the editor open");
        ctx.expect (host.tapeSelectedTrack() == first, "the click that committed the rename did not keep its own track selected");
    } });
    steps->push_back ({ 500, [&host, &ctx]
    {
        ctx.expect (host.consoleFocusedStrip() == first, "the click that committed the rename did not focus its track's strip");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 200, [&host, &ctx, &session, lastName]
    {
        ctx.expect (session.track (last).name.toStdString() == lastName, "Undo did not restore the track name");
        ctx.expect (host.tapeSelectedTrack() == first, "Undo dropped the selected track");
        ctx.expect (host.clickTapeTrackName (first, 2), "the name did not take a double-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        if (! ctx.expect (host.tapeNameEditorTrack() == first, "the double-click did not open the editor")) return;
        ctx.expect (host.pressPeerKey ("x", 'x'), "the name editor rejected a character");
        ctx.expect (host.pressPeerKey ("escape"), "the name editor did not accept Escape");
    } });
    steps->push_back ({ 200, [&host, &ctx, &session]
    {
        ctx.expect (host.tapeNameEditorTrack() < 0, "Escape left the name editor open");
        ctx.expect (session.track (first).name == "keys", "Escape renamed the track");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickTapeTrackName (first, 2), "the name did not take a double-click"); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        if (! ctx.expect (host.tapeNameEditorTrack() == first, "the double-click did not open the editor")) return;
        ctx.expect (host.pressPeerKey ("command + A") && host.pressPeerKey ("backspace"), "the name editor did not clear");
        ctx.expect (host.pressPeerKey ("return"), "the name editor did not accept Return");
    } });
    steps->push_back ({ 200, [&host, &ctx, &session]
    {
        ctx.expect (host.tapeNameEditorTrack() < 0, "Return left the name editor open");
        ctx.expect (session.track (first).name == std::to_string (first + 1).c_str(),
                    "an emptied name did not fall back to the track number");
        ctx.expect (host.pressKey ("1", '1') && host.consolePageMatches (0), "the console did not go back to its first page");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.clickTapeTrackName (first, 1), "the track's name is not in the tape strip");
        ctx.expect (host.clickAudioRegion (last, 0), "the other track's region did not take a click");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.tapeSelectedTrack() == last, "a name click still waiting to page took the selection back");
        ctx.expect (host.consolePageMatches (0), "a name click paged the console after another track was picked");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineTrackName { Scenario {
    "gui.timeline_track_name", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineTrackName (host, ctx); }
} };

// Shift and Cmd/Ctrl on the tape strip's track names build a multi-track
// selection without paging the console, Shift by shown row so a hidden track
// between stays out. The names' right-click menu then edits every region on
// those tracks, audio and MIDI, one undo step per edit, rebuilding playback
// once and publishing a MIDI track's regions once per edit and per undo; a
// frozen track sits the menu out. A / S / X turn their state off on every
// selected track when any has it on, and on otherwise.
std::optional<ScenarioResult> runTimelineTrackMultiselect (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    if (host.consolePageCount() < 2) return ScenarioResult::skip ("requires a console with more than one page");
    static constexpr int first = Session::kNumTracks - 4;
    static constexpr int hidden = Session::kNumTracks - 3;
    static constexpr int second = Session::kNumTracks - 2;
    static constexpr int midi = Session::kNumTracks - 1;
    static constexpr std::uint32_t blue = 0xff6090d0;
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    session.track (first).regions = { region };
    session.track (second).regions = { region };
    MidiRegion notes;
    notes.timelineStart = region.timelineStart;
    notes.lengthInTicks = 7680;
    notes.lengthInSamples = session.ticksToSamples (notes.lengthInTicks, ctx.engine().getCurrentSampleRate());
    notes.notes = { { 1, 60, 100, 0, 480 } };
    // Two on the one track, so an edit that published per region would show.
    MidiRegion later = notes;
    later.timelineStart = notes.timelineStart + notes.lengthInSamples;
    session.track (midi).mode.store ((int) Track::Mode::Midi);
    session.track (midi).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { notes, later }));
    auto view = host.tapeView();
    if (view.size() != 7) return ScenarioResult::fail ("the tape strip is missing");
    view[5] = 0.0;
    host.restoreTapeView (view);

    const std::vector<int> all { first, second, midi };
    const auto picked = [&host, &ctx] (std::vector<int> expected, const std::string& failure)
    {
        const auto tracks = host.tapeSelectedTracks();
        std::string got;
        for (const int t : tracks) got += " " + std::to_string (t);
        ctx.expect (tracks == expected, failure + " (selected:" + got + ")");
    };
    // Each region on the three tracks, and the seeded one on track 0 that
    // nothing here may touch.
    const auto regionsHold = [&ctx, &session] (std::function<bool (const AudioRegion&)> audio,
                                               std::function<bool (const MidiRegion&)> notesHold,
                                               const std::string& failure)
    {
        bool held = session.track (0).regions.size() == 1;
        for (const int t : { first, second })
            held = held && session.track (t).regions.size() == 1 && audio (session.track (t).regions.front());
        const auto& live = session.track (midi).midiRegions.current();
        held = held && live.size() == 2;
        for (const auto& r : live) held = held && notesHold (r);
        ctx.expect (held, failure);
    };
    const auto trackZeroUntouched = [&ctx, &session] (const std::string& failure)
    {
        const auto& seeded = session.track (0).regions;
        ctx.expect (seeded.size() == 1 && ! seeded.front().muted && ! seeded.front().locked
                    && seeded.front().customColour.isTransparent(), failure);
    };
    // Playback rebuilds and MIDI-track publishes since the last mark.
    auto marks = std::make_shared<std::array<std::uint64_t, 2>>();
    const auto mark = [&ctx, &session, marks]
    {
        *marks = { ctx.engine().getPlaybackEngine().rebuildCount(), session.track (midi).midiRegions.generation() };
    };
    const auto costs = [&ctx, &session, marks, mark] (std::uint64_t rebuilds, std::uint64_t publishes, const std::string& what)
    {
        const auto rebuilt = ctx.engine().getPlaybackEngine().rebuildCount() - (*marks)[0];
        const auto published = session.track (midi).midiRegions.generation() - (*marks)[1];
        ctx.expect (rebuilt == rebuilds && published == publishes,
                    what + " rebuilt playback " + std::to_string (rebuilt) + " times and published the MIDI track "
                    + std::to_string (published) + " times");
        mark();
    };
    auto steps = std::make_shared<std::vector<Step>>();
    const auto menu = [&host, &ctx, steps, mark] (int track, std::vector<std::string> path)
    {
        steps->push_back ({ 300, [&host, &ctx, track, mark]
        {
            mark();
            ctx.expect (host.clickTapeTrackName (track, 1, 4), "the track's name did not take a right-click");
        } });
        for (const auto& item : path)
            steps->push_back ({ 200, [&host, &ctx, item]
            { ctx.expect (host.clickContextMenuItem (item), "the track name menu has no " + item); } });
    };
    const auto undo = [&host, &ctx, steps, mark]
    {
        steps->push_back ({ 300, [&host, &ctx, mark]
        {
            mark();
            ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
        } });
    };

    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pressKey ("2", '2') && host.pressKey ("1", '1') && host.consolePageMatches (0),
                    "the console did not go to its first page");
    } });
    steps->push_back ({ 300, [&host, &ctx, picked]
    {
        ctx.expect (host.tapeTrackRowY (hidden) < 0 && host.tapeTrackRowY (first) >= 0 && host.tapeTrackRowY (midi) >= 0,
                    "the rows around the hidden track are not laid out as the range needs");
        ctx.expect (host.clickTapeTrackName (first, 1), "the first name did not take a click");
        picked ({ first }, "a plain click did not select just its track");
        ctx.expect (host.clickTapeTrackName (midi, 1, 1), "the name did not take a Shift-click");
        picked ({ first, second, midi }, "Shift did not select the shown rows from the anchor");
        ctx.expect (host.clickTapeTrackName (second, 1, 2), "the name did not take a Cmd-click");
        picked ({ first, midi }, "Cmd did not take its track out of the selection");
    } });
    // Past the double-click window: a second press on the same name inside it
    // is a double-click, not another toggle.
    steps->push_back ({ 700, [&host, &ctx, picked]
    {
        ctx.expect (host.clickTapeTrackName (second, 1, 2), "the name did not take a Cmd-click");
        picked ({ first, second, midi }, "Cmd did not put its track back into the selection");
        ctx.expect (host.tapeSelectedTrack() == second, "the last clicked name is not the primary selection");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.consolePageMatches (0), "a modified name click paged the console"); } });
    // A track lit by its selected region carries into a Cmd-click selection.
    steps->push_back ({ 300, [&host, &ctx, picked]
    {
        ctx.expect (host.clickAudioRegion (first, 0), "the region did not take a click");
        picked ({ first }, "a selected region did not light its track");
        ctx.expect (host.clickTapeTrackName (midi, 1, 2), "the name did not take a Cmd-click");
        picked ({ first, midi }, "Cmd dropped the track its selected region lit");
        ctx.expect (host.clickTapeTrackName (second, 1, 2), "the name did not take a Cmd-click");
        picked ({ first, second, midi }, "Cmd did not add its track");
    } });

    steps->push_back ({ 300, [&host, &ctx, mark]
    {
        mark();
        ctx.expect (host.clickTapeTrackName (second, 1, 4), "the track's name did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx, picked, all]
    {
        const std::vector<std::string> expected { "3 tracks", "Loop region span", "Split at playhead", "Reverse regions",
                                                  "-", "Mute regions", "Lock regions", "-", "Color", "-", "Delete regions" };
        const auto items = host.contextMenuItems();
        std::string got;
        for (const auto& item : items) got += " | " + item;
        ctx.expect (items == expected, "the track name menu reads:" + got);
        picked (all, "right-clicking a selected name changed the selection");
        ctx.expect (host.clickContextMenuItem ("Color"), "the track name menu has no Color");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Blue"), "the Color submenu has no Blue"); } });
    steps->push_back ({ 300, [regionsHold, trackZeroUntouched, costs]
    {
        regionsHold ([] (const AudioRegion& r) { return r.customColour.getARGB() == blue; },
                     [] (const MidiRegion& r) { return r.customColour.getARGB() == blue; },
                     "Color did not tint every region on the selected tracks");
        trackZeroUntouched ("Color tinted a region on a track that is not selected");
        costs (1, 1, "Color");
    } });
    undo();
    steps->push_back ({ 300, [regionsHold, picked, all, costs]
    {
        regionsHold ([] (const AudioRegion& r) { return r.customColour.isTransparent(); },
                     [] (const MidiRegion& r) { return r.customColour.isTransparent(); },
                     "one Undo did not take back the whole recolour");
        picked (all, "Undo dropped the track selection");
        costs (1, 1, "undoing Color");
    } });

    menu (first, { "Mute regions" });
    steps->push_back ({ 300, [regionsHold, trackZeroUntouched, costs]
    {
        regionsHold ([] (const AudioRegion& r) { return r.muted; }, [] (const MidiRegion& r) { return r.muted; },
                     "Mute regions did not mute every region on the selected tracks");
        trackZeroUntouched ("Mute regions muted a region on a track that is not selected");
        costs (1, 1, "Mute regions");
    } });
    undo();
    steps->push_back ({ 300, [regionsHold, costs]
    {
        regionsHold ([] (const AudioRegion& r) { return ! r.muted; }, [] (const MidiRegion& r) { return ! r.muted; },
                     "one Undo did not take back the whole mute");
        costs (1, 1, "undoing Mute regions");
    } });

    menu (midi, { "Lock regions" });
    steps->push_back ({ 300, [&host, &ctx, regionsHold, costs]
    {
        regionsHold ([] (const AudioRegion& r) { return r.locked; }, [] (const MidiRegion& r) { return r.locked; },
                     "Lock regions did not lock every region on the selected tracks");
        costs (1, 1, "Lock regions");
        ctx.expect (host.clickTapeTrackName (first, 1, 4), "the track's name did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (! host.contextMenuItemEnabled ("Delete regions"), "Delete regions is offered with every region locked");
        ctx.expect (host.contextMenuItemEnabled ("Unlock regions"), "a locked selection is not offered Unlock regions");
        host.closeTopModal();
    } });
    undo();
    steps->push_back ({ 300, [&host, &ctx, regionsHold, costs]
    {
        regionsHold ([] (const AudioRegion& r) { return ! r.locked; }, [] (const MidiRegion& r) { return ! r.locked; },
                     "one Undo did not take back the whole lock");
        costs (1, 1, "undoing Lock regions");
        ctx.expect (host.clickAudioRegion (second, 0, true), "the region did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Lock region"), "the region menu has no Lock region"); } });
    menu (first, { "Delete regions" });
    steps->push_back ({ 300, [&ctx, &session, trackZeroUntouched, costs]
    {
        ctx.expect (session.track (first).regions.empty() && session.track (midi).midiRegions.current().empty(),
                    "Delete regions did not delete the unlocked regions");
        ctx.expect (session.track (second).regions.size() == 1 && session.track (second).regions.front().locked,
                    "Delete regions deleted a locked region");
        trackZeroUntouched ("Delete regions touched a track that is not selected");
        costs (1, 1, "Delete regions");
    } });
    undo();
    steps->push_back ({ 300, [&ctx, &session, notes, later, costs]
    {
        const auto& live = session.track (midi).midiRegions.current();
        ctx.expect (session.track (first).regions.size() == 1 && live.size() == 2,
                    "one Undo did not bring back every deleted region");
        ctx.expect (live.size() == 2 && live[0].timelineStart == notes.timelineStart
                    && live[1].timelineStart == later.timelineStart,
                    "Undo did not put the MIDI regions back in their order");
        costs (1, 1, "undoing Delete regions");
    } });
    undo();
    steps->push_back ({ 300, [&ctx, &session]
    { ctx.expect (! session.track (second).regions.front().locked, "Undo did not unlock the region the region menu locked"); } });

    menu (second, { "Loop region span" });
    steps->push_back ({ 300, [&ctx, &transport, region, later]
    {
        const auto end = std::max (region.timelineStart + region.lengthInSamples, later.timelineStart + later.lengthInSamples);
        ctx.expect (transport.isLoopEnabled() && transport.getLoopStart() == region.timelineStart
                    && transport.getLoopEnd() == end && transport.getPlayhead() == region.timelineStart,
                    "Loop region span did not loop from the earliest start to the latest end");
        transport.locate (region.timelineStart + region.lengthInSamples / 4);
    } });
    menu (second, { "Split at playhead" });
    steps->push_back ({ 300, [&ctx, &session, costs]
    {
        ctx.expect (session.track (first).regions.size() == 2 && session.track (second).regions.size() == 2
                    && session.track (0).regions.size() == 1,
                    "Split at playhead did not split the selected tracks' regions, and only those");
        costs (1, 0, "Split at playhead");
    } });
    undo();
    steps->push_back ({ 300, [&ctx, &session, costs]
    {
        ctx.expect (session.track (first).regions.size() == 1 && session.track (second).regions.size() == 1,
                    "one Undo did not take back the whole split");
        costs (1, 0, "undoing Split at playhead");
    } });
    menu (second, { "Reverse regions" });
    steps->push_back ({ 300, [regionsHold, trackZeroUntouched, region, costs]
    {
        regionsHold ([region] (const AudioRegion& r) { return r.file != region.file; },
                     [] (const MidiRegion&) { return true; },
                     "Reverse regions did not reverse every audio region on the selected tracks");
        trackZeroUntouched ("Reverse regions changed a track that is not selected");
        costs (1, 0, "Reverse regions");
    } });
    undo();
    steps->push_back ({ 300, [regionsHold, region, costs]
    {
        regionsHold ([region] (const AudioRegion& r) { return r.file == region.file; },
                     [] (const MidiRegion&) { return true; },
                     "one Undo did not take back the whole reverse");
        costs (1, 0, "undoing Reverse regions");
    } });

    // A frozen track sits the menu out. Its four muted, unlocked regions would
    // otherwise turn the label to Unmute and offer Split and Delete over the
    // locked regions of the other two.
    steps->push_back ({ 300, [&host, &ctx, &session, region, mark]
    {
        auto& frozen = session.track (second);
        frozen.frozen.store (true);
        frozen.regions.clear();
        for (int i = 0; i < 4; ++i)
        {
            auto copy = region;
            copy.timelineStart += i * region.lengthInSamples;
            copy.muted = true;
            frozen.regions.push_back (copy);
        }
        session.track (first).regions.front().locked = true;
        session.track (midi).midiRegions.mutate ([] (std::vector<MidiRegion>& regions)
        {
            for (auto& r : regions) r.locked = true;
        });
        mark();
        ctx.expect (host.clickTapeTrackName (first, 1, 4), "the track's name did not take a right-click");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        const auto items = host.contextMenuItems();
        ctx.expect (std::find (items.begin(), items.end(), "Mute regions") != items.end(),
                    "a frozen track's muted regions turned the label to Unmute regions");
        ctx.expect (! host.contextMenuItemEnabled ("Split at playhead") && ! host.contextMenuItemEnabled ("Delete regions")
                    && ! host.contextMenuItemEnabled ("Reverse regions"),
                    "a frozen track's regions are offered to Split, Reverse or Delete");
        ctx.expect (host.clickContextMenuItem ("Mute regions"), "the track name menu has no Mute regions");
    } });
    steps->push_back ({ 300, [&ctx, &session, costs]
    {
        const auto& frozen = session.track (second).regions;
        const auto& live = session.track (midi).midiRegions.current();
        bool thawedMuted = session.track (first).regions.front().muted && live.size() == 2;
        for (const auto& r : live) thawedMuted = thawedMuted && r.muted;
        ctx.expect (thawedMuted, "Mute regions did not mute the thawed tracks' regions");
        ctx.expect (frozen.size() == 4 && std::all_of (frozen.begin(), frozen.end(),
                                                       [] (const AudioRegion& r) { return r.muted && ! r.locked; }),
                    "Mute regions changed a frozen track");
        costs (1, 1, "Mute regions beside a frozen track");
    } });
    steps->push_back ({ 300, [&session, region]
    {
        auto& frozen = session.track (second);
        frozen.frozen.store (false);
        frozen.regions = { region };
    } });

    steps->push_back ({ 300, [&host, &ctx, picked]
    {
        ctx.expect (host.clickTapeTrackName (0, 1, 4), "the track's name did not take a right-click");
        picked ({ 0 }, "right-clicking a name outside the selection did not select just that track");
        const auto items = host.contextMenuItems();
        ctx.expect (! items.empty() && items.front() == "Track 1", "the menu does not name the one track it acts on");
        host.closeTopModal();
    } });
    // Past the double-click window, so the plain click does not rename. The
    // Shift-clicked audio track is the last clicked, and a headless device may
    // give it no input to arm with.
    steps->push_back ({ 700, [&host, &ctx, picked, all]
    {
        ctx.expect (host.clickTapeTrackName (midi, 1) && host.clickTapeTrackName (first, 1, 1),
                    "the names did not take the clicks");
        picked (all, "the selection for the keys did not form");
        ctx.expect (host.tapeSelectedTrack() == first, "the Shift-clicked name is not the primary selection");
    } });
    for (const char key : { 'S', 'X', 'A' })
    {
        const auto state = [&session, key] (int t)
        {
            auto& track = session.track (t);
            return key == 'S' ? track.strip.solo.load() : key == 'X' ? track.strip.mute.load() : track.recordArmed.load();
        };
        // Only the MIDI track on, which arming never refuses, and the last
        // clicked track off: the key turns it off everywhere.
        steps->push_back ({ 200, [&ctx, &session, state, key]
        {
            if (key == 'S') session.setTrackSoloed (midi, true);
            else if (key == 'X') session.track (midi).strip.mute.store (true);
            else session.setTrackArmed (midi, true);
            ctx.expect (state (midi) && ! state (first), std::string (1, key) + " could not start from a mixed selection");
        } });
        for (const bool on : { false, true, false })
        {
            steps->push_back ({ 200, [&host, &ctx, key]
            {
                const char lower = (char) (key - 'A' + 'a');
                ctx.expect (host.pressKey (std::string (1, key), lower), std::string (1, key) + " was not handled");
            } });
            steps->push_back ({ 200, [&ctx, &session, state, key, on, all]
            {
                bool together = ! state (0);
                for (const int t : all)
                {
                    // Arming refuses an audio track with no input to record from,
                    // which the device a headless run opens may not offer.
                    const bool armable = key != 'A' || session.missingInputForTrack (t) == Session::kInputAvailable;
                    together = together && state (t) == (on && armable);
                }
                ctx.expect (together, std::string (1, key) + (on ? " did not switch on" : " did not switch off")
                                          + " every selected track, and only those");
            } });
        }
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineTrackMultiselect { Scenario {
    "gui.timeline_track_multiselect", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineTrackMultiselect (host, ctx); }
} };

// Dragging a track's name to another row of the tape strip moves the track
// there, the tracks in between shifting a slot: its name, regions, strip and
// insert go with it, it keeps the input it followed, the selection follows it
// and the console pages to it. Several picked names move as one block in their
// order. With empty rows hidden, a drop goes right after the row above it,
// unless that would carry the dragged track past the hidden ones just above
// it. A click, even one that drifts a little, still only selects and pages.
// Escape drops the drag, a drop that moves nothing does nothing, and a running
// transport, a frozen track in the way or a plug-in still loading refuses with
// the reason. The move is one undo step, refused with the reason while the
// transport runs or a render's dialog is open, and its undo closes an editor
// open on a track it moves.
std::optional<ScenarioResult> runTimelineTrackMove (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    // Back before the session reopen, whose new console would follow the
    // surface bank this case published.
    ctx.cleanup (host.preserveKeyboardFocus());
    const int pages = host.consolePageCount();
    if (pages < 2) return ScenarioResult::skip ("requires a console with more than one page");
    static constexpr int kick = 17;
    static constexpr int frozenTrack = 5;
    static constexpr int renderTrack = 20;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    std::array<const ChannelStrip*, Session::kNumTracks> launched {};
    for (int t = 0; t < Session::kNumTracks; ++t) launched[(std::size_t) t] = &engine.getChannelStrip (t);

    auto& kickStrip = engine.getChannelStrip (kick);
    std::string error;
    engine.suspendProcessing();
    const bool loaded = kickStrip.loadBuiltin ("dusk.builtin.utility", error);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load Utility: " + error);
    ctx.cleanup ([&engine, &kickStrip]
    {
        engine.suspendProcessing();
        kickStrip.unloadBuiltin();
        engine.resumeProcessing();
    });
    // Before the session reopen seedTapeRegion set up, which would otherwise
    // land in slots the strips have left.
    ctx.cleanup ([&host, &engine, &session, &transport, launched]
    {
        drainModals (host);
        engine.stop();
        transport.setPlayhead (0);
        session.track (frozenTrack).frozen.store (false);
        if (session.track (renderTrack).frozen.load()) engine.unfreezeTrack (renderTrack);
        std::array<int, Session::kNumTracks> newToOld {};
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            int now = 0;
            while (now < Session::kNumTracks - 1 && &engine.getChannelStrip (now) != launched[(std::size_t) t]) ++now;
            newToOld[(std::size_t) t] = now;
        }
        if (const auto back = trackMoveFromNewToOld (newToOld)) engine.moveTracks (*back);
        engine.getUndoManager().clearUndoHistory();
    });

    for (int t = 0; t < kick; ++t) session.track (t).name = ("T" + std::to_string (t + 1)).c_str();
    session.track (kick).name = "kick";
    session.track (kick).inputSource.store (kInputFollowsTrack);
    auto kickRegion = region;
    kickRegion.timelineStart = region.timelineStart + region.lengthInSamples;
    session.track (kick).regions = { kickRegion };
    auto view = host.tapeView();
    if (view.size() != 7) return ScenarioResult::fail ("the tape strip is missing");
    view[5] = 0.0;
    host.restoreTapeView (view);

    // Bindings name a slot, and keep naming it whichever track lands there.
    const auto bindingsWere = session.midiBindings.current();
    ctx.cleanup ([&session, bindingsWere]
    { session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (bindingsWere)); });
    std::vector<MidiBinding> slotBindings;
    for (const auto target : { MidiBindingTarget::TrackFader, MidiBindingTarget::TrackPluginParam })
    {
        MidiBinding binding;
        binding.dataNumber = 20 + (int) slotBindings.size();
        binding.target = target;
        binding.targetIndex = 0;
        slotBindings.push_back (binding);
    }
    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (slotBindings));
    // The group master is the member in the lowest slot, so a move can change it.
    static constexpr int groupMate = 5;
    session.track (groupMate).strip.faderGroupId.store (1);
    session.track (kick).strip.faderGroupId.store (1);
    const auto groupMaster = [&host] (int strip)
    {
        std::string text;
        int master = -1;
        bool filled = false;
        host.groupChipView (strip, text, master, filled);
        return master;
    };

    // from[t] is the slot the track now at t launched in: its name and its strip.
    const auto order = [&ctx, &session, &engine, launched] (std::vector<int> from, const std::string& failure)
    {
        std::string got;
        bool held = true;
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const int was = t < (int) from.size() ? from[(std::size_t) t] : t;
            const auto expected = was == kick ? std::string ("kick") : was < kick ? "T" + std::to_string (was + 1)
                                                                                  : session.track (t).name.toStdString();
            held = held && session.track (t).name.toStdString() == expected
                        && &engine.getChannelStrip (t) == launched[(std::size_t) was];
            if (t <= kick) got += " " + session.track (t).name.toStdString();
        }
        ctx.expect (held, failure + " (tracks from 1:" + got + ")");
    };
    const auto identity = [] { std::vector<int> v; for (int t = 0; t < Session::kNumTracks; ++t) v.push_back (t); return v; };
    // The slots after a block of `moving` lands before insertBefore.
    const auto moved = [] (std::vector<int> moving, int insertBefore)
    {
        const auto plan = planBlockMove (moving, insertBefore);
        return std::vector<int> (plan.newToOld.begin(), plan.newToOld.end());
    };
    const auto picked = [&host, &ctx] (std::vector<int> expected, int primary, const std::string& failure)
    {
        const auto tracks = host.tapeSelectedTracks();
        std::string got;
        for (const int t : tracks) got += " " + std::to_string (t);
        ctx.expect (tracks == expected && host.tapeSelectedTrack() == primary,
                    failure + " (selected:" + got + ", primary " + std::to_string (host.tapeSelectedTrack()) + ")");
    };
    const auto pageShown = [&host, pages]
    {
        for (int p = 0; p < pages; ++p)
            if (host.consolePageMatches (p)) return p;
        return -1;
    };
    const auto kickPage = std::make_shared<int> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    const auto alerted = [&host, &ctx, steps] (const std::string& how, const std::string& expected)
    {
        steps->push_back ({ 300, [&host, &ctx, how, expected]
        {
            ctx.expect (host.modalText() == expected, how + ": the alert read '" + host.modalText() + "'");
            ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
        } });
    };

    // A click that drifts under the drag threshold is still a click. The kick's
    // region reaches the rows on the strip's next poll, but their height only
    // on the window's next layout, which a page change runs.
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pressKey ("2", '2') && host.pressKey ("1", '1') && host.consolePageMatches (0),
                    "the console did not go to its first page");
        ctx.expect (host.tapeTrackRowY (kick) >= 0, "the kick's row is not in the tape strip");
        ctx.expect (host.nudgeTapeTrackName (kick, 2), "the kick's name did not take a click");
    } });
    steps->push_back ({ 700, [&host, &ctx, order, identity, picked, pageShown, kickPage, groupMaster]
    {
        order (identity(), "a click moved the tracks");
        ctx.expect (groupMaster (kick) == groupMate, "the kick's fader group is not mastered by its lowest member");
        picked ({ kick }, kick, "a click did not select its track");
        *kickPage = pageShown();
        ctx.expect (*kickPage >= 0 && host.consoleFocusedStrip() == kick,
                    "a click that drifted a little did not page the console to its track");
        if (*kickPage == 0) ctx.note ("the kick's strip is on the first page, so the drop's paging proves less");
    } });

    // The kick from the last shown row to the top.
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.dragTapeTrackName (kick, 0), "the kick's name did not take a drag"); } });
    steps->push_back ({ 300, [&host, &ctx, &session, &engine, order, moved, picked, region, kickRegion, groupMaster]
    {
        ctx.expect (host.modalStackEmpty(), "the move raised '" + host.modalText() + "'");
        order (moved ({ kick }, 0), "dragging the kick to the top did not move it there and shift the rest down");
        const auto& top = session.track (0).regions;
        const auto& second = session.track (1).regions;
        ctx.expect (top.size() == 1 && top.front().timelineStart == kickRegion.timelineStart
                        && second.size() == 1 && second.front().timelineStart == region.timelineStart,
                    "the regions did not go with their tracks");
        ctx.expect (engine.getChannelStrip (0).isBuiltinLoaded() && ! engine.getChannelStrip (kick).isBuiltinLoaded(),
                    "the insert did not go with its track");
        const auto off = host.consoleStripsOffTheirSlot();
        ctx.expect (off.empty(), "the console still drives the old insert on " + std::to_string (off.size()) + " strips");
        picked ({ 0 }, 0, "the selection did not follow the moved track");
        ctx.expect (host.consolePageMatches (0) && host.consoleFocusedStrip() == 0,
                    "the drop did not page the console to the moved track");
        ctx.expect (session.track (0).inputSource.load() == kick && host.stripInputText (0) == "In 18 (fixed)",
                    "the moved track did not keep its input: the strip shows '" + host.stripInputText (0) + "'");
        ctx.expect (engine.getUndoManager().getUndoDescription() == kMoveTracksTransaction,
                    "the move is not the undo step");
        ctx.expect (groupMaster (0) == 0 && groupMaster (groupMate + 1) == 0,
                    "the moved kick, now the group's lowest member, did not become its master");
        const auto& bindings = session.midiBindings.current();
        ctx.expect (bindings.size() == 2 && bindings[0].targetIndex == 0 && bindings[1].targetIndex == 0,
                    "the move took the MIDI bindings off track 1's slot");
    } });

    // Undo is refused while playing, and the history stays.
    steps->push_back ({ 300, [&host, &ctx, &transport]
    {
        transport.setState (Transport::State::Playing);
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    alerted ("undo while playing", "Can't move tracks\nStop playback, then undo the move again.");
    steps->push_back ({ 300, [&ctx, &engine, &transport, order, moved]
    {
        order (moved ({ kick }, 0), "a refused undo moved the tracks");
        ctx.expect (engine.getUndoManager().getUndoDescription() == kMoveTracksTransaction,
                    "a refused undo dropped the move from the history");
        engine.stop();
        transport.setPlayhead (0);
    } });

    // Undo from the audio editor is refused while playing, as from the window.
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.openAudioEditor (0, 0), "the audio editor did not open on the moved region"); } });
    steps->push_back ({ 500, [&host, &ctx, &transport]
    {
        transport.setState (Transport::State::Playing);
        ctx.expect (host.pressAudioEditorKey ("command + Z"), "the audio editor did not handle Undo");
    } });
    alerted ("undo from the editor while playing", "Can't move tracks\nStop playback, then undo the move again.");
    steps->push_back ({ 300, [&ctx, &engine, &transport, order, moved]
    {
        order (moved ({ kick }, 0), "a refused undo from the editor moved the tracks");
        ctx.expect (engine.getUndoManager().getUndoDescription() == kMoveTracksTransaction,
                    "a refused undo from the editor dropped the move from the history");
        engine.stop();
        transport.setPlayhead (0);
    } });

    // Undo from an editor open on a moved track keeps it open on that track, now
    // back in its old slot; redo and undo again.
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.openAudioEditor (0, 0), "the audio editor did not open on the moved region"); } });
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + Z"), "the audio editor did not handle Undo"); } });
    steps->push_back ({ 300, [&host, &ctx, &session, order, identity, picked]
    {
        order (identity(), "Undo did not put the tracks back");
        ctx.expect (session.track (kick).inputSource.load() == kInputFollowsTrack
                        && host.stripInputText (kick) == "In 18 (follow)",
                    "Undo did not set the kick's input back to following its track: the strip shows '"
                        + host.stripInputText (kick) + "'");
        ctx.expect (host.audioEditorOpen() && host.audioEditorTrack() == kick && host.audioEditorRegion() == 0,
                    "the audio editor did not follow its track back to slot " + std::to_string (kick + 1)
                        + " (editing track " + std::to_string (host.audioEditorTrack() + 1) + ")");
        host.closeAudioEditor();
        ctx.expect (host.modalStackEmpty(), "the undo raised '" + host.modalText() + "'");
        ctx.expect (host.consoleStripsOffTheirSlot().empty(), "the console drives the wrong inserts after the undo");
        picked ({ kick }, kick, "the selection did not follow the track back");
        ctx.expect (host.consolePageMatches (0), "the undo paged the console");
        ctx.expect (host.pressKey ("command + shift + Z"), "Redo was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, order, moved]
    {
        order (moved ({ kick }, 0), "Redo did not move the kick back to the top");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [order, identity] { order (identity(), "the second undo did not put the tracks back"); } });

    // Two picked names move as one block, in their order.
    steps->push_back ({ 300, [&host, &ctx, picked]
    {
        ctx.expect (host.clickTapeTrackName (2, 1, 2), "the name did not take a Cmd-click");
        picked ({ 2, kick }, 2, "Cmd did not add the track to the selection");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.dragTapeTrackName (2, 0), "the picked name did not take a drag"); } });
    steps->push_back ({ 300, [&host, &ctx, order, moved, picked]
    {
        order (moved ({ 2, kick }, 0), "the two picked tracks did not move to the top in their order");
        picked ({ 0, 1 }, 0, "the selection did not follow the block");
        ctx.expect (host.consolePageMatches (0) && host.consoleFocusedStrip() == 0,
                    "the block drop did not page the console to the dragged track");
        ctx.expect (host.consoleStripsOffTheirSlot().empty(), "the console drives the wrong inserts after the block move");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [order, identity, picked]
    {
        order (identity(), "Undo did not take the block back");
        picked ({ 2, kick }, 2, "the selection did not follow the block back");
    } });
    // A plain click on one of several picked names narrows the pick to it.
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.nudgeTapeTrackName (kick, 1), "the kick's name did not take a click"); } });
    steps->push_back ({ 700, [&host, &ctx, order, identity, picked, kickPage]
    {
        order (identity(), "a click on a picked name moved the tracks");
        picked ({ kick }, kick, "a click on a picked name did not narrow the selection to it");
        ctx.expect (host.consolePageMatches (*kickPage), "a click on a picked name did not page the console to it");
        ctx.expect (host.pressKey ("1", '1') && host.consolePageMatches (0), "the console did not go back to its first page");
    } });

    // With empty rows hidden, a drop right after a shown row keeps the hidden
    // tracks under that row below the moved one. The first page is narrower
    // than the tracks before the kick, as the page count above requires.
    const auto rowsAbove = std::make_shared<int> (-1);
    steps->push_back ({ 700, [&host, &ctx, rowsAbove]
    {
        int shown = 0;
        for (int t = 0; t < kick; ++t) shown += host.tapeTrackRowY (t) >= 0 ? 1 : 0;
        if (! ctx.expect (shown >= 2 && shown < kick && host.tapeTrackRowY (shown - 1) >= 0
                              && host.tapeTrackRowY (shown) < 0,
                          "the first page shows " + std::to_string (shown)
                              + " rows before the kick, with no hidden track after them"))
            return;
        *rowsAbove = shown;
        ctx.expect (host.dragTapeTrackName (0, shown, false), "track 1's name did not take a drag");
        ctx.expect (host.tapeMoveLineY() >= 0, "no drop line shows between the last row before the hidden tracks and the kick");
        ctx.expect (host.releaseTapeTrackName (shown), "the drag could not be released");
    } });
    steps->push_back ({ 300, [&host, &ctx, order, moved, rowsAbove]
    {
        if (*rowsAbove < 0) return;
        order (moved ({ 0 }, *rowsAbove), "a drop after the last shown row did not land before the hidden tracks");
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, order, identity]
    {
        order (identity(), "Undo did not take the hidden-row drop back");
        ctx.expect (host.pressKey ("1", '1') && host.consolePageMatches (0), "the console did not go back to its first page");
        ctx.expect (engine.getUndoManager().getRedoDescription() == kMoveTracksTransaction,
                    "the undone move is not the redo step");
    } });
    // A press on the kick that drifts up past the drag threshold, into the gap
    // above it, drops the kick where it is: not after the shown row above, which
    // would move it over the hidden tracks, and not a step that clears history.
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.nudgeTapeTrackName (kick, -4), "the kick's name did not take a press"); } });
    steps->push_back ({ 300, [&host, &ctx, &engine, order, identity]
    {
        order (identity(), "a drift into the gap above the kick moved it past the hidden tracks");
        ctx.expect (host.modalStackEmpty(), "the drift raised '" + host.modalText() + "'");
        ctx.expect (engine.getUndoManager().getRedoDescription() == kMoveTracksTransaction,
                    "the drift cleared the undo history");
        ctx.expect (host.pressKey ("1", '1') && host.consolePageMatches (0), "the console did not go back to its first page");
    } });

    // Escape drops the drag; a drop that moves nothing shows no line and does nothing.
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.dragTapeTrackName (kick, 0, false), "the kick's name did not take a drag");
        ctx.expect (host.tapeMoveLineY() >= 0, "no drop line shows while the name is dragged");
        ctx.expect (host.pressKey ("escape"), "Escape was not handled during the drag");
        ctx.expect (host.tapeMoveLineY() < 0, "the drop line stayed after Escape");
        ctx.expect (host.releaseTapeTrackName (0), "the drag could not be released");
    } });
    steps->push_back ({ 300, [&host, &ctx, order, identity]
    {
        order (identity(), "a drag dropped by Escape moved the tracks");
        ctx.expect (host.modalStackEmpty(), "Escape left '" + host.modalText() + "'");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        int rows = 0;
        for (int t = 0; t < Session::kNumTracks; ++t) rows += host.tapeTrackRowY (t) >= 0 ? 1 : 0;
        ctx.expect (host.dragTapeTrackName (kick, rows, false), "the kick's name did not take a drag");
        ctx.expect (host.tapeMoveLineY() < 0, "a drop line shows where the kick already is");
        ctx.expect (host.releaseTapeTrackName (rows), "the drag could not be released");
    } });
    steps->push_back ({ 300, [&host, &ctx, order, identity]
    {
        order (identity(), "a drop where the kick already is moved the tracks");
        ctx.expect (host.modalStackEmpty(), "a drop that moves nothing raised '" + host.modalText() + "'");
    } });

    // Refused while a plug-in is still loading into a track in the way: the
    // load's completion would land on the slot it started in. A plug-in file
    // that is not there fails, but only once the load has come back.
    steps->push_back ({ 700, [&host, &ctx, &engine]
    {
        auto& slot = engine.getChannelStrip (0).getPluginSlot();
        PluginDescriptor missing;
        missing.name = "Missing";
        missing.formatName = "VST3";
        missing.location = (ctx.tempDir() / "Missing.vst3").string();
        slot.loadFromDescriptorAsync (missing, [] (bool, auto) {});
        if (! ctx.expect (slot.isAsyncLoadPending(), "the load came back before the drag could start")) return;
        ctx.expect (host.dragTapeTrackName (kick, 0), "the kick's name did not take a drag");
    } });
    alerted ("while a plug-in loads", "Can't move tracks\nWait for the plug-in to finish loading, then move the tracks again.");
    Step loadDone { 100, [order, identity] { order (identity(), "a move while a plug-in loads ran anyway"); } };
    loadDone.until = [&engine] { return ! engine.getChannelStrip (0).getPluginSlot().isAsyncLoadPending(); };
    loadDone.timeout = "the missing plug-in's load never came back";
    steps->push_back (std::move (loadDone));
    steps->push_back ({ 300, [&host, &ctx] { dismissAlert (host); ctx.expect (host.modalStackEmpty(), "the failed load left '" + host.modalText() + "' up"); } });

    // Refused while playing, and with a frozen track in the way.
    steps->push_back ({ 700, [&host, &ctx, &transport]
    {
        transport.setState (Transport::State::Playing);
        ctx.expect (host.dragTapeTrackName (kick, 0), "the kick's name did not take a drag");
    } });
    alerted ("while playing", "Can't move tracks\nStop playback, then move the tracks again.");
    steps->push_back ({ 300, [&session, &engine, &transport, order, identity]
    {
        order (identity(), "a move while playing ran anyway");
        engine.stop();
        transport.setPlayhead (0);
        session.track (frozenTrack).frozen.store (true);
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.dragTapeTrackName (kick, 0), "the kick's name did not take a drag"); } });
    alerted ("with a frozen track", "Can't move tracks\nUnfreeze track 6, then move the tracks again. "
                                    "A frozen track can't be moved or shifted.");
    steps->push_back ({ 300, [&session, order, identity]
    {
        order (identity(), "a move over a frozen track ran anyway");
        session.track (frozenTrack).frozen.store (false);
    } });

    // A render's dialog holds tracks by slot, so the move's undo waits for it.
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.dragTapeTrackName (kick, 0), "the kick's name did not take a drag"); } });
    steps->push_back ({ 300, [&host, &ctx, &session, order, moved, region]
    {
        order (moved ({ kick }, 0), "the kick did not move to the top again");
        session.track (renderTrack).regions = { region };
        ctx.expect (host.pressKey ("2", '2') && host.pressKey ("1", '1'), "the console did not change page");
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickTapeTrackName (renderTrack, 1), "the rendered track's name did not take a click"); } });
    steps->push_back ({ 700, [&host, &ctx]
    {
        ctx.expect (host.clickStripControl (GuiHost::StripKind::Channel, renderTrack, "print", 1, false),
                    "the rendered track has no FREEZE button to click");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressKey ("command + Z"), "Undo was not handled over the freeze dialog"); } });
    alerted ("undo over the freeze dialog", "Can't move tracks\nClose the freeze dialog, then undo the move again.");
    steps->push_back ({ 100, [&host, order, moved]
    {
        order (moved ({ kick }, 0), "the undo ran over the freeze dialog");
        if (host.renderRunning()) host.clickModalButton ("Cancel");
    } });
    Step closed { 300, [&host, &ctx] { ctx.expect (host.clickModalButton ("Close"), "the freeze dialog did not offer Close"); } };
    closed.until = [&host] { return ! host.renderRunning(); };
    closed.timeout = "the freeze did not stop";
    steps->push_back (std::move (closed));
    steps->push_back ({ 300, [&host, &ctx, &session, &engine]
    {
        ctx.expect (host.modalStackEmpty(), "the freeze left '" + host.modalText() + "' up");
        if (session.track (renderTrack).frozen.load()) engine.unfreezeTrack (renderTrack);
        ctx.expect (host.pressKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 300, [order, identity] { order (identity(), "Undo after the freeze did not put the tracks back"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineTrackMove { Scenario {
    "gui.timeline_track_move", { "gui", "region" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineTrackMove (host, ctx); }
} };

// Clone to track says why it refuses, while the transport runs or with a frozen
// track on either side, and clones once both are clear.
std::optional<ScenarioResult> runCloneRefusals (GuiHost& host, ScenarioContext& ctx)
{
    ctx.cleanup (host.preserveKeyboardFocus());
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& dest = session.track (1);
    ctx.keep (dest.frozen);
    ctx.cleanup ([&engine, &transport]
    {
        engine.stop();
        transport.setPlayhead (0);
        engine.getUndoManager().clearUndoHistory();
    });
    const auto destName = dest.name.toStdString();
    auto steps = std::make_shared<std::vector<Step>>();
    const auto clone = [&host, &ctx, steps, destName] (const std::string& how)
    {
        steps->push_back ({ 300, [&host, &ctx, how]
        {
            ctx.expect (host.modalStackEmpty(), how + ": a modal was still open");
            ctx.expect (host.clickStripControl (GuiHost::StripKind::Channel, 0, "name", 1, true),
                        how + ": the strip name did not take a right-click");
        } });
        steps->push_back ({ 200, [&host, &ctx, how]
        {
            ctx.expect (host.clickContextMenuItem ("Clone to track..."),
                        how + ": the strip menu has no Clone to track...");
        } });
        steps->push_back ({ 200, [&host, &ctx, how, destName]
        {
            ctx.expect (host.clickContextMenuItem ("2: " + destName),
                        how + ": the Clone to track... submenu has no track 2");
        } });
    };
    const auto alerted = [&host, &ctx, &dest, steps, destName] (const std::string& how,
                                                                const std::string& expected)
    {
        steps->push_back ({ 300, [&host, &ctx, &dest, how, expected, destName]
        {
            ctx.expect (host.modalText() == expected, how + ": the alert read '" + host.modalText() + "'");
            ctx.expect (host.clickModalButton ("OK"), how + ": the alert has no OK button");
            ctx.expect (dest.name.toStdString() == destName, how + ": the clone ran anyway");
        } });
    };

    steps->push_back ({ 100, [&transport] { transport.setState (Transport::State::Playing); } });
    clone ("while playing");
    alerted ("while playing", "Can't clone track\nStop playback, then clone the track again.");
    steps->push_back ({ 100, [&engine, &transport, &dest]
    {
        engine.stop();
        transport.setPlayhead (0);
        dest.frozen.store (true);
    } });
    clone ("onto a frozen track");
    alerted ("onto a frozen track", "Can't clone track\nUnfreeze the track, then clone it again. "
                                    "A frozen track can't be cloned or cloned onto.");
    steps->push_back ({ 100, [&dest] { dest.frozen.store (false); } });
    clone ("with both clear");
    steps->push_back ({ 300, [&host, &ctx, &engine, &session, &dest, destName]
    {
        ctx.expect (host.modalStackEmpty(), "a clone with both clear still raised an alert");
        ctx.expect (dest.name == session.track (0).name + " (copy)", "a clone with both clear did not run");
        ctx.expect (engine.getUndoManager().undo() && dest.name.toStdString() == destName,
                    "undo did not put the destination back");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar cloneRefusals { Scenario {
    "gui.clone_track_refusals", { "gui", "clone" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCloneRefusals (host, ctx); }
} };

// A track move's undo and redo wait while a menu or a dialog is open over the
// window: the track name menu and the import target picker hold a track by its
// row, and the move would put another track under them. An EQ or plug-in editor
// does not hold them up; the move closes it with its strip.
std::optional<ScenarioResult> runTrackMoveUndoUnderMenu (GuiHost& host, ScenarioContext& ctx)
{
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    for (const int t : { 1, 2 })
    {
        if (session.track (t).frozen.load() || ! session.track (t).midiRegions.current().empty())
            return ScenarioResult::skip ("requires tracks 2 and 3 unfrozen with no MIDI regions");
        session.track (t).regions = { region };
    }
    std::array<const ChannelStrip*, Session::kNumTracks> launched {};
    for (int t = 0; t < Session::kNumTracks; ++t) launched[(std::size_t) t] = &engine.getChannelStrip (t);
    ctx.cleanup ([&host, &engine, launched]
    {
        drainModals (host);
        for (int t = 0; t < 3; ++t)
            if (auto* strip = host.strip (t))
            {
                strip->closeEditor();
                strip->unloadNativePlugins();
                strip->refreshInsertButton();
            }
        std::array<int, Session::kNumTracks> newToOld {};
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            int now = 0;
            while (now < Session::kNumTracks - 1 && &engine.getChannelStrip (now) != launched[(std::size_t) t]) ++now;
            newToOld[(std::size_t) t] = now;
        }
        if (const auto back = trackMoveFromNewToOld (newToOld)) engine.moveTracks (*back);
        engine.getUndoManager().clearUndoHistory();
    });
    const auto movedUp = [&engine, launched]
    {
        return &engine.getChannelStrip (0) == launched[2] && &engine.getChannelStrip (2) == launched[1];
    };
    const auto inPlace = [&engine, launched]
    {
        for (int t = 0; t < 3; ++t)
            if (&engine.getChannelStrip (t) != launched[(std::size_t) t]) return false;
        return true;
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.dragTapeTrackName (2, 0), "track 3's name did not take a drag"); } });
    steps->push_back ({ 300, [&host, &ctx, &engine, movedUp]
    {
        ctx.expect (movedUp() && engine.getUndoManager().getUndoDescription() == kMoveTracksTransaction,
                    "dragging track 3 to the top did not move it there as one undo step");
        ctx.expect (host.clickTapeTrackName (0, 1, 4), "the moved track's name did not take a right-click");
    } });
    for (const bool redo : { false, true })
    {
        const std::string step = redo ? "redo" : "undo";
        steps->push_back ({ 300, [&host, &ctx, step, redo]
        {
            ctx.expect (! host.contextMenuItems().empty(), "the track name menu did not open for the " + step);
            ctx.expect (host.pressPeerKey (redo ? "command + shift + Z" : "command + Z"),
                        "the window did not take the " + step + " key with the menu open");
        } });
        steps->push_back ({ 300, [&host, &ctx, &engine, step, redo, movedUp, inPlace]
        {
            ctx.expect (host.modalText() == "Can't move tracks\nClose the open menu or dialog, then " + step + " the move again.",
                        "the " + step + " under the menu read '" + host.modalText() + "'");
            ctx.expect (redo ? inPlace() : movedUp(), "the " + step + " ran under the open menu");
            auto& um = engine.getUndoManager();
            ctx.expect ((redo ? um.getRedoDescription() : um.getUndoDescription()) == kMoveTracksTransaction,
                        "a refused " + step + " took the move out of the history");
            ctx.expect (host.clickModalButton ("OK"), "the alert has no OK button");
        } });
        steps->push_back ({ 300, [&host, &ctx]
        {
            ctx.expect (! host.contextMenuItems().empty(), "the alert took the track name menu down");
            ctx.expect (host.pressPeerKey ("escape"), "Escape was not handled with the menu open");
        } });
        steps->push_back ({ 300, [&host, &ctx, step, redo]
        {
            ctx.expect (host.modalStackEmpty(), "Escape left '" + host.modalText() + "' up");
            ctx.expect (host.pressPeerKey (redo ? "command + shift + Z" : "command + Z"), "the window did not take the " + step + " key");
        } });
        steps->push_back ({ 300, [&host, &ctx, step, redo, movedUp, inPlace]
        {
            ctx.expect (redo ? movedUp() : inPlace(), "the " + step + " with the menu closed did not run");
            if (! redo) ctx.expect (host.clickTapeTrackName (2, 1, 4), "track 3's name did not take a right-click");
        } });
    }

    // An editor holds its strip, not a row, and the move closes it with the strip.
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickStripModule (0, 0, true, false), "the moved track's EQ label is not on screen"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.stripModuleEditorOpen (0, 0), "the EQ label did not open its editor");
        ctx.expect (host.pressPeerKey ("command + Z"), "the window did not take the undo key over the EQ editor");
    } });
    steps->push_back ({ 300, [&host, &ctx, inPlace]
    {
        ctx.expect (inPlace(), "the undo over the EQ editor did not run ('" + host.modalText() + "')");
        ctx.expect (host.modalStackEmpty(), "the undo left '" + host.modalText() + "' up over the moved strip");
        ctx.expect (host.clickStripModule (2, 0, true, false), "track 3's EQ label is not on screen after the undo");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.stripModuleEditorOpen (2, 0), "the EQ label did not open its editor for the redo");
        ctx.expect (host.pressPeerKey ("command + shift + Z"), "the window did not take the redo key over the EQ editor");
    } });
    // Linux builds the CLAP fixture beside the suite; elsewhere this leg is noted.
    const auto pluginEditor = std::make_shared<bool> (false);
    steps->push_back ({ 300, [&host, &ctx, movedUp, pluginEditor]
    {
        ctx.expect (movedUp(), "the redo over the EQ editor did not run ('" + host.modalText() + "')");
        ctx.expect (host.modalStackEmpty(), "the redo left '" + host.modalText() + "' up over the moved strip");
        const auto fixture = ctx.fixture ("no_window.clap");
        auto* strip = host.strip (0);
        std::string error;
        const bool loaded = fixture && strip != nullptr
                         && strip->loadNativeClap (*fixture, "studio.dusk.test.no-window", error);
       #if DUSKSTUDIO_HAS_NATIVE_CLAP && defined (__linux__)
        if (! ctx.expect (loaded, "the CLAP fixture did not load for the plug-in editor leg " + error)) return;
       #else
        if (! loaded)
        {
            ctx.note ("no CLAP fixture to open a plug-in editor with, so that leg was skipped " + error);
            return;
        }
       #endif
        strip->refreshInsertButton();
        if (! strip->openEditor())
        {
           #if DUSKSTUDIO_HAS_NATIVE_CLAP && defined (__linux__)
            if (host.canEmbedPluginEditors())
                ctx.expect (false, "the CLAP fixture's editor could not be embedded for the plug-in editor leg");
            else
           #endif
            ctx.note ("the CLAP fixture's editor could not be embedded on this display, so that leg was skipped");
            dismissAlert (host);
            return;
        }
        *pluginEditor = true;
        ctx.expect (host.pressPeerKey ("command + Z"), "the window did not take the undo key over the plug-in editor");
    } });
    steps->push_back ({ 300, [&host, &ctx, inPlace, pluginEditor]
    {
        if (! *pluginEditor) return;
        ctx.expect (inPlace(), "the undo over the plug-in editor did not run ('" + host.modalText() + "')");
        ctx.expect (host.modalStackEmpty(), "the undo left '" + host.modalText() + "' up over the moved strip");
        for (int t = 0; t < 3; ++t)
            if (auto* strip = host.strip (t))
                ctx.expect (! strip->hasOpenEditor(), "the plug-in editor stayed open on row " + std::to_string (t + 1));
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar trackMoveUndoUnderMenu { Scenario {
    "gui.track_move_undo_under_menu", { "gui", "region", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTrackMoveUndoUnderMenu (host, ctx); }
} };

// Undo and Redo typed into the compressor editor or Utility's knob panel of a
// moved strip run once the panel's frame is out, so the move they take back
// closes the panel with its strip instead of destroying it mid-frame.
std::optional<ScenarioResult> runTrackMoveUndoFromPanel (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    for (const int t : { 1, 2 })
    {
        if (session.track (t).frozen.load() || ! session.track (t).midiRegions.current().empty())
            return ScenarioResult::skip ("requires tracks 2 and 3 unfrozen with no MIDI regions");
        session.track (t).regions = { region };
    }
    if (engine.getChannelStrip (2).isBuiltinLoaded() || engine.getChannelStrip (2).getPluginSlot().isLoaded())
        return ScenarioResult::skip ("requires an empty insert on track 3");
    std::array<ChannelStrip*, Session::kNumTracks> launched {};
    for (int t = 0; t < Session::kNumTracks; ++t) launched[(std::size_t) t] = &engine.getChannelStrip (t);
    ctx.cleanup ([&host, &engine, launched, mode = launched[2]->insertMode.load()]
    {
        for (int t = 0; t < 3; ++t)
        {
            host.closeBuiltin (t);
            host.closeStripModuleEditors (t);
        }
        drainModals (host);
        engine.suspendProcessing();
        launched[2]->unloadBuiltin();
        launched[2]->insertMode.store (mode);
        engine.resumeProcessing();
        std::array<int, Session::kNumTracks> newToOld {};
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            int now = 0;
            while (now < Session::kNumTracks - 1 && &engine.getChannelStrip (now) != launched[(std::size_t) t]) ++now;
            newToOld[(std::size_t) t] = now;
        }
        if (const auto back = trackMoveFromNewToOld (newToOld)) engine.moveTracks (*back);
        for (int t = 0; t < 3; ++t)
            if (auto* strip = host.strip (t)) strip->refreshInsertButton();
        engine.getUndoManager().clearUndoHistory();
    });
    const auto movedUp = [&engine, launched]
    {
        return &engine.getChannelStrip (0) == launched[2] && &engine.getChannelStrip (2) == launched[1];
    };
    const auto inPlace = [&engine, launched]
    {
        for (int t = 0; t < 3; ++t)
            if (&engine.getChannelStrip (t) != launched[(std::size_t) t]) return false;
        return true;
    };
    const auto panelsClosed = [&host]
    {
        for (int t = 0; t < 3; ++t)
            if (host.stripModuleEditorOpen (t, 1)) return false;
        for (int t = 0; t < 3; ++t)
            if (auto* strip = host.strip (t); strip != nullptr && strip->hasOpenBuiltinEditor()) return false;
        return true;
    };

    // A probe edit on top of the move: its undo reports whether a panel was
    // drawing when the panel's Cmd+Z reached the history.
    auto probeUndone = std::make_shared<int> (0);
    auto undoneInFrame = std::make_shared<bool> (false);
    const auto pushProbe = [&host, &engine, probeUndone, undoneInFrame]
    {
        auto& um = engine.getUndoManager();
        um.beginNewTransaction ("Panel frame probe");
        um.perform (new ParamEditAction ([] {},
            [&host, probeUndone, undoneInFrame]
            {
                ++*probeUndone;
                *undoneInFrame = host.panelFrameDrawing();
            }));
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.dragTapeTrackName (2, 0), "track 3's name did not take a drag"); } });
    steps->push_back ({ 300, [&host, &ctx, &engine, movedUp]
    {
        ctx.expect (movedUp() && engine.getUndoManager().getUndoDescription() == kMoveTracksTransaction,
                    "dragging track 3 to the top did not move it there as one undo step");
        ctx.expect (host.clickStripModule (0, 1, true, false), "the moved track's compressor label is not on screen");
    } });
    steps->push_back ({ 400, [&host, &ctx, pushProbe]
    {
        if (! ctx.expect (host.stripModuleEditorOpen (0, 1), "the compressor label did not open its editor")) return;
        pushProbe();
        ctx.expect (host.inputStripPanel (0, "command + Z"), "the compressor editor did not take Cmd+Z");
    } });
    auto inFrame = std::make_shared<bool> (true);
    steps->push_back ({ 300, [&host, &ctx, probeUndone, undoneInFrame, inFrame]
    {
        if (! ctx.expect (*probeUndone == 1, "Cmd+Z in the compressor editor did not reach the history")) return;
        *inFrame = *undoneInFrame;
        if (! ctx.expect (! *inFrame, "Cmd+Z in the compressor editor ran inside the panel's frame, "
                                      "where undoing the move would destroy the panel; that leg was not driven"))
            return;
        ctx.expect (host.inputStripPanel (0, "command + Z"), "the compressor editor did not take Cmd+Z for the move");
    } });
    steps->push_back ({ 400, [&host, &ctx, &engine, inPlace, panelsClosed, inFrame]
    {
        if (*inFrame) return;
        ctx.expect (inPlace(), "Cmd+Z in the moved strip's compressor editor did not undo the move ('" + host.modalText() + "')");
        ctx.expect (host.modalStackEmpty(), "the undo from the compressor editor left '" + host.modalText() + "' up");
        ctx.expect (panelsClosed(), "the undo left a compressor editor open");
        std::string error;
        auto& dsp = engine.getChannelStrip (2);
        engine.suspendProcessing();
        const bool loaded = dsp.loadBuiltin ("dusk.builtin.utility", error);
        if (loaded) dsp.insertMode.store (ChannelStrip::kInsertPlugin);
        engine.resumeProcessing();
        if (! ctx.expect (loaded, "could not load Utility: " + error)) return;
        if (auto* strip = host.strip (2)) strip->refreshInsertButton();
        ctx.expect (host.clickInsert (2, false), "track 3's insert is not on screen");
    } });
    steps->push_back ({ 400, [&host, &ctx, inFrame]
    {
        if (*inFrame) return;
        auto* strip = host.strip (2);
        if (! ctx.expect (strip != nullptr && strip->hasOpenBuiltinEditor(), "Utility's knob panel did not open")) return;
        ctx.expect (host.inputStripPanel (2, "command + shift + Z"), "Utility's knob panel did not take Cmd+Shift+Z");
    } });
    steps->push_back ({ 400, [&host, &ctx, movedUp, panelsClosed, inFrame]
    {
        if (*inFrame) return;
        ctx.expect (movedUp(), "Cmd+Shift+Z in the moved strip's knob panel did not redo the move ('" + host.modalText() + "')");
        ctx.expect (host.modalStackEmpty(), "the redo from Utility's knob panel left '" + host.modalText() + "' up");
        ctx.expect (panelsClosed(), "the redo left Utility's knob panel open");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar trackMoveUndoFromPanel { Scenario {
    "gui.track_move_undo_from_panel", { "gui", "region", "undo", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTrackMoveUndoFromPanel (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
