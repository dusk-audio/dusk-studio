#include "../GuiScenarioSupport.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../session/SessionSerializer.h"
#include "../../../session/TakeComp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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
// On Windows the editor's child never holds the keyboard, so what is typed into one of
// its fields reaches the shell's window. While a field or a menu is open the editor
// takes every such key and replays it into the child: Backspace and letters edit a
// take's name instead of deleting the tape strip's selected region or toggling loop,
// Space types a space instead of playing, Enter renames and Escape cancels. A child that
// holds the keyboard already got the key, so the shell's copy is dropped, not typed twice.
std::optional<ScenarioResult> runAudioEditorFieldKeysFromShell (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto& transport = engine.getTransport();
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto regionsBefore = track.regions;
    const auto newest = ids->back();
    const bool loopWas = transport.isLoopEnabled();
    const bool expanded = host.timelineViewMatches (true);
    ctx.cleanup ([&host, &engine, &transport, loopWas, expanded]
    {
        engine.stop();
        transport.setLoopEnabled (loopWas);
        host.closeAudioEditor();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    ctx.cleanup (host.preserveKeyboardFocus());
    if (! expanded) host.pressKey ("T", 't');

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
    const auto type = [&host] (char glyph) { host.pressPeerKey (keyCodeDescription (glyph), glyph); };
    const auto untouched = [&ctx, &track, &transport, regionsBefore, loopWas] (const std::string& where)
    {
        ctx.expect (sameRegions (track.regions, regionsBefore), "a key " + where + " changed the track's regions");
        ctx.expect (transport.isStopped(), "a key " + where + " started the transport");
        ctx.expect (transport.isLoopEnabled() == loopWas, "a key " + where + " toggled loop");
    };

    const auto nameShown = [&host, newest] { return host.audioEditorTakePoint ("name", newest, 0).size() == 2; };

    auto steps = std::make_shared<std::vector<Step>>();
    const auto openName = [&steps, click, renaming, nameShown] (const char* timeout)
    {
        steps->push_back ({ 100, click, nameShown, "the take's header never came into view" });
        steps->push_back ({ 100, [] {}, renaming, timeout });
    };
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.doubleClickAudioRegion (0, 0), "the region is not visible for a double-click"); } });
    steps->push_back ({ 100, [] {}, [&host] { return host.audioEditorTakeLanes().size() == 2; },
                        "the double-click did not open the editor on the take lanes" });
    openName ("a double-click on the take's name did not open it for typing");
    steps->push_back ({ 100, [&host, &ctx, type]
    {
        ctx.expect (host.audioEditorKeyboardFocus (true), "the editor's child could not take the keyboard");
        type ('l');
        host.pressPeerKey ("backspace", 0);
    } });
    steps->push_back ({ 100, [&host, &ctx, type, untouched]
    {
        untouched ("the shell got while the child held the keyboard");
        ctx.expect (host.audioEditorKeyboardFocus (false), "the editor's child could not give up the keyboard");
        type ('x');
        type ('l');
        host.pressPeerKey ("spacebar", ' ');
        type ('q');
    } });
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("backspace", 0); } });
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("return", 0); } });
    steps->push_back ({ 100, [&ctx, &engine, nameOf, untouched]
    {
        ctx.expect (nameOf() == "xl", "the keys the shell got renamed the take '" + nameOf() + "'");
        ctx.expect (undoDescription (engine) == "Rename take", "the rename is not the \"Rename take\" step");
        untouched ("typed into the take's name");
    }, fieldClosed, "Enter from the shell left the name field open" });
    openName ("the name did not reopen for typing");
    steps->push_back ({ 100, [&host, &ctx, type]
    {
        ctx.expect (host.audioEditorKeyboardFocus (false), "the editor's child could not give up the keyboard");
        type ('z');
        host.pressPeerKey ("escape", 0);
    } });
    steps->push_back ({ 100, [&host, &ctx, nameOf, untouched]
    {
        ctx.expect (nameOf() == "xl", "Escape from the shell renamed the take");
        ctx.expect (host.audioEditorOpen(), "Escape from the shell closed the editor under the field");
        untouched ("that cancelled the name");
        const auto at = host.audioEditorPoint ("wave", kTakeCaseLength / 2);
        if (ctx.expect (at.size() == 2, "editor geometry unavailable"))
        {
            constexpr int rightButton = 4;
            host.audioEditorPointer (at[0], at[1], true, rightButton);
            host.audioEditorPointer (at[0], at[1], false, rightButton);
        }
    }, fieldClosed, "Escape from the shell left the name field open" });
    steps->push_back ({ 150, [&host, &ctx, type]
    {
        ctx.expect (host.audioEditorKeyboardFocus (false), "the editor's child could not give up the keyboard");
        type ('l');
        host.pressPeerKey ("escape", 0);
    } });
    steps->push_back ({ 150, [&host, &ctx, untouched]
    {
        untouched ("with the editor's menu open");
        ctx.expect (host.audioEditorOpen(), "Escape from the shell closed the editor under its menu");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorFieldKeysFromShell { Scenario {
    "gui.audio_editor_field_keys_from_shell", { "gui", "keyboard", "editor", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorFieldKeysFromShell (host, ctx); }
} };

// Escape during a drag cancels the drag, with or without the modifier the drag is
// held with, whether the key reaches the editor's child or the shell's window: the region, or the span dragged across a take, goes back to
// where the press found it, nothing is recorded, and the editor stays up. Closing the
// editor in the middle of a drag puts the region back the same way.
std::optional<ScenarioResult> runAudioEditorEscapeCancelsDrag (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto ids = addLevelTakes (ctx, track, 2);
    if (! ids) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    const auto before = track.regions;
    const auto other = ids->back();
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto point = [&host, &ctx] (const std::string& kind, std::int64_t sample)
    {
        const auto at = host.audioEditorPoint (kind, sample);
        ctx.expect (at.size() == 2, kind + " geometry unavailable");
        return at.size() == 2 ? at : std::vector<int> { 0, 0 };
    };
    auto held = std::make_shared<std::vector<int>>();
    auto heldModifiers = std::make_shared<int> (0);
    const auto press = [&host, &ctx, held, heldModifiers] (std::vector<int> from, std::vector<int> to, int modifiers = 0)
    {
        ctx.expect (host.audioEditorPointer (from[0], from[1], true, modifiers), "the drag's press failed");
        host.audioEditorPointer (to[0], to[1], true, modifiers);
        *held = to;
        *heldModifiers = modifiers;
    };
    const auto release = [&host, held, heldModifiers]
    { host.audioEditorPointer ((*held)[0], (*held)[1], false, *heldModifiers); };
    const auto restored = [&ctx, &engine, &track, before] (const std::string& what)
    {
        ctx.expect (sameRegions (track.regions, before) && ! track.regions.empty()
                        && std::abs (track.regions[0].gainDb) < 1.0e-4f,
                    what + " left the region where the drag had taken it");
        ctx.expect (! engine.getUndoManager().canUndo(), what + " recorded an undo step");
    };
    const auto region = [&track] { return track.regions.empty() ? AudioRegion() : track.regions[0]; };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [press, point] { press (point ("end", kTakeCaseLength), point ("wave", 72000)); },
                        [&host] { return host.audioEditorPoint ("end", kTakeCaseLength).size() == 2; },
                        "the editor never laid out" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("escape"), "Escape was not delivered to the editor"); },
    [region] { return region().lengthInSamples < kTakeCaseLength - 12000; }, "the trim did not follow the pointer" });
    steps->push_back ({ 150, [&host, &ctx, restored, release]
    {
        ctx.expect (host.audioEditorOpen(), "Escape during a trim closed the editor");
        restored ("Escape during a trim");
        release();
    } });
    steps->push_back ({ 150, [restored, press, point]
    {
        restored ("the release after a cancelled trim");
        const auto from = point ("gain", 48000);
        press (from, { from[0], from[1] - 60 });
    } });
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("escape", 0); },
                        [region] { return region().gainDb > 3.0f; }, "the gain did not follow the pointer" });
    steps->push_back ({ 150, [&host, &ctx, restored, release]
    {
        ctx.expect (host.audioEditorOpen(), "Escape from the shell during a gain drag closed the editor");
        restored ("Escape from the shell during a gain drag");
        release();
    } });
    static constexpr int command = 2;
    const auto trimmed = [region] { return region().lengthInSamples < kTakeCaseLength - 12000; };
    steps->push_back ({ 150, [restored, press, point]
    {
        restored ("the release after a cancelled gain drag");
        press (point ("end", kTakeCaseLength), point ("wave", 72000), command);
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + escape"), "Cmd+Escape was not delivered to the editor"); },
    trimmed, "the Cmd-held trim did not follow the pointer" });
    steps->push_back ({ 150, [&host, &ctx, restored, release]
    {
        ctx.expect (host.audioEditorOpen(), "Cmd+Escape during a Cmd-held trim closed the editor");
        restored ("Cmd+Escape during a Cmd-held trim");
        release();
    } });
    steps->push_back ({ 150, [restored, press, point]
    {
        restored ("the release after a trim cancelled by Cmd+Escape");
        press (point ("end", kTakeCaseLength), point ("wave", 72000), command);
    } });
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("command + escape", 0); },
                        trimmed, "the second Cmd-held trim did not follow the pointer" });
    steps->push_back ({ 150, [&host, &ctx, restored, release]
    {
        ctx.expect (host.audioEditorOpen(), "Cmd+Escape from the shell during a Cmd-held trim closed the editor");
        restored ("Cmd+Escape from the shell during a Cmd-held trim");
        release();
    } });
    steps->push_back ({ 150, [&host, press, other]
    {
        const auto from = host.audioEditorTakePoint ("lane", other, 24000);
        const auto to = host.audioEditorTakePoint ("lane", other, 60000);
        if (from.size() == 2 && to.size() == 2) press (from, to);
    }, [&host, other] { return host.audioEditorTakePoint ("lane", other, 24000).size() == 2; },
       "the other take's lane is not on screen" });
    steps->push_back ({ 100, [&host] { host.pressAudioEditorKey ("escape"); },
                        [&host, other] { const auto s = host.audioEditorTakeState(); return s.size() == 5 && s[2] == (std::int64_t) other; },
                        "the drag across the take is not held" });
    steps->push_back ({ 150, [&host, &ctx, release]
    {
        const auto state = host.audioEditorTakeState();
        ctx.expect (state.size() == 5 && state[2] == 0, "Escape did not drop the drag across the take");
        ctx.expect (host.audioEditorOpen(), "Escape during a drag across a take closed the editor");
        release();
    } });
    steps->push_back ({ 300, [&ctx, &track, restored, press, point, other]
    {
        ctx.expect (takeCoverage (track, other).empty(), "the release after a cancelled drag promoted the take");
        restored ("the release after a cancelled drag across a take");
        press (point ("wave", 48000), point ("wave", 72000));
    } });
    steps->push_back ({ 100, [&host] { host.raiseAlert ("Audio device lost", "The audio device went away."); },
                        [region] { return region().timelineStart > 12000; }, "the region did not follow the pointer" });
    steps->push_back ({ 100, [&host, restored]
    {
        restored ("the editor closing for an alert during a move");
        host.closeTopModal();
        host.openAudioEditor (0, 0);
    }, [&host] { return ! host.audioEditorOpen(); }, "the alert did not close the editor" });
    steps->push_back ({ 150, [press, point] { press (point ("wave", 48000), point ("wave", 72000)); },
                        [&host] { return host.audioEditorPoint ("wave", 48000).size() == 2; },
                        "the editor did not open again" });
    steps->push_back ({ 100, [&host] { host.closeAudioEditor(); },
                        [region] { return region().timelineStart > 12000; }, "the second move did not follow the pointer" });
    steps->push_back ({ 150, [restored] { restored ("closing the editor during a move"); },
                        [&host] { return ! host.audioEditorOpen(); }, "the editor did not close" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorEscapeCancelsDrag { Scenario {
    "gui.audio_editor_escape_cancels_drag", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorEscapeCancelsDrag (host, ctx); }
} };

// A menu that opens in the editor asks the system for the keyboard even when the
// child already counts itself focused: on X11 a focus event the pointer caused
// leaves it so while the keys still go to the shell's window.
std::optional<ScenarioResult> runAudioEditorMenuAsksForKeyboard (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& session = ctx.session();
    auto& track = session.track (0);
    if (! addLevelTakes (ctx, track, 1)) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    auto asked = std::make_shared<int> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx, asked]
    {
        ctx.expect (host.audioEditorKeyboardFocus (true), "the editor's child could not take the keyboard");
        *asked = host.audioEditorKeyboardRequests();
        const auto at = host.audioEditorPoint ("wave", kTakeCaseLength / 2);
        constexpr int rightButton = 4;
        host.audioEditorPointer (at[0], at[1], true, rightButton);
        host.audioEditorPointer (at[0], at[1], false, rightButton);
    }, [&host] { return host.audioEditorPoint ("wave", kTakeCaseLength / 2).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 100, [&host] { host.pressAudioEditorKey ("escape"); },
                        [&host, asked] { return host.audioEditorKeyboardRequests() > *asked; },
                        "the editor's menu opened without asking for the keyboard" });
    steps->push_back ({ 150, [&ctx, &host] { ctx.expect (host.audioEditorOpen(), "Escape closed the editor under its menu"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorMenuAsksForKeyboard { Scenario {
    "gui.audio_editor_menu_asks_for_keyboard", { "gui", "keyboard", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorMenuAsksForKeyboard (host, ctx); }
} };

// While a drag is held every key but Escape waits, whether it reaches the editor's
// child or the shell's window: Delete, Undo or Split pressed mid-drag leaves the
// regions and the undo history as the drag alone leaves them.
std::optional<ScenarioResult> runAudioEditorKeysWaitForDrag (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    if (! addLevelTakes (ctx, track, 2)) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[1], kTakeCaseLength / 2, kTakeCaseLength);
    if (track.regions.size() != 2) return ScenarioResult::fail ("the setup did not leave two regions");
    const auto before = track.regions;
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    auto held = std::make_shared<std::vector<int>>();
    auto seeded = std::make_shared<float> (0.0f);
    const auto gain = [&track] { return track.regions.empty() ? 0.0f : track.regions[0].gainDb; };
    const auto pressGain = [&host, &ctx, held]
    {
        const auto from = host.audioEditorPoint ("gain", 12000);
        if (! ctx.expect (from.size() == 2, "gain geometry unavailable")) return;
        ctx.expect (host.audioEditorPointer (from[0], from[1], true), "the drag's press failed");
        *held = { from[0], from[1] - 30 };
        host.audioEditorPointer ((*held)[0], (*held)[1], true);
    };
    const auto release = [&host, held]
    {
        if (held->size() == 2) host.audioEditorPointer ((*held)[0], (*held)[1], false);
    };

    struct Press { std::string label; std::function<bool()> send; };
    const std::vector<Press> presses {
        { "Delete at the editor",  [&host] { return host.pressAudioEditorKey ("delete"); } },
        { "Undo at the editor",    [&host] { return host.pressAudioEditorKey ("command + Z"); } },
        { "Split at the editor",   [&host] { return host.pressAudioEditorKey ("command + E"); } },
        { "Delete at the shell",   [&host] { return host.pressPeerKey ("delete", 0); } },
        { "Undo at the shell",     [&host] { return host.pressPeerKey ("command + Z", 0); } },
        { "Split at the shell",    [&host] { return host.pressPeerKey ("command + E", 0); } },
    };

    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& press : presses)
    {
        const auto label = press.label;
        steps->push_back ({ 150, [&engine, &track, before]
        {
            track.regions = before;
            engine.getUndoManager().clearUndoHistory();
        } });
        steps->push_back ({ 150, pressGain, [&host] { return host.audioEditorPoint ("gain", 12000).size() == 2; },
                            "the editor never laid out" });
        steps->push_back ({ 100, release, [gain] { return gain() > 1.0f; }, "the first gain drag did not follow the pointer" });
        steps->push_back ({ 150, [seeded, gain, pressGain] { *seeded = gain(); pressGain(); } });
        steps->push_back ({ 100, [&ctx, press, label] { ctx.expect (press.send(), label + " was not delivered"); },
                            [seeded, gain] { return gain() > *seeded + 1.0f; }, "the second gain drag did not follow the pointer" });
        steps->push_back ({ 150, [&ctx, &track, release, label]
        {
            ctx.expect (track.regions.size() == 2, label + " during a drag changed the regions");
            release();
        } });
        steps->push_back ({ 200, [&ctx, &engine, &host, &track, before, seeded, gain, label]
        {
            auto& undo = engine.getUndoManager();
            const auto dragged = gain();
            ctx.expect (sameRegions (track.regions, before) && dragged > *seeded + 1.0f,
                        label + " during a drag left the regions other than the drag did");
            ctx.expect (host.audioEditorRegion() == 0, label + " during a drag moved the editor off the region");
            ctx.expect (undoDescription (engine) == "Region gain", label + " during a drag recorded another step");
            undo.undo();
            ctx.expect (sameRegions (track.regions, before) && std::abs (gain() - *seeded) < 1.0e-4f,
                        label + " during a drag: Undo did not take back just the drag");
            undo.undo();
            ctx.expect (sameRegions (track.regions, before) && std::abs (gain()) < 1.0e-4f && ! undo.canUndo(),
                        label + " during a drag: the history held more than the two drags");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorKeysWaitForDrag { Scenario {
    "gui.audio_editor_keys_wait_for_drag", { "gui", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorKeysWaitForDrag (host, ctx); }
} };

// Cmd+C and Cmd+X in the editor take the selected range when one is drawn and the
// focused region otherwise, each cut one undo step; a cut region closes the editor.
// Cmd+V lands the copy on the track the editor shows, at its edit cursor, wherever
// the copy came from: one "Paste region" step, the source track untouched.
std::optional<ScenarioResult> runAudioEditorClipboardKeys (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    ctx.cleanup ([&engine, clipboard = engine.getRegionClipboard()] { engine.getRegionClipboard() = clipboard; });
    engine.getRegionClipboard() = {};
    const auto path = ctx.tempDir() / "clipboard-source.wav";
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 24 });
    if (writer == nullptr) return ScenarioResult::fail ("could not create the clipboard fixture");
    std::vector<float> silence (192000, 0.0f);
    const float* channels[] { silence.data() };
    if (! writer->write (channels, 1, 192000) || ! writer->flush())
        return ScenarioResult::fail ("could not write the clipboard fixture");
    writer.reset();
    AudioRegion copied;
    copied.file = decltype (copied.file) (path.u8string().c_str());
    copied.lengthInSamples = 48000;
    auto shown = copied;
    shown.timelineStart = 96000;
    shown.lengthInSamples = 96000;
    auto& source = session.track (0);
    auto& target = session.track (1);
    target.frozen.store (false);
    target.takes.clear();
    source.regions = { copied };
    target.regions = { shown };
    session.editMode = EditMode::Grab;
    session.audioEditorSnap = false;
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto laidOut = [&host] { return host.audioEditorPoint ("wave", 1000).size() == 2; };
    const auto closeTo = [] (std::int64_t a, std::int64_t b) { return std::abs (a - b) < 128; };
    const auto chunkOnClipboard = [&ctx, &engine, closeTo] (const std::string& what)
    {
        const auto& clip = engine.getRegionClipboard();
        ctx.expect (clip.hasContent && clip.sourceTrack == 0 && closeTo (clip.region.sourceOffset, 12000)
                    && closeTo (clip.region.lengthInSamples, 12000), what + " did not put the range on the clipboard");
    };
    const auto cursor = std::make_shared<std::int64_t> (-1);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    {
        const auto from = host.audioEditorPoint ("wave", 12000);
        const auto to = host.audioEditorPoint ("wave", 24000);
        if (! ctx.expect (from.size() == 2 && to.size() == 2, "editor geometry unavailable")) return;
        static constexpr int shift = 1;
        host.audioEditorPointer (from[0], from[1], true, shift);
        host.audioEditorPointer (to[0], to[1], true, shift);
        host.audioEditorPointer (to[0], to[1], false, shift);
    }, laidOut, "the editor on the first track never laid out" });
    steps->push_back ({ 150, [&host, &ctx]
    {
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[1] == 1, "the Shift drag drew no range");
        ctx.expect (host.pressAudioEditorKey ("command + C"), "Copy was not handled");
    } });
    steps->push_back ({ 100, [&host, &ctx, &engine, &source, copied, chunkOnClipboard]
    {
        chunkOnClipboard ("Copy with a range drawn");
        ctx.expect (sameRegions (source.regions, { copied }), "Copy changed the track");
        engine.getRegionClipboard() = {};
        ctx.expect (host.pressAudioEditorKey ("command + X"), "Cut was not handled");
    } });
    steps->push_back ({ 100, [&host, &ctx, &engine, &source, chunkOnClipboard]
    {
        chunkOnClipboard ("Cut with a range drawn");
        ctx.expect (source.regions.size() == 2, "Cut with a range drawn did not take the range out of the region");
        ctx.expect (undoDescription (engine) == "Cut chunk", "the range cut is not one \"Cut chunk\" step");
        ctx.expect (host.pressAudioEditorKey ("command + Z"), "Undo was not handled");
    } });
    steps->push_back ({ 100, [&host, &ctx, &source, copied]
    {
        ctx.expect (sameRegions (source.regions, { copied }), "one Undo did not put the cut range back");
        const auto selection = host.audioEditorSelection();
        ctx.expect (selection.size() == 5 && selection[1] == 0, "Undo left the range drawn");
        ctx.expect (host.pressAudioEditorKey ("command + C"), "Copy was not handled");
    } });
    steps->push_back ({ 100, [&host, &ctx, &engine, copied]
    {
        const auto& clip = engine.getRegionClipboard();
        ctx.expect (clip.hasContent && clip.sourceTrack == 0 && sameRegions ({ clip.region }, { copied }),
                    "Copy with no range did not put the focused region on the clipboard");
        engine.getRegionClipboard() = {};
        ctx.expect (host.pressAudioEditorKey ("command + X"), "Cut was not handled");
    } });
    steps->push_back ({ 100, [&ctx, &engine, &source, copied]
    {
        const auto& clip = engine.getRegionClipboard();
        ctx.expect (clip.hasContent && clip.sourceTrack == 0 && sameRegions ({ clip.region }, { copied }),
                    "Cut with no range did not put the focused region on the clipboard");
        ctx.expect (source.regions.empty(), "Cut with no range left the region on the track");
        ctx.expect (undoDescription (engine) == "Cut region", "the region cut is not one \"Cut region\" step");
        auto& undo = engine.getUndoManager();
        undo.undo();
        ctx.expect (sameRegions (source.regions, { copied }) && ! undo.canUndo(), "one Undo did not put the cut region back");
    }, [&host] { return ! host.audioEditorOpen(); }, "cutting the focused region left the editor open" });
    steps->push_back ({ 100, [&host, &ctx, &engine]
    {
        const auto& clip = engine.getRegionClipboard();
        ctx.expect (clip.hasContent && clip.sourceTrack == 0, "the clipboard lost the first track's region");
        ctx.expect (host.openAudioEditor (1, 0), "the editor did not open on the second track");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickAudioEditorSample (96000 + 24000), "could not place the edit cursor"); },
                        laidOut, "the editor on the second track never laid out" });
    steps->push_back ({ 150, [&host, &ctx, cursor]
    {
        const auto view = host.audioEditorView();
        if (ctx.expect (view.size() == 3, "editor view unavailable"))
            *cursor = static_cast<std::int64_t> (view[2]);
        ctx.expect (host.pressAudioEditorKey ("command + V"), "Paste was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &engine, &source, &target, copied, shown, cursor]
    {
        ctx.expect (sameRegions (source.regions, { copied }), "Paste changed the track the copy came from");
        if (ctx.expect (target.regions.size() == 2, "Paste did not add a region to the track the editor shows"))
            ctx.expect (target.regions[1].timelineStart == shown.timelineStart + *cursor
                        && target.regions[1].lengthInSamples == copied.lengthInSamples,
                        "Paste did not land the copy at the edit cursor (start "
                            + std::to_string (target.regions[1].timelineStart) + ", cursor " + std::to_string (*cursor) + ")");
        ctx.expect (undoDescription (engine) == "Paste region", "the paste is not one \"Paste region\" step");
        auto& undo = engine.getUndoManager();
        undo.undo();
        ctx.expect (sameRegions (target.regions, { shown }) && sameRegions (source.regions, { copied }) && ! undo.canUndo(),
                    "one Undo did not take back just the paste");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorClipboardKeys { Scenario {
    "gui.audio_editor_clipboard_keys", { "gui", "keyboard", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorClipboardKeys (host, ctx); }
} };

// A key held at the shell's window repeats only where it repeats at the editor's
// child. Delete held deletes one region, the auto-repeats that follow doing nothing
// until the key comes up, and the next press deletes one more; Right held nudges the
// region once per repeat. Unfocused, the keys reach the window with no component
// holding the keyboard, as on Windows once the editor's child has taken it.
std::optional<ScenarioResult> runAudioEditorHeldKeys (GuiHost& host, ScenarioContext& ctx, bool unfocused)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto path = ctx.tempDir() / "held-delete.wav";
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 24 });
    if (writer == nullptr) return ScenarioResult::fail ("could not create the held-key fixture");
    std::vector<float> silence (48000, 0.0f);
    const float* channels[] { silence.data() };
    if (! writer->write (channels, 1, 48000) || ! writer->flush())
        return ScenarioResult::fail ("could not write the held-key fixture");
    writer.reset();
    AudioRegion region;
    region.file = decltype (region.file) (path.u8string().c_str());
    region.lengthInSamples = 48000;
    for (int i = 0; i < 4; ++i)
    {
        region.timelineStart = 96000 * i;
        track.regions.push_back (region);
    }
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto counted = [&ctx, &track] (std::size_t expected, const std::string& what)
    {
        ctx.expect (track.regions.size() == expected,
                    what + " left " + std::to_string (track.regions.size()) + " regions, not " + std::to_string (expected));
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, unfocused]
    {
        if (unfocused) ctx.cleanup (host.unfocusWindow());
        ctx.expect (host.holdPeerKey ("delete"), "the press of Delete was not handled");
    }, [&host] { return host.audioEditorPoint ("wave", 1000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 100, [&host, counted]
    {
        counted (3, "the press of Delete");
        host.holdPeerKey ("delete");
    } });
    for (int repeat = 0; repeat < 3; ++repeat)
        steps->push_back ({ 60, [&host] { host.holdPeerKey ("delete"); } });
    steps->push_back ({ 100, [&host, &ctx, counted]
    {
        counted (3, "holding Delete");
        ctx.expect (host.audioEditorOpen(), "holding Delete closed the editor");
        host.releasePeerKey();
        ctx.expect (host.pressPeerKey ("delete"), "Delete pressed again was not handled");
    } });
    const auto start = [&host, &track]
    {
        const auto index = static_cast<std::size_t> (host.audioEditorRegion());
        return index < track.regions.size() ? track.regions[index].timelineStart : std::int64_t { -1 };
    };
    const auto origin = std::make_shared<std::int64_t> (-1);
    const auto step = std::make_shared<std::int64_t> (0);
    steps->push_back ({ 100, [&host, &ctx, &engine, counted, start, origin]
    {
        counted (2, "Delete pressed again after the release");
        ctx.expect (host.audioEditorOpen(), "the second Delete closed the editor");
        ctx.expect (undoDescription (engine) == "Delete region", "the second Delete is not a \"Delete region\" step");
        *origin = start();
        host.pressPeerKey ("cursor right");
    } });
    steps->push_back ({ 100, [&host, &ctx, start, origin, step]
    {
        *step = start() - *origin;
        ctx.expect (*origin >= 0 && *step > 0, "Right did not nudge the region");
        *origin = start();
        host.holdPeerKey ("cursor right");
    } });
    for (int repeat = 0; repeat < 2; ++repeat)
        steps->push_back ({ 60, [&host] { host.holdPeerKey ("cursor right"); } });
    steps->push_back ({ 100, [&host, &ctx, start, origin, step]
    {
        host.releasePeerKey();
        ctx.expect (start() - *origin == 3 * *step, "Right held for two repeats moved the region "
                                                       + std::to_string (start() - *origin) + " samples, not three nudges of "
                                                       + std::to_string (*step));
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorHeldKeys { Scenario {
    "gui.audio_editor_held_keys", { "gui", "keyboard", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorHeldKeys (host, ctx, false); }
} };

const ScenarioRegistrar audioEditorHeldKeysUnfocused { Scenario {
    "gui.audio_editor_held_keys_unfocused", { "gui", "keyboard", "editor", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorHeldKeys (host, ctx, true); }
} };

// With the editor up, a key it does not take at the shell's window goes nowhere, as at
// its child: Delete pressed while the lane divider is held leaves the timeline's
// selected region alone, and S, X, M, Cmd+D and a page digit leave the selected track,
// the markers, the regions and the console page as they were. Space and ., which the
// child passes on, still reach the transport, and L and P still toggle loop and punch.
std::optional<ScenarioResult> runAudioEditorHoldsTimelineKeys (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    if (! addLevelTakes (ctx, track, 2)) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[1], kTakeCaseLength / 2, kTakeCaseLength);
    if (track.regions.size() != 2) return ScenarioResult::fail ("the setup did not leave two regions");
    track.strip.solo.store (false);
    track.strip.mute.store (false);
    const auto before = track.regions;
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, shown] { host.setTimelineShown (shown); });
    ctx.cleanup (host.preserveKeyboardFocus());
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

    const auto markers = session.getMarkers().size();
    const auto page = std::make_shared<int> (-1);
    const auto pageShown = [&host]
    {
        for (int index = 0; index < host.consolePageCount(); ++index)
            if (host.consolePageMatches (index)) return index;
        return -1;
    };
    const auto untouched = [&ctx, &session, &track, before, markers, page, pageShown] (const std::string& what)
    {
        ctx.expect (! track.strip.solo.load() && ! track.strip.mute.load(), what + " soloed or muted the selected track");
        ctx.expect (session.getMarkers().size() == markers, what + " dropped a marker");
        ctx.expect (sameRegions (track.regions, before), what + " changed the track's regions");
        ctx.expect (pageShown() == *page, what + " switched the console page");
    };
    const auto divider = std::make_shared<std::vector<int>>();
    const bool loop = transport.isLoopEnabled();
    const bool punch = transport.isPunchEnabled();
    ctx.cleanup ([&engine, &transport, loop, punch]
    {
        engine.stop();
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
    });

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0), "could not select the region on the timeline"); } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        ctx.expect (host.tapeSelectedTracks() == std::vector<int> { 0 }, "the timeline click did not select the track");
        ctx.expect (host.openAudioEditor (0, 0), "audio editor unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, page, pageShown, divider]
    {
        ctx.expect (host.tapeSelectedTracks() == std::vector<int> { 0 }, "opening the editor dropped the timeline's selection");
        *page = pageShown();
        *divider = host.audioEditorPoint ("divider", 0);
        ctx.expect (host.audioEditorPointer ((*divider)[0], (*divider)[1], true), "the divider did not take the press");
    }, [&host] { return host.audioEditorPoint ("divider", 0).size() == 2; }, "the editor never laid out its take lanes" });
    steps->push_back ({ 150, [&host, untouched]
    {
        host.pressPeerKey ("delete");
        untouched ("Delete pressed while the divider is held");
    } });
    steps->push_back ({ 100, [&host, divider] { host.audioEditorPointer ((*divider)[0], (*divider)[1], false); } });
    steps->push_back ({ 150, [&host, &ctx, page]
    {
        ctx.expect (host.tapeSelectedTracks() == std::vector<int> { 0 }, "the timeline's selection went before the keys");
        host.pressPeerKey (keyCodeDescription ('s'), 's');
        host.pressPeerKey (keyCodeDescription ('x'), 'x');
        host.pressPeerKey ("command + " + keyCodeDescription ('d'), controlCharacter ('d'));
        if (host.consolePageCount() > 1)
        {
            const char digit = static_cast<char> ('1' + (*page == 0 ? 1 : 0));
            host.pressPeerKey (std::string (1, digit), digit);
        }
        host.pressPeerKey (keyCodeDescription ('m'), 'm');
    } });
    steps->push_back ({ 150, [&host, &ctx, &engine, untouched]
    {
        untouched ("A key pressed at the shell over the editor");
        ctx.expect (undoDescription (engine).empty(), "the keys recorded \"" + undoDescription (engine) + "\"");
        ctx.expect (host.audioEditorOpen(), "the keys closed the editor");
        ctx.expect (host.pressPeerKey ("spacebar", ' '), "Space was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, &transport, loop, punch]
    {
        ctx.expect (host.pressPeerKey (".", '.'), ". was not handled");
        ctx.expect (transport.isStopped() && transport.getPlayhead() == 0, ". over the editor did not stop and rewind");
        host.pressPeerKey (keyCodeDescription ('l'), 'l');
        host.pressPeerKey (keyCodeDescription ('p'), 'p');
        ctx.expect (transport.isLoopEnabled() != loop, "L over the editor did not toggle the loop");
        ctx.expect (transport.isPunchEnabled() != punch, "P over the editor did not toggle punch");
    }, [&transport] { return transport.isPlaying(); }, "Space over the editor did not start playback" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorHoldsTimelineKeys { Scenario {
    "gui.audio_editor_holds_timeline_keys", { "gui", "keyboard", "editor", "take", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorHoldsTimelineKeys (host, ctx); }
} };

// Save is the session's key, not the timeline's: with the editor up on a saved session
// Cmd+S writes session.json with the editor's edits in it, whether the key reaches the
// shell's window or the editor's child, and Cmd+Shift+S closes the editor for the Save As
// browser. While a field is open for typing the chord is the field's and saves nothing,
// and S alone still does nothing.
std::optional<ScenarioResult> runAudioEditorSaveKeys (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto sessionJson = ctx.tempDir() / "Saved song" / "session.json";
    std::filesystem::create_directories (sessionJson.parent_path());
    applySessionDirectory (session, sessionJson.parent_path());
    if (! SessionSerializer::save (session, sessionJson)) return ScenarioResult::fail ("could not save the session fixture");
    reopenSavedSession (host, sessionJson);
    if (! host.sessionOnDisk() || ! host.modalStackEmpty())
        return ScenarioResult::fail ("the saved session fixture did not open");

    auto& track = session.track (0);
    if (! addLevelTakes (ctx, track, 2)) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[1], kTakeCaseLength / 2, kTakeCaseLength);
    if (track.regions.size() != 2) return ScenarioResult::fail ("the setup did not leave two regions");
    track.strip.solo.store (false);
    engine.getUndoManager().clearUndoHistory();
    ctx.cleanup (host.preserveKeyboardFocus());
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto savedRegions = [sessionJson]
    {
        Session probe;
        return SessionSerializer::load (probe, sessionJson) ? (int) probe.track (0).regions.size() : -1;
    };
    const auto shellChord = [&host] (char letter)
    { host.pressPeerKey ("command + " + keyCodeDescription (letter), controlCharacter (letter)); };
    const auto requests = std::make_shared<int> (0);
    const auto written = std::make_shared<std::string>();

    auto steps = std::make_shared<std::vector<Step>>();
    const auto rewritten = [sessionJson, written] { return fileBytes (sessionJson) != *written; };
    steps->push_back ({ 100, [&host, &ctx, sessionJson, written, savedRegions]
    {
        *written = fileBytes (sessionJson);
        ctx.expect (savedRegions() == 0, "the fixture session was saved with regions on its first track");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not delivered to the editor");
    }, [&host] { return host.audioEditorPoint ("wave", 12000).size() == 2; }, "the editor never laid out" });
    steps->push_back ({ 100, [shellChord] { shellChord ('s'); },
                        [&track] { return track.regions.size() == 1; }, "Delete did not delete the focused region" });
    steps->push_back ({ 100, [&host, &ctx, sessionJson, written, savedRegions]
    {
        ctx.expect (savedRegions() == 1, "Save at the shell's window wrote the session without the editor's edit");
        ctx.expect (host.audioEditorOpen(), "Save at the shell's window closed the editor");
        *written = fileBytes (sessionJson);
        ctx.expect (host.pressAudioEditorKey ("command + Z"), "Undo was not delivered to the editor");
    }, rewritten, "Save pressed at the shell's window over the editor did not write session.json" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + S"), "Save was not delivered to the editor"); },
    [&track] { return track.regions.size() == 2; }, "Undo did not bring the region back" });
    steps->push_back ({ 100, [&host, &ctx, sessionJson, written, savedRegions]
    {
        ctx.expect (savedRegions() == 2, "Save at the editor's child wrote the session without the editor's undo");
        ctx.expect (host.audioEditorOpen(), "Save at the editor's child closed the editor");
        *written = fileBytes (sessionJson);
        host.pressPeerKey (keyCodeDescription ('s'), 's');
        ctx.expect (host.pressAudioEditorKey ("S"), "S was not delivered to the editor");
        ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not delivered to the editor");
    }, rewritten, "Save pressed at the editor's child did not write session.json" });
    steps->push_back ({ 100, [&host, &ctx, requests]
    {
        *requests = host.audioEditorKeyboardRequests();
        ctx.expect (host.doubleClickAudioEditorButton ("Gain"), "no gain readout to double-click");
    }, [&track] { return track.regions.size() == 1; }, "the second Delete did not delete the focused region" });
    steps->push_back ({ 100, [&host, &ctx, shellChord]
    {
        ctx.expect (host.pressAudioEditorKey ("command + S"), "Save was not delivered to the open field");
        ctx.expect (host.audioEditorKeyboardFocus (false), "the editor's child could not give up the keyboard");
        shellChord ('s');
    }, [&host, requests] { return host.audioEditorKeyboardRequests() > *requests; },
    "a double-click on the gain readout did not open it for typing" });
    steps->push_back ({ 300, [&host, &ctx, &track, sessionJson, written]
    {
        ctx.expect (fileBytes (sessionJson) == *written,
                    "S alone, or Save typed into the open gain field, wrote session.json");
        ctx.expect (! track.strip.solo.load(), "S over the editor soloed the track behind it");
        ctx.expect (host.audioEditorOpen(), "Save typed into the open gain field closed the editor");
        ctx.expect (host.pressAudioEditorKey ("escape"), "Escape was not delivered to the field");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + shift + S"), "Save As was not delivered to the editor"); } });
    steps->push_back ({ 100, [&host, &ctx, sessionJson, written]
    {
        ctx.expect (host.focusFileName(), "Save As over the editor did not open its browser");
        ctx.expect (fileBytes (sessionJson) == *written, "Save As wrote session.json before it asked where");
        drainModals (host);
    }, [&host] { return ! host.audioEditorOpen() && ! host.modalStackEmpty(); },
    "Save As pressed at the editor's child did not close the editor for its browser" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorSaveKeys { Scenario {
    "gui.audio_editor_save_keys", { "gui", "keyboard", "editor", "take" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorSaveKeys (host, ctx); }
} };

// Quit is the application's key: with the editor up over unsaved edits Cmd+Q raises the
// quit prompt, whether the key reaches the shell's window or the editor's child, and the
// editor closes for it. While a field is open for typing the chord is the field's.
std::optional<ScenarioResult> runAudioEditorQuitKey (GuiHost& host, ScenarioContext& ctx)
{
    if (auto early = beginTakeCase (host, ctx)) return early;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.isAudioCallbackRegistered() || ! host.autosaveRunning() || host.engineDetached())
        return ScenarioResult::skip ("requires live audio and autosave");
    const auto sessionJson = ctx.tempDir() / "Saved song" / "session.json";
    std::filesystem::create_directories (sessionJson.parent_path());
    applySessionDirectory (session, sessionJson.parent_path());
    if (! SessionSerializer::save (session, sessionJson)) return ScenarioResult::fail ("could not save the session fixture");
    reopenSavedSession (host, sessionJson);
    if (! host.sessionOnDisk() || ! host.modalStackEmpty())
        return ScenarioResult::fail ("the saved session fixture did not open");

    auto& track = session.track (0);
    if (! addLevelTakes (ctx, track, 2)) return ScenarioResult::fail ("could not write take fixture");
    promoteTakeRange (session, track, track.takes[0], 0, kTakeCaseLength);
    promoteTakeRange (session, track, track.takes[1], kTakeCaseLength / 2, kTakeCaseLength);
    if (track.regions.size() != 2) return ScenarioResult::fail ("the setup did not leave two regions");
    engine.getUndoManager().clearUndoHistory();
    ctx.cleanup ([&engine] { engine.reattachAudioCallback(); });
    ctx.cleanup (host.preserveKeyboardFocus());
    if (! host.openAudioEditor (0, 0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto written = fileBytes (sessionJson);
    const auto shellQuit = [&host]
    { host.pressPeerKey ("command + " + keyCodeDescription ('q'), controlCharacter ('q')); };
    const auto laidOut = [&host] { return host.audioEditorPoint ("wave", 12000).size() == 2; };
    const auto requests = std::make_shared<int> (0);
    // A session with nothing to save quits at the chord, which ends the run.
    const auto unsaved = [&ctx, &track]
    { return ctx.expect (track.regions.size() == 1, "the editor's Delete left the session as it was saved"); };
    const auto cancelPrompt = [&host, &ctx, sessionJson, written] (const std::string& how)
    {
        if (! ctx.expect (host.modalText().rfind ("Save changes before quitting?", 0) == 0,
                          how + " showed '" + host.modalText() + "' rather than the quit prompt"))
            return;
        ctx.expect (! host.audioEditorOpen(), how + " left the editor up over the quit prompt");
        ctx.expect (fileBytes (sessionJson) == written, how + " saved the session before the prompt was answered");
        ctx.expect (host.clickModalButton ("Cancel"), "the quit prompt did not offer Cancel");
    };
    const auto running = [&host, &ctx, &engine, &track] (const std::string& how)
    {
        ctx.expect (host.modalStackEmpty(), "Cancel after " + how + " left '" + host.modalText() + "' up");
        ctx.expect (engine.isAudioCallbackRegistered() && ! host.engineDetached() && host.autosaveRunning(),
                    "Cancel after " + how + " left audio or autosave parked");
        ctx.expect (track.regions.size() == 1, "Cancel after " + how + " did not leave the session as it was");
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("delete"), "Delete was not delivered to the editor"); },
    laidOut, "the editor never laid out" });
    steps->push_back ({ 100, [&ctx, unsaved, shellQuit]
    {
        if (! unsaved()) { ctx.complete (ctx.verdict()); return; }
        shellQuit();
    }, [&track] { return track.regions.size() == 1; }, "Delete did not delete the focused region" });
    steps->push_back ({ 100, [cancelPrompt] { cancelPrompt ("Quit at the shell's window"); },
                        [&host] { return ! host.modalStackEmpty(); },
                        "Quit pressed at the shell's window over the editor did not ask about the unsaved edit" });
    steps->push_back ({ 400, [&host, &ctx, running]
    {
        running ("Quit at the shell's window");
        ctx.expect (host.openAudioEditor (0, 0), "the editor did not reopen after the cancelled quit");
    } });
    steps->push_back ({ 100, [&host, &ctx, unsaved]
    {
        if (! unsaved()) { ctx.complete (ctx.verdict()); return; }
        ctx.expect (host.pressAudioEditorKey ("command + Q"), "Quit was not delivered to the editor");
    }, laidOut, "the reopened editor never laid out" });
    steps->push_back ({ 100, [cancelPrompt] { cancelPrompt ("Quit at the editor's child"); },
                        [&host] { return ! host.modalStackEmpty(); },
                        "Quit pressed at the editor's child did not ask about the unsaved edit" });
    steps->push_back ({ 400, [&host, &ctx, running]
    {
        running ("Quit at the editor's child");
        ctx.expect (host.openAudioEditor (0, 0), "the editor did not reopen after the second cancelled quit");
    } });
    steps->push_back ({ 100, [&host, &ctx, requests]
    {
        *requests = host.audioEditorKeyboardRequests();
        ctx.expect (host.doubleClickAudioEditorButton ("Gain"), "no gain readout to double-click");
    }, laidOut, "the editor never laid out for the field" });
    steps->push_back ({ 100, [&host, &ctx, unsaved, shellQuit]
    {
        if (! unsaved()) { ctx.complete (ctx.verdict()); return; }
        ctx.expect (host.pressAudioEditorKey ("command + Q"), "Quit was not delivered to the open field");
        ctx.expect (host.audioEditorKeyboardFocus (false), "the editor's child could not give up the keyboard");
        shellQuit();
    }, [&host, requests] { return host.audioEditorKeyboardRequests() > *requests; },
    "a double-click on the gain readout did not open it for typing" });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (host.modalStackEmpty(), "Quit typed into the open gain field showed '" + host.modalText() + "'");
        ctx.expect (host.audioEditorOpen(), "Quit typed into the open gain field closed the editor");
        ctx.expect (host.pressAudioEditorKey ("escape"), "Escape was not delivered to the field");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorQuitKey { Scenario {
    "gui.audio_editor_quit_key", { "gui", "keyboard", "editor", "take", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorQuitKey (host, ctx); }
} };

// Cmd+Y redoes in the editor as Cmd+Shift+Z does, at the editor's child or the shell's
// window: the editor stays up on the region it was showing.
std::optional<ScenarioResult> runAudioEditorRedoKey (GuiHost& host, ScenarioContext& ctx)
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
    ctx.cleanup (host.preserveKeyboardFocus());
    if (! host.openAudioEditorOnTakes (0)) return ScenarioResult::fail ("audio editor unavailable");

    const auto focusedStart = [&host, &track]
    {
        const auto selection = host.audioEditorSelection();
        if (selection.size() != 5 || selection[0] < 0 || selection[0] >= (std::int64_t) track.regions.size())
            return std::int64_t { -1 };
        return track.regions[(std::size_t) selection[0]].timelineStart;
    };
    const auto promoted = [&track, other] { return ! takeCoverage (track, other).empty(); };
    const auto stillThere = [&host, &ctx, focusedStart] (const std::string& what)
    {
        ctx.expect (host.audioEditorOpen(), what + " closed the editor");
        ctx.expect (focusedStart() == 72000, "after " + what + " the editor shows the region at "
                                                 + std::to_string (focusedStart()) + " rather than the one it showed");
    };
    const auto shellChord = [&host] (char letter)
    { host.pressPeerKey ("command + " + keyCodeDescription (letter), controlCharacter (letter)); };

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
    promoted, "the drag did not promote the take" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + Z"), "Undo was not delivered to the editor"); },
    [focusedStart] { return focusedStart() == 72000; }, "the click did not focus the last region" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + Y"), "Cmd+Y was not delivered to the editor"); },
    [promoted] { return ! promoted(); }, "Undo at the editor's child did not take the promote back" });
    steps->push_back ({ 100, [stillThere, shellChord]
    {
        stillThere ("Cmd+Y at the editor's child");
        shellChord ('z');
    }, promoted, "Cmd+Y pressed at the editor's child did not redo the promote" });
    steps->push_back ({ 100, [shellChord] { shellChord ('y'); },
                        [promoted] { return ! promoted(); }, "Undo at the shell's window did not take the promote back" });
    steps->push_back ({ 100, [&ctx, &engine = ctx.engine(), stillThere]
    {
        stillThere ("Cmd+Y at the shell's window");
        ctx.expect (! engine.getUndoManager().canRedo(), "Cmd+Y left the promote to redo");
    }, promoted, "Cmd+Y pressed at the shell's window over the editor did not redo the promote" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorRedoKey { Scenario {
    "gui.audio_editor_redo_key", { "gui", "keyboard", "editor", "take", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorRedoKey (host, ctx); }
} };

std::optional<ScenarioResult> runAudioEditorKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (0);
    const bool expanded = host.timelineViewMatches (true);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &ctx, &session, &track, expanded, regions = track.regions, mode = session.editMode]
    {
        host.closeAudioEditor();
        track.regions = regions;
        session.editMode = mode;
        ctx.engine().getUndoManager().clearUndoHistory();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    const auto path = ctx.tempDir() / "editor-keys.wav";
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the editor keyboard fixture")) return ctx.verdict();
    std::vector<float> silence (240000, 0.0f);
    const float* channels[] { silence.data() };
    if (! ctx.expect (writer->write (channels, 1, 240000), "could not write the editor keyboard fixture")) return ctx.verdict();
    writer.reset();
    AudioRegion first;
    first.file = decltype (first.file) (path.u8string().c_str());
    first.sourceOffset = 12000;
    first.lengthInSamples = 192000;
    auto second = first;
    second.timelineStart = 384000;
    second.sourceOffset = 24000;
    second.lengthInSamples = 96000;
    track.regions = { first, second };
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    ctx.engine().getUndoManager().clearUndoHistory();
    if (! expanded) host.pressKey ("T", 't');
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.doubleClickAudioRegion (0, 0), "could not open the first region"); } });
    steps->push_back ({ 300, [&host, &ctx, &session]
    {
        session.editMode = EditMode::Range;
        ctx.expect (host.pressPeerKey ("G", 'g'), "the editor did not handle G");
        ctx.expect (session.editMode == EditMode::Grab, "G did not select Grab mode");
        ctx.expect (host.clickAudioEditorWaveform(), "could not place the edit cursor with the mouse");
    } });
    // The click releases a frame later, and keys wait while the button is down.
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressAudioEditorKey ("command + E"), "the editor did not handle Split"); } });
    steps->push_back ({ 100, [&host, &ctx, &track, first]
    {
        if (ctx.expect (track.regions.size() == 3, "Split did not create a third region"))
        {
            std::vector<AudioRegion> slices;
            for (const auto& region : track.regions)
                if (region.timelineStart < 384000) slices.push_back (region);
            std::sort (slices.begin(), slices.end(), [] (const auto& a, const auto& b)
            { return a.timelineStart < b.timelineStart; });
            if (ctx.expect (slices.size() == 2, "Split affected the wrong region"))
                ctx.expect (slices[0].lengthInSamples > 0 && slices[1].lengthInSamples > 0
                            && slices[0].lengthInSamples + slices[1].lengthInSamples == first.lengthInSamples
                            && slices[0].sourceOffset == first.sourceOffset
                            && slices[1].sourceOffset == first.sourceOffset + slices[0].lengthInSamples
                            && slices[1].timelineStart == slices[0].lengthInSamples,
                            "Split did not preserve contiguous source and timeline spans");
        }
        ctx.expect (host.pressAudioEditorKey ("command + Z"), "the editor did not handle Undo");
    } });
    steps->push_back ({ 100, [&host, &ctx, &track]
    {
        ctx.expect (track.regions.size() == 2, "Undo did not restore the two original regions");
        ctx.expect (host.pressAudioEditorKey ("command + ]"), "the editor did not handle next region");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.audioEditorRegion() == 1, "next region did not open the second region");
        ctx.expect (host.pressAudioEditorKey ("command + ["), "the editor did not handle previous region");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.audioEditorRegion() == 0, "previous region did not return to the first region");
        ctx.expect (host.pressPeerKey ("escape"), "the editor did not handle Escape");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (! host.audioEditorOpen(), "Escape did not close the editor"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar audioEditorKeys { Scenario {
    "gui.audio_editor_keys", { "gui", "keyboard", "editor" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAudioEditorKeys (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
