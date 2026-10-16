#include "../GuiScenarioSupport.h"
#include "../../AppConfig.h"
#include "../../WheelScroll.h"
#include "../../../session/ParamEditAction.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/SessionSerializer.h"
#include "../../../engine/scenario/cases/MidiProbeHarness.h"

#if DUSKSTUDIO_HAS_NATIVE_UI
 #include "../../imgui/DuskPanelWindow.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
std::optional<ScenarioResult> runPianoSelection (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        host.pianoNotePointer (1200, 60, false);
        host.closePiano();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    MidiRegion region;
    region.lengthInTicks = 7680;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    region.notes = { { 1, 60, 100, 1200, 480 }, { 1, 64, 90, 2400, 480 }, { 1, 67, 80, 3600, 480 } };
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { region }));
    session.midiEditorSnap = true;
    if (! host.openPiano (0, 0)) return ScenarioResult::fail ("piano roll did not open");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("G", 'g'), "Grab key was not handled"); } });
    struct Click { std::int64_t tick; int pitch, modifiers; std::vector<int> expected; };
    for (const auto& click : std::vector<Click> {
             { 1320, 60, 0, { 0 } }, { 2520, 64, 1, { 0, 1 } }, { 2520, 64, 1, { 0 } },
             { 2520, 64, 2, { 0, 1 } }, { 1320, 60, 2, { 1 } }, { 1320, 60, 0, { 0 } } })
    {
        steps->push_back ({ 150, [&host, &ctx, click]
        {
            ctx.expect (host.pianoNotePointer (click.tick, click.pitch, true, click.modifiers), "note click failed");
            ctx.expect (host.pianoNotePointer (click.tick, click.pitch, false, click.modifiers), "note release failed");
        } });
        steps->push_back ({ 0, [&host, &ctx, click]
        {
            auto actual = host.pianoSelection();
            std::sort (actual.begin(), actual.end());
            ctx.expect (actual == click.expected, "note click produced the wrong selection");
        } });
    }
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (1320, 60, true); } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (1700, 62, true); } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (1700, 62, false); } });
    steps->push_back ({ 150, [&ctx, &session]
    {
        const auto& notes = session.track (0).midiRegions.current()[0].notes;
        ctx.expect (notes.size() == 3, "move changed note count");
        if (notes.size() != 3) return;
        ctx.expect (notes[0].startTick == 1560 && notes[0].noteNumber == 62 && notes[0].lengthInTicks == 480,
                    "body drag did not snap movement and transpose the note");
        ctx.expect (notes[1].startTick == 2400 && notes[2].startTick == 3600, "move changed unselected notes");
    } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (2040, 62, true); } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (2520, 62, true); } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (2520, 62, false); } });
    steps->push_back ({ 150, [&ctx, &session]
    {
        const auto& notes = session.track (0).midiRegions.current()[0].notes;
        if (! ctx.expect (! notes.empty(), "resize removed the note")) return;
        const auto& note = notes[0];
        ctx.expect (note.startTick == 1560 && note.lengthInTicks == 960, "right-edge drag did not resize the note");
    } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (1000, 69, true); } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (4200, 59, true); } });
    steps->push_back ({ 150, [&host] { host.pianoNotePointer (4200, 59, false); } });
    steps->push_back ({ 150, [&ctx, &host]
    {
        auto selected = host.pianoSelection();
        std::sort (selected.begin(), selected.end());
        ctx.expect (selected == std::vector<int> { 0, 1, 2 }, "rubber band did not select all enclosed notes");
        ctx.expect (host.pressPeerKey ("backspace", '\b'), "Backspace was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &session]
    { ctx.expect (session.track (0).midiRegions.current()[0].notes.empty(), "Backspace did not delete selected notes"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoSelection { Scenario {
    "gui.piano_selection", { "gui", "piano" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoSelection (host, ctx); }
} };

std::optional<ScenarioResult> runPianoCcEditing (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        host.pianoCcPointer (1200, 64, false);
        host.closePiano();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    MidiRegion region;
    region.lengthInTicks = 7680;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::initializer_list<MidiRegion> { region }));
    session.midiEditorSnap = true;
    if (! host.openPiano (0, 0)) return ScenarioResult::fail ("piano roll did not open");
    const auto check = [&ctx, &session] (size_t count, int controller, int value, std::int64_t tick)
    {
        const auto& events = session.track (0).midiRegions.current()[0].ccs;
        ctx.expect (events.size() == count, "unexpected number of CC events");
        const auto event = std::find_if (events.begin(), events.end(), [controller, tick] (const auto& cc)
        { return cc.controller == controller && cc.atTick == tick; });
        if (! ctx.expect (event != events.end(), "CC event has wrong controller or tick")) return;
        ctx.expect (event->channel == 1, "CC event has wrong MIDI channel");
        ctx.expect (std::abs (event->value - value) <= 1,
                    "painted CC position has wrong value: " + std::to_string (event->value));
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickPianoCcToggle(), "CC toolbar button is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.pianoCcController() == 1, "default CC is not Mod Wheel");
        ctx.expect (host.pianoCcPointer (1200, 64, true), "CC click failed");
        host.pianoCcPointer (1200, 64, false);
    } });
    steps->push_back ({ 150, [check, &host] { check (1, 1, 64, 1200); host.pianoCcPointer (1200, 64, true); } });
    steps->push_back ({ 150, [&host] { host.pianoCcPointer (1200, 100, true); } });
    steps->push_back ({ 150, [&host] { host.pianoCcPointer (1200, 100, false); } });
    steps->push_back ({ 150, [check, &host, &ctx]
    {
        check (1, 1, 100, 1200);
        ctx.expect (host.pressPeerKey ("L", 'l'), "controller key was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        ctx.expect (host.pianoCcController() == 7, "controller did not change to Volume");
        host.pianoCcPointer (2400, 32, true);
        host.pianoCcPointer (2400, 32, false);
    } });
    steps->push_back ({ 150, [check] { check (2, 7, 32, 2400); check (2, 1, 100, 1200); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoCcEditing { Scenario {
    "gui.piano_cc_editing", { "gui", "piano" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoCcEditing (host, ctx); }
} };

std::optional<ScenarioResult> runPianoOptions (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        drainModals (host);
        host.closeRegionEditors();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    MidiRegion region;
    region.lengthInTicks = 960;
    region.lengthInSamples = session.ticksToSamples (960, engine.getCurrentSampleRate());
    for (const auto tick : { 48, 196 })
    {
        MidiNote note;
        note.noteNumber = 60;
        note.velocity = 80;
        note.startTick = tick;
        note.lengthInTicks = 120;
        region.notes.push_back (note);
    }
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    host.switchToStage (GuiHost::Stage::Recording);
    const auto checkTicks = [&ctx, &session] (int first, int second)
    {
        const auto& regions = session.track (0).midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().notes.size() == 2, "quantize changed note count");
        if (regions.size() == 1 && regions.front().notes.size() == 2)
        {
            const auto& notes = regions.front().notes;
            ctx.expect (notes[0].startTick == first && notes[1].startTick == second, "quantize used the wrong grid or strength");
            ctx.expect (notes[0].lengthInTicks == 120 && notes[1].lengthInTicks == 120, "quantize changed note lengths");
        }
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 0, true), "piano roll did not open"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('q'), 'q'), "Q was not handled"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("1/16 @ 75%"), "75-percent quantize row was not clicked"); } });
    steps->push_back ({ 150, [&host, &ctx, checkTicks]
    {
        checkTicks (12, 229);
        ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('q'), 'q'), "second Q was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("1/16 @ 100%"), "full-strength quantize row was not clicked"); } });
    steps->push_back ({ 150, [&host, &ctx, checkTicks]
    {
        checkTicks (0, 240);
        ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('s'), 's'), "S was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Major"), "Major scale submenu was not clicked"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("D"), "D scale root was not clicked"); } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        const auto options = host.pianoOptions();
        ctx.expect (options[0] == 1 && options[1] == 2, "scale picker did not select D Major");
    } });
    for (const int controller : { 7, 11, 64, 74, 1 })
    {
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('l'), 'l'), "L was not handled"); } });
        steps->push_back ({ 100, [&host, &ctx, controller]
        { ctx.expect (host.pianoOptions()[2] == controller, "L cycled to the wrong CC"); } });
    }
    for (const int colour : { 1, 2, 0 })
    {
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('c'), 'c'), "C was not handled"); } });
        steps->push_back ({ 100, [&host, &ctx, colour]
        { ctx.expect (host.pianoOptions()[3] == colour, "C cycled to the wrong note-colour mode"); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoOptions { Scenario {
    "gui.piano_options", { "gui", "piano" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoOptions (host, ctx); }
} };

std::optional<ScenarioResult> runPianoViewport (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        host.closeRegionEditors();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    MidiRegion region;
    region.lengthInTicks = 100000;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    host.switchToStage (GuiHost::Stage::Recording);
    auto before = std::make_shared<std::array<double, 4>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 0, true), "piano roll did not open"); } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        *before = host.pianoViewport();
        ctx.expect (host.pressPeerKey ("=", '='), "zoom-in key was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        ctx.expect (host.pianoViewport()[0] > (*before)[0], "equals did not zoom in");
        ctx.expect (host.pressPeerKey ("-", '-'), "zoom-out key was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        ctx.expect (std::abs (host.pianoViewport()[0] - (*before)[0]) < 1.0e-6, "minus did not restore the zoom");
        *before = host.pianoViewport();
        ctx.expect (host.scrollPiano (notch (-1.0f), false, false, false), "vertical wheel was not delivered");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        const auto after = host.pianoViewport();
        ctx.expect (after[2] > (*before)[2] && std::abs (after[0] - (*before)[0]) < 1.0e-6,
                    "unmodified wheel did not scroll the pitch range");
        *before = after;
        ctx.expect (host.scrollPiano (notch (-1.0f), false, true, false), "Shift wheel was not delivered");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        const auto after = host.pianoViewport();
        ctx.expect (after[1] > (*before)[1] && std::abs (after[2] - (*before)[2]) < 0.1,
                    "Shift wheel did not scroll horizontally");
        *before = after;
        ctx.expect (host.scrollPiano (notch (1.0f), true, false, false), "command wheel was not delivered");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        const auto after = host.pianoViewport();
        ctx.expect (after[0] > (*before)[0] && std::abs (after[2] - (*before)[2]) < 0.1,
                    "command wheel did not zoom horizontally");
        ctx.expect (host.clickPianoFit(), "Zoom fit button was not clicked");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    {
        const auto after = host.pianoViewport();
        ctx.expect (std::abs (after[1]) < 0.1, "Zoom fit did not return to the region start");
        ctx.expect (std::abs (after[0] * 100000.0 - after[3]) < 1.0, "Zoom fit did not fit the whole region");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoViewport { Scenario {
    "gui.piano_viewport", { "gui", "piano" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoViewport (host, ctx); }
} };

// The piano roll's pitch scroll and zoom under macOS wheel and trackpad sizes:
// a click moves at least a key, single points add up, and a trackpad stream
// zooms by the notches it travels instead of a whole step per event.
std::optional<ScenarioResult> runPianoWheelSizes (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr double kKeyH = 16.0;
    static constexpr int kPoints = 16;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        wheel::setProfileForScenario (wheel::kPlatformProfile);
        host.closeRegionEditors();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    MidiRegion region;
    region.lengthInTicks = 100000;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    host.switchToStage (GuiHost::Stage::Recording);
    const auto pitch = [&host] { return host.pianoViewport()[2]; };
    const auto zoom = [&host] { return host.pianoViewport()[0]; };
    const auto before = std::make_shared<double> (0.0);
    const auto wheelOf = [&host, &ctx] (float delta, bool command, bool smooth, int events)
    {
        for (int i = 0; i < events; ++i)
            ctx.expect (host.scrollPiano (delta, command, false, smooth), "the piano roll did not take the wheel");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 0, true), "piano roll did not open"); } });
    steps->push_back ({ 0, [pitch, before, wheelOf]
    {
        wheel::setProfileForScenario (wheel::kMacProfile);
        *before = pitch();
        wheelOf (-wheel::kMacProfile.lineNotchDelta, false, false, 1);
    }, [&host] { return host.pianoRollOpen(); }, "the piano roll did not come up" });
    steps->push_back ({ 0, [&ctx, pitch, before, wheelOf]
    {
        ctx.expect (pitch() - *before >= kKeyH, "a macOS wheel click scrolled the piano roll less than a key");
        *before = pitch();
        wheelOf (-wheel::kSmoothDeltaPerPoint, false, true, kPoints);
    }, [pitch, before] { return pitch() > *before; }, "a macOS wheel click did not scroll the piano roll" });
    steps->push_back ({ 0, [&ctx, pitch, zoom, before, wheelOf]
    {
        ctx.expect (pitch() - *before >= kPoints - 1, "single trackpad points did not follow the fingers");
        *before = zoom();
        wheelOf (wheel::kSmoothDeltaPerPoint, true, true, static_cast<int> (wheel::kPointsPerNotch));
    }, [pitch, before] { return pitch() > *before; }, "single trackpad points did not scroll the piano roll" });
    steps->push_back ({ 0, [&ctx, zoom, before]
    {
        const double ratio = zoom() / *before;
        ctx.expect (ratio > 1.14 && ratio < 1.16,
                    "a notch of trackpad travel zoomed by " + std::to_string (ratio) + " instead of one step");
    }, [zoom, before] { return zoom() > *before; }, "a trackpad stream with command did not zoom the piano roll" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoWheelSizes { Scenario {
    "gui.piano_wheel_sizes", { "gui", "piano" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoWheelSizes (host, ctx); }
} };

std::optional<ScenarioResult> runPianoStepRecord (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native virtual keyboard");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    const int centre = appconfig::getVkbCentreNote();
    if (! transport.isStopped() || host.virtualKeyboardOpen())
        return ScenarioResult::skip ("requires stopped transport and a closed virtual keyboard");
    if (centre < 0 || centre > 115) return ScenarioResult::skip ("requires the typing notes below MIDI 128");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalPosition = transport.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &transport, originalDir, originalStage, originalPosition, restore]
    {
        host.closeVirtualKeyboard();
        host.closeRegionEditors();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        transport.setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });
    const auto rate = engine.getCurrentSampleRate();
    MidiRegion region;
    region.lengthInTicks = 240;
    region.lengthInSamples = session.ticksToSamples (240, rate);
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    host.switchToStage (GuiHost::Stage::Recording);
    transport.setPlayhead (session.ticksToSamples (480, rate));
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 0, true), "piano roll did not open"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("K", 'k'), "K did not reach the piano roll"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.virtualKeyboardOpen(), "virtual keyboard did not open above the piano roll");
        ctx.expect (host.inputVirtualKeyboard ("z") && host.inputVirtualKeyboard ("x"),
                    "chord keys did not reach the native keyboard");
    } });
    steps->push_back ({ 600, [&host, &ctx, &session, &transport, centre, rate]
    {
        const auto& regions = session.track (0).midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().notes.size() == 2, "first chord did not insert two notes");
        if (regions.size() == 1 && regions.front().notes.size() == 2)
        {
            const auto& notes = regions.front().notes;
            for (int i = 0; i < 2; ++i)
            {
                ctx.expect (notes[(std::size_t) i].noteNumber == centre + 2 * i, "chord has the wrong pitch");
                ctx.expect (notes[(std::size_t) i].startTick == 480 && notes[(std::size_t) i].lengthInTicks == 120,
                            "chord did not share the playhead and snap length");
            }
            ctx.expect (regions.front().lengthInTicks == 600, "step record did not extend the region");
        }
        ctx.expect (transport.getPlayhead() == session.ticksToSamples (480, rate),
                    "releasing the chord advanced before the next note");
        ctx.expect (host.inputVirtualKeyboard ("q"), "next chord key did not reach the native keyboard");
    } });
    steps->push_back ({ 600, [&ctx, &session, &transport, centre, rate]
    {
        const auto& regions = session.track (0).midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().notes.size() == 3, "next chord did not insert a note");
        if (regions.size() == 1 && regions.front().notes.size() == 3)
        {
            const auto& note = regions.front().notes.back();
            ctx.expect (note.noteNumber == centre + 12 && note.startTick == 600 && note.lengthInTicks == 120,
                        "next chord did not advance by one snap step");
            ctx.expect (regions.front().lengthInTicks == 720, "next chord did not extend the region again");
        }
        ctx.expect (transport.getPlayhead() == session.ticksToSamples (600, rate) && transport.isStopped(),
                    "step record changed the transport state or advanced the wrong distance");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("K", 'k'), "K did not reach the piano roll a second time"); } });
    // The panel's close is deferred, so the verdict waits for it rather than
    // sampling after a fixed delay.
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.virtualKeyboardOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "K did not close the virtual keyboard");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pianoStepRecord { Scenario {
    "gui.piano_step_record", { "gui", "piano", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoStepRecord (host, ctx); }
} };

std::optional<ScenarioResult> beginMidiCaptureCase (GuiHost& host, ScenarioContext& ctx, MidiRegion seed)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty() || host.virtualKeyboardOpen())
        return ScenarioResult::skip ("requires stopped transport, no modal and no virtual keyboard");
    if (engine.getCurrentSampleRate() <= 0.0 || engine.getVirtualKeyboardInputIndex() < 0)
        return ScenarioResult::skip ("requires an audio device and virtual MIDI input");
    auto& track = session.track (0);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiChannel);
    ctx.keep (session.countInEnabled);
    ctx.keep (session.midiMergeEnabled);
    ctx.keep (session.tempoBpm);
    ctx.keep (session.lastRecordPointSamples);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, index == 0);
    }
    keepStage (host, ctx);
    ctx.cleanup ([&host, &engine, &session, &track, &transport,
                  regions = track.midiRegions.current(), directory = currentSessionDirectory (session), tempo = session.tempoMap,
                  at = transport.getPlayhead(), rollStart = transport.getRollStart(), loop = transport.isLoopEnabled(),
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled(), punchIn = transport.getPunchIn(), punchOut = transport.getPunchOut(),
                  savedLoopStart = session.savedLoopStart, savedLoopEnd = session.savedLoopEnd,
                  savedLoopEnabled = session.savedLoopEnabled, savedPunchIn = session.savedPunchIn,
                  savedPunchOut = session.savedPunchOut, savedPunchEnabled = session.savedPunchEnabled]
    {
        engine.stop();
        host.closeVirtualKeyboard();
        host.closeRegionEditors();
        drainModals (host);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        session.tempoMap = tempo;
        applySessionDirectory (session, directory);
        engine.getUndoManager().clearUndoHistory();
        transport.setPlayhead (at);
        transport.setRollStart (rollStart);
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (loop);
        transport.setPunchRange (punchIn, punchOut);
        transport.setPunchEnabled (punch);
        session.savedLoopStart = savedLoopStart;
        session.savedLoopEnd = savedLoopEnd;
        session.savedLoopEnabled = savedLoopEnabled;
        session.savedPunchIn = savedPunchIn;
        session.savedPunchOut = savedPunchOut;
        session.savedPunchEnabled = savedPunchEnabled;
    });
    ctx.setSessionDirectory (ctx.tempDir() / "capture");
    session.countInEnabled.store (false);
    session.midiMergeEnabled.store (false);
    session.tempoBpm.store (120.0f);
    session.tempoMap.clear();
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiChannel.store (1);
    seed.lengthInSamples = session.ticksToSamples (seed.lengthInTicks, engine.getCurrentSampleRate());
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { seed }));
    engine.getUndoManager().clearUndoHistory();
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    transport.setPlayhead (0);
    host.switchToStage (GuiHost::Stage::Recording);
    return std::nullopt;
}

std::optional<ScenarioResult> runPianoMidiMergeToggle (GuiHost& host, ScenarioContext& ctx)
{
    MidiRegion seed;
    seed.lengthInTicks = 1920;
    seed.notes = { { 1, 60, 90, 0, 480 } };
    if (auto result = beginMidiCaptureCase (host, ctx, seed)) return result;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto window = host.mainWindowSize();
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, window, timeline]
    {
        if (window.size() >= 2) host.resizeMainWindow (window[0], window[1]);
        host.setTimelineShown (timeline);
    });
    const auto controls = [&host] (bool on, bool enabled)
    {
        return host.midiMergeControls() == std::array<int, 4> { on ? 1 : 0, enabled ? 1 : 0,
                                                                on ? 1 : 0, enabled ? 1 : 0 };
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 0, true), "the MIDI editor did not open"); } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPianoMidiMerge(), "the piano-roll MIDI Merge button was not clickable"); },
        [controls] { return controls (false, true); }, "both MIDI Merge controls did not start off", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx, &session]
    {
        ctx.expect (session.midiMergeEnabled.load(), "the piano-roll toggle did not enable MIDI Merge");
        ctx.expect (host.clickMidiMerge(), "the transport MIDI Merge button was not clickable");
    }, [controls] { return controls (true, true); }, "the piano-roll change did not reach the transport", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPianoMidiMerge(), "the piano-roll toggle was not clickable after a transport change"); },
        [controls] { return controls (false, true); }, "the transport change did not reach the piano roll", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx, window]
    {
        host.closeRegionEditors();
        ctx.expect (host.openRegionEditor (0, 0, true), "the MIDI editor did not reopen");
        if (window.size() >= 4)
            ctx.expect (host.resizeMainWindow (std::max (window[2], 820), std::max (window[3], 680)),
                        "the MIDI editor could not use a smaller window");
    }, [controls] { return controls (true, true); }, "MIDI Merge was not enabled before reopening", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPianoMidiMerge(), "the MIDI Merge button was clipped at the smaller window size"); },
        [controls] { return controls (true, true); }, "reopening the MIDI editor lost MIDI Merge", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPianoMidiMerge(), "the smaller MIDI editor toggle was not clickable"); },
        [controls] { return controls (false, true); }, "MIDI Merge did not turn off in the smaller editor", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickRecord(), "Record was unavailable with the MIDI editor open"); },
        [controls] { return controls (true, true); }, "MIDI Merge did not turn back on", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx, &session, &engine]
    {
        ctx.expect (! host.clickPianoMidiMerge() && ! host.clickMidiMerge(), "MIDI Merge was editable during recording");
        ctx.expect (session.midiMergeEnabled.load(), "recording changed MIDI Merge");
        engine.pressStop();
    }, [controls, &engine] { return engine.getTransport().isRecording() && controls (true, false); },
        "both MIDI Merge buttons did not disable during recording", {}, 4000 });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickPianoMidiMerge(), "the MIDI editor toggle stayed disabled after Stop"); },
        [controls] { return controls (true, true); }, "MIDI Merge did not re-enable after Stop", {}, 4000 });
    steps->push_back ({ 0, [&ctx, &session]
    { ctx.expect (! session.midiMergeEnabled.load(), "the MIDI editor toggle did not turn merging off"); },
        [controls] { return controls (false, true); }, "both MIDI Merge buttons did not finish off", {}, 4000 });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoMidiMergeToggle { Scenario {
    "gui.piano_midi_merge_toggle", { "gui", "piano", "midi", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoMidiMergeToggle (host, ctx); }
} };

std::optional<ScenarioResult> runMidiMergeLoops (GuiHost& host, ScenarioContext& ctx)
{
    MidiRegion seed;
    seed.lengthInTicks = 1920;
    seed.notes = { { 1, 60, 90, 0, 1920 } };
    seed.ccs = { { 1, 7, 80, 0 } };
    MidiTakeRef earlier;
    earlier.lengthInTicks = 480;
    earlier.notes = { { 1, 59, 70, 0, 120 } };
    seed.previousTakes = { earlier };
    if (auto result = beginMidiCaptureCase (host, ctx, seed)) return result;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    const bool originalMonitor = track.inputMonitor.load();
    ctx.keep (track.inputMonitor);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the MIDI Merge track has no strip")) return ctx.verdict();
    bool hasProbe = false;
   #if DUSKSTUDIO_HAS_NATIVE_CLAP
    const auto fixture = ctx.fixture ("panic_probe.clap");
    auto& channelStrip = engine.getChannelStrip (0);
    if (fixture && ! channelStrip.getPluginSlot().isLoaded() && ! channelStrip.isNativeClapLoaded()
        && ! channelStrip.isNativeLv2Loaded() && ! channelStrip.isNativeVst3Loaded()
        && ! channelStrip.isNativeAuLoaded() && ! channelStrip.isNativeMultisampleLoaded()
        && ! channelStrip.isBuiltinLoaded())
    {
        ctx.cleanup ([&engine, strip] { engine.stop(); strip->unloadNativePlugins(); strip->refreshInsertButton(); });
        std::string error;
        hasProbe = strip->loadNativeClap (*fixture, midiprobe::kPanicProbeClapId, error);
        if (! ctx.expect (hasProbe, "the MIDI Merge instrument probe did not load: " + error)) return ctx.verdict();
        strip->refreshInsertButton();
    }
   #endif
    if (! hasProbe) ctx.note ("instrument routing assertions require an available panic probe and an empty insert");
    const auto loopLength = session.ticksToSamples (3840, engine.getCurrentSampleRate());
    transport.setLoopRange (0, loopLength);
    transport.setLoopEnabled (true);
    const auto send = [&engine] (std::uint8_t status, std::uint8_t first, std::uint8_t second = 0)
    {
        const std::uint8_t message[] { status, first, second };
        engine.postVirtualKeyboardMidi (message, (status == 0xc0 || status == 0xd0) ? 2 : 3);
    };
    const auto passReady = [&engine] (int ordinal)
    {
        engine.serviceMidiRecording();
        const auto* preview = engine.getRecordManager().midiCapturePreview (0);
        return preview != nullptr && preview->passOrdinal >= ordinal;
    };
    auto consumed = std::make_shared<MidiCaptureStamp>();
    const auto check = [&ctx, &track, seed, consumed] (int passes)
    {
        const auto capture = track.midiRegions.read()->capture;
        ctx.expect (capture.gestureId != 0 && capture.consumedThroughPass >= passes,
                    "the published events lost their consumed capture pass");
        ctx.expect (capture.gestureId >= consumed->gestureId
                    && (capture.gestureId != consumed->gestureId
                        || capture.consumedThroughPass >= consumed->consumedThroughPass),
                    "Undo or Redo made an already consumed pass eligible for replay");
        *consumed = capture;
        const auto& regions = track.midiRegions.current();
        if (! ctx.expect (regions.size() == 1, "MIDI Merge made extra regions")) return;
        const auto& region = regions.front();
        ctx.expect (region.notes.size() == (std::size_t) passes + 1, "MIDI Merge has the wrong number of pass notes");
        ctx.expect (std::find (region.notes.begin(), region.notes.end(), seed.notes.front()) != region.notes.end(),
                    "MIDI Merge changed the seed note");
        ctx.expect (region.previousTakes.size() == 1 && region.previousTakes.front().notes == seed.previousTakes.front().notes,
                    "MIDI Merge changed the take history");
        ctx.expect (region.ccs.size() == (std::size_t) passes + 1
                    && std::find (region.ccs.begin(), region.ccs.end(), seed.ccs.front()) != region.ccs.end(),
                    "MIDI Merge lost seed or recorded channel controls");
        for (int index = 0; index < passes; ++index)
        {
            const int pitch = std::array<int, 3> { 64, 67, 69 }[(std::size_t) index];
            ctx.expect (std::count_if (region.notes.begin(), region.notes.end(), [pitch] (const auto& note)
                        { return note.noteNumber == pitch && note.lengthInTicks > 0; }) == 1,
                        "MIDI Merge lost or duplicated a played note");
            const auto kind = std::array<MidiCc::Kind, 3> { MidiCc::Kind::PitchBend,
                MidiCc::Kind::ChannelPressure, MidiCc::Kind::ProgramChange }[(std::size_t) index];
            const int value = std::array<int, 3> { 12289, 91, 12 }[(std::size_t) index];
            ctx.expect (std::any_of (region.ccs.begin(), region.ccs.end(), [kind, value] (const auto& event)
                        { return event.kind == kind && event.channel == 1 && event.value == value; }),
                        "MIDI Merge changed a channel-control message");
        }
        if (passes > 0) ctx.expect (region.lengthInTicks >= 3840, "MIDI Merge did not grow the seed region");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [strip] { strip->clickMonitor(); } });
    steps->push_back ({ 700, [&track, strip]
    { if (! track.inputMonitor.load()) strip->clickMonitor(); } });
    steps->push_back ({ 200, [&ctx, &host, &session]
    {
        ctx.expect (! session.midiMergeEnabled.load(), "MIDI Merge did not start off");
        ctx.expect (host.clickMidiMerge(), "the MIDI Merge toggle is unavailable");
    } });
    steps->push_back ({ 100, [&ctx, &host, &session]
    {
        ctx.expect (session.midiMergeEnabled.load(), "the MIDI Merge toggle did not enable merging");
        ctx.expect (host.openRegionEditor (0, 0, true), "the seeded piano roll did not open");
        ctx.expect (host.clickRecord(), "Record was unavailable");
    } });
    for (int index = 0; index < 3; ++index)
    {
        const int ordinal = index + 1;
        const int pitch = std::array<int, 3> { 64, 67, 69 }[(std::size_t) index];
        if (index > 0)
            steps->push_back ({ 0, [strip] { strip->clickMonitor(); } });
        steps->push_back ({ 100, [&ctx, &engine, &transport, &host, &track, send, ordinal, pitch, index]
        {
            ctx.expect (transport.isRecording(), "loop passes stopped the transport");
            ctx.expect (track.inputMonitor.load() == (index != 1), "IN did not change for the MIDI Merge pass");
            const auto* preview = engine.getRecordManager().midiCapturePreview (0);
            ctx.expect (preview != nullptr && preview->passOrdinal == ordinal,
                        "a loop pass was missed before MIDI input");
            if (index == 0) ctx.expect (! host.clickMidiMerge(), "MIDI Merge changed during a recording gesture");
            send (0x90, (std::uint8_t) pitch, 100);
            if (index == 0) send (0xe0, 1, 96);
            if (index == 1) send (0xd0, 91);
            if (index == 2) send (0xc0, 12);
            if (index == 0) host.autosaveTick();
        }, [passReady, ordinal, &track, index]
        { return passReady (ordinal) && track.inputMonitor.load() == (index != 1); },
            "the next loop pass or its IN setting did not arrive", {}, 4000 });
        steps->push_back ({ index == 1 ? 500 : 200, [&ctx, &engine, send, pitch, index, hasProbe]
        {
           #if DUSKSTUDIO_HAS_NATIVE_CLAP
            if (index == 1 && hasProbe)
            {
                engine.suspendProcessing();
                const auto voicesHeld = midiprobe::counter (ctx, 0, "voicesHeld");
                engine.resumeProcessing();
                ctx.expect (std::abs (voicesHeld - 2.0) < 1.0e-6,
                            "IN-off Merge did not audition the live note alongside its accompaniment");
            }
           #else
            (void) ctx; (void) engine; (void) index; (void) hasProbe;
           #endif
            send (0x80, (std::uint8_t) pitch);
        } });
        steps->push_back ({ 0, [check, ordinal] { check (ordinal); },
            [passReady, ordinal] { return passReady (ordinal + 1); },
            "the completed MIDI pass did not commit while recording", {}, 4000 });
    }
    steps->push_back ({ 0, [&ctx, &host, &transport, check]
    {
        ctx.expect (transport.isRecording(), "three passes required stopping the transport");
        ctx.expect (host.focusPiano() && host.pressPeerKey (commandChord ('z'), 'z'),
                    "Undo did not reach the piano roll while recording");
        check (2);
    } });
    steps->push_back ({ 0, [&engine, &transport, &ctx, check]
    {
        engine.pressStop();
        ctx.expect (transport.isStopped(), "Stop did not finish loop recording");
        check (2);
        ctx.expect (engine.getUndoManager().redo(), "the undone third pass could not be redone after Stop");
        check (3);
        for (int remaining = 2; remaining >= 0; --remaining)
        {
            ctx.expect (engine.getUndoManager().undo(), "one completed pass did not have its own Undo");
            check (remaining);
        }
        ctx.expect (! engine.getUndoManager().canUndo(), "recording left an extra whole-gesture undo transaction");
        for (int restored = 1; restored <= 3; ++restored)
        {
            ctx.expect (engine.getUndoManager().redo(), "a pass could not be redone");
            check (restored);
        }
    } });
    steps->push_back ({ 0, [&track, strip, originalMonitor]
    { if (track.inputMonitor.load() != originalMonitor) strip->clickMonitor(); } });
    steps->push_back ({ 0, [] {}, [&track, originalMonitor]
    { return track.inputMonitor.load() == originalMonitor; }, "IN did not return to its initial state", {}, 3000 });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar midiMergeLoops { Scenario {
    "gui.midi_merge_loops", { "gui", "piano", "midi", "recording", "loop", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiMergeLoops (host, ctx); }
} };

std::optional<ScenarioResult> runMidiMergeGrowsRegion (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    MidiRegion seed;
    seed.timelineStart = ticksToSamples (480, engine.getCurrentSampleRate(), 120.0f);
    seed.lengthInTicks = 480;
    seed.notes = { { 1, 60, 90, 0, 240 } };
    seed.ccs = { { 1, 7, 80, 120 } };
    if (auto result = beginMidiCaptureCase (host, ctx, seed)) return result;
    auto& transport = engine.getTransport();
    const auto target = session.ticksToSamples (1920, engine.getCurrentSampleRate());
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickMidiMerge(), "MIDI Merge was unavailable"); } });
    steps->push_back ({ 100, [&host, &ctx, &session]
    {
        ctx.expect (session.midiMergeEnabled.load(), "MIDI Merge did not enable");
        ctx.expect (host.clickRecord(), "Record was unavailable");
    } });
    steps->push_back ({ 100, [&engine]
    {
        const std::uint8_t message[] { 0x90, 64, 100 };
        engine.postVirtualKeyboardMidi (message, 3);
    }, [&transport] { return transport.isRecording(); }, "recording did not start", {}, 3000 });
    steps->push_back ({ 250, [&engine]
    {
        const std::uint8_t message[] { 0x80, 64, 0 };
        engine.postVirtualKeyboardMidi (message, 3);
    } });
    steps->push_back ({ 0, [&engine, &session, &ctx, seed]
    {
        engine.pressStop();
        const auto& regions = session.track (0).midiRegions.current();
        if (! ctx.expect (regions.size() == 1, "recording before the region made a second region")) return;
        const auto& region = regions.front();
        ctx.expect (region.timelineStart == 0 && region.lengthInTicks >= 1920,
                    "MIDI Merge did not grow the region in both directions");
        auto shiftedNote = seed.notes.front();
        shiftedNote.startTick += 480;
        auto shiftedCc = seed.ccs.front();
        shiftedCc.atTick += 480;
        ctx.expect (region.notes.size() == 2 && std::find (region.notes.begin(), region.notes.end(), shiftedNote) != region.notes.end()
                    && region.ccs == std::vector<MidiCc> { shiftedCc }, "left growth changed the seed's absolute event positions");
        ctx.expect (engine.getUndoManager().undo(), "the partial merge pass could not be undone");
        const auto& restored = session.track (0).midiRegions.current();
        ctx.expect (restored.size() == 1 && restored.front().timelineStart == seed.timelineStart
                    && restored.front().notes == seed.notes && restored.front().ccs == seed.ccs,
                    "Undo did not restore the region before left growth");
    }, [&transport, target] { return transport.getPlayhead() >= target; }, "recording did not pass the seed region", {}, 4000 });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar midiMergeGrowsRegion { Scenario {
    "gui.midi_merge_grows_region", { "gui", "midi", "recording", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 12000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiMergeGrowsRegion (host, ctx); }
} };

std::optional<ScenarioResult> runPianoLiveRecordingPreview (GuiHost& host, ScenarioContext& ctx)
{
   #if ! defined (__linux__) || ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native virtual keyboard and X server readback");
   #else
    const int typedPitch = std::clamp (appconfig::getVkbCentreNote(), 0, 120) + 2;
    if (typedPitch < 0 || typedPitch > 127)
        return ScenarioResult::skip ("requires the typing note below MIDI 128");
    const int heldPitch = typedPitch == 64 ? 67 : 64;
    MidiRegion seed;
    seed.lengthInTicks = 7680;
    if (auto result = beginMidiCaptureCase (host, ctx, seed)) return result;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    const auto scratch = ctx.tempDir() / "preview.ppm";
    const auto length = [&engine, heldPitch]
    {
        engine.serviceMidiRecording();
        const auto* preview = engine.getRecordManager().midiCapturePreview (0);
        if (preview != nullptr)
            for (const auto& note : preview->notes)
                if (note.noteNumber == heldPitch) return note.lengthInTicks;
        return std::int64_t { 0 };
    };
    const auto unchanged = [&ctx, &session]
    {
        const auto& regions = session.track (0).midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().notes.empty(),
                    "live recording preview or virtual keyboard step recording changed the committed model");
    };
    const auto heldLength = std::make_shared<std::int64_t> (0);
    const auto releasedProgress = std::make_shared<std::int64_t> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&ctx, &host]
    {
        ctx.expect (host.openRegionEditor (0, 0, true), "the empty piano roll did not open");
        ctx.expect (host.pressPeerKey ("K", 'k'), "K did not open the piano roll's virtual keyboard");
    } });
    steps->push_back ({ 0, [&ctx, &host] { ctx.expect (host.clickRecord(), "Record was unavailable with the keyboard open"); },
        [&host] { return host.virtualKeyboardOpen(); }, "the virtual keyboard did not open", {}, 3000 });
    steps->push_back ({ 100, [&ctx, &engine, &host, heldPitch]
    {
        const std::uint8_t message[] { 0x90, (std::uint8_t) heldPitch, 100 };
        engine.postVirtualKeyboardMidi (message, 3);
        ctx.expect (host.inputVirtualKeyboard ("x"), "the typing note did not reach the virtual keyboard while recording");
    }, [&transport] { return transport.isRecording(); }, "Record did not start", {}, 3000 });
    steps->push_back ({ 0, [&host] { host.closeVirtualKeyboard(); } });
    steps->push_back ({ 300, [&ctx, &host, scratch, heldPitch, heldLength, length, unchanged]
    {
        *heldLength = length();
        ctx.expect (host.pianoRowOnScreen (heldPitch, scratch) == 1, "the held note did not appear before note-off");
        unchanged();
    }, [length] { return length() >= 120; }, "the held note did not reach the live preview", {}, 3000 });
    steps->push_back ({ 300, [&ctx, &host, scratch, heldPitch, heldLength, length, unchanged, &engine]
    {
        ctx.expect (length() >= *heldLength + 120, "the held-note preview did not grow across audio blocks");
        ctx.expect (host.pianoRowOnScreen (heldPitch, scratch) == 1, "the growing held note disappeared from the roll");
        unchanged();
        const std::uint8_t message[] { 0x80, (std::uint8_t) heldPitch, 0 };
        engine.postVirtualKeyboardMidi (message, 3);
        const std::uint8_t marker[] { 0xb0, 113, 37 };
        engine.postVirtualKeyboardMidi (marker, 3);
    }, [length, heldLength] { return length() >= *heldLength + 120; }, "the held-note length stopped growing", {}, 3000 });
    steps->push_back ({ 0, [&engine, heldLength, releasedProgress, length, unchanged]
    {
        *heldLength = length();
        *releasedProgress = engine.getRecordManager().midiCapturePreview (0)->lengthInTicks;
        unchanged();
    }, [&engine]
    {
        engine.serviceMidiRecording();
        const auto* preview = engine.getRecordManager().midiCapturePreview (0);
        return preview != nullptr && std::any_of (preview->ccs.begin(), preview->ccs.end(), [] (const auto& cc)
        {
            return cc.kind == MidiCc::Kind::ControlChange && cc.channel == 1
                && cc.controller == 113 && cc.value == 37;
        });
    }, "the note-off marker did not reach the capture preview", {}, 3000 });
    steps->push_back ({ 300, [&ctx, &host, scratch, heldPitch, heldLength, length, unchanged, &engine]
    {
        ctx.expect (length() == *heldLength, "the preview kept extending the note after note-off");
        ctx.expect (host.pianoRowOnScreen (heldPitch, scratch) == 1, "the released note vanished before Stop");
        unchanged();
        engine.pressStop();
    }, [&engine, releasedProgress]
    {
        engine.serviceMidiRecording();
        const auto* preview = engine.getRecordManager().midiCapturePreview (0);
        return preview != nullptr && preview->lengthInTicks >= *releasedProgress + 120;
    }, "the capture preview stopped advancing after note-off", {}, 3000 });
    steps->push_back ({ 300, [&ctx, &host, &session, &transport, heldPitch, typedPitch, heldLength, scratch]
    {
        ctx.expect (transport.isStopped(), "Stop did not finish MIDI recording");
        const auto& regions = session.track (0).midiRegions.current();
        if (! ctx.expect (regions.size() == 2 && regions.back().notes.size() == 2,
                          "recording committed extra virtual-keyboard step notes or lost captured notes")) return;
        ctx.expect (host.pianoRollRegion() == 1, "Stop did not follow the newly committed MIDI region");
        const auto& notes = regions.back().notes;
        ctx.expect (std::count_if (notes.begin(), notes.end(), [heldPitch, heldLength] (const auto& note)
                    { return note.noteNumber == heldPitch && note.lengthInTicks == *heldLength; }) == 1,
                    "the committed held note differs from the released preview");
        ctx.expect (std::count_if (notes.begin(), notes.end(), [typedPitch] (const auto& note)
                    { return note.noteNumber == typedPitch; }) == 1, "the typing note was duplicated by step recording");
        ctx.expect (host.pianoRowOnScreen (heldPitch, scratch) == 1,
                    "the committed roll did not retain the held-note preview");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pianoLiveRecordingPreview { Scenario {
    "gui.piano_live_recording_preview", { "gui", "piano", "midi", "recording", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoLiveRecordingPreview (host, ctx); }
} };

// A pass recorded over the region the piano roll shows lands as a new take of
// it. The roll previews the unfinished pass, shows each take after
// Stop, follows undo and redo, and Alt+T steps the takes of the region it shows.
// With a later region on the track the take lands at another index, which the
// roll follows, and a pass that makes a region of its own takes the roll with
// it. Read from the window's pixels:
// a roll that never repainted still shows the earlier take.
std::optional<ScenarioResult> runPianoRollShowsEachTake (GuiHost& host, ScenarioContext& ctx, bool laterRegion)
{
   #if ! defined (__linux__)
    (void) host;
    (void) ctx;
    (void) laterRegion;
    return ScenarioResult::skip ("reads the window back through the X server");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalPosition = transport.getPlayhead();
    const bool expanded = host.timelineViewMatches (true);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, &transport, originalDir, originalStage, originalPosition, expanded, restore]
    {
        engine.stop();
        host.closeRegionEditors();
        session.setTrackArmed (0, false);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getUndoManager().clearUndoHistory();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
        transport.setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });

    std::vector<MidiRegion> seed (1);
    seed[0].lengthInTicks = 480;
    seed[0].lengthInSamples = session.ticksToSamples (480, rate);
    if (laterRegion)
    {
        MidiRegion later;
        later.timelineStart = session.ticksToSamples (7680, rate);
        later.lengthInTicks = 1920;
        later.lengthInSamples = session.ticksToSamples (1920, rate);
        later.notes = { { 1, 72, 100, 0, 480 } };
        seed.push_back (later);
    }
    auto& track = session.track (0);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (seed));
    engine.getUndoManager().clearUndoHistory();
    host.switchToStage (GuiHost::Stage::Recording);
    if (! expanded) host.pressKey ("T", 't');
    host.pressKey ("0", '0');
    transport.setPlayhead (0);

    // The recorder appends the region it commits, so a take of the first region
    // sits after the later one.
    const int takeIdx = laterRegion ? 1 : 0;
    const auto scratch = ctx.tempDir() / "window.ppm";
    const auto expectShown = [&host, &ctx, scratch] (int pitch, std::string what)
    {
        const int shown = host.pianoRowOnScreen (pitch, scratch);
        ctx.expect (shown == 1, what + ": the roll shows " + std::to_string (shown) + " notes at pitch " + std::to_string (pitch));
        for (const int other : { 64, 67, 69, 72 })
            if (other != pitch)
            {
                const int stale = host.pianoRowOnScreen (other, scratch);
                ctx.expect (stale == 0, what + ": the roll still shows " + std::to_string (stale) + " notes at pitch "
                                            + std::to_string (other));
            }
    };
    const auto pitchAt = [&track] (int index)
    {
        const auto& regions = track.midiRegions.current();
        return index < (int) regions.size() && regions[(std::size_t) index].notes.size() == 1
             ? regions[(std::size_t) index].notes.front().noteNumber : -1;
    };
    const auto livePitch = [pitchAt, takeIdx] { return pitchAt (takeIdx); };
    const auto note = [&engine] (int pitch, bool on)
    {
        const std::uint8_t message[] { (std::uint8_t) (on ? 0x90 : 0x80), (std::uint8_t) pitch, (std::uint8_t) (on ? 100 : 0) };
        engine.postVirtualKeyboardMidi (message, 3);
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.doubleClickMidiRegion (0, 0), "double-clicking the region did not reach it"); } });
    steps->push_back ({ 400, [&host, &ctx, scratch]
    {
        ctx.expect (host.pianoRollOpen() && host.pianoRollRegion() == 0, "the piano roll did not open on the region");
        ctx.expect (host.pianoRowOnScreen (64, scratch) == 0 && host.pianoRowOnScreen (67, scratch) == 0,
                    "the empty region's roll could not be read back or showed notes");
    } });
    // A pass that is to become a take runs on until the playhead is past the end
    // of the region the roll shows, whatever the wall clock did meanwhile.
    const auto pastShownRegion = [&session, &transport, &host, rate]
    {
        const auto& regions = session.track (0).midiRegions.current();
        const int shownIdx = host.pianoRollRegion();
        if (shownIdx < 0 || shownIdx >= (int) regions.size()) return false;
        const auto& shownRegion = regions[(std::size_t) shownIdx];
        return transport.getPlayhead() >= shownRegion.timelineStart + shownRegion.lengthInSamples
                                              + static_cast<std::int64_t> (rate / 4.0);
    };
    const auto pass = [&ctx, &session, &engine, &transport, &host, steps, note, pitchAt, expectShown, scratch,
                       pastShownRegion]
                      (int pitch, int tailMs, std::string name, std::size_t earlierTakes,
                       std::int64_t from, int landsAt)
    {
        const bool becomesTake = earlierTakes > 0;
        steps->push_back ({ 100, [&ctx, &session, &engine, &transport, name, from]
        {
            transport.setPlayhead (from);
            session.setTrackArmed (0, true);
            engine.record();
            ctx.expect (transport.isRecording(), name + " did not start");
        } });
        steps->push_back ({ 300, [note, pitch] { note (pitch, true); } });
        steps->push_back ({ 300, [note, pitch] { note (pitch, false); } });
        steps->push_back ({ tailMs, [&ctx, &session, &engine, &host, scratch, pitch, name]
        {
            engine.serviceMidiRecording();
            const auto* preview = engine.getRecordManager().midiCapturePreview (0);
            ctx.expect (preview != nullptr && std::any_of (preview->notes.begin(), preview->notes.end(),
                        [pitch] (const auto& n) { return n.noteNumber == pitch; }), name + ": capture preview is missing the recorded note");
            const int shown = host.pianoRollRegion();
            const auto& regions = session.track (0).midiRegions.current();
            if (shown >= 0 && shown < (int) regions.size()
                && engine.getRecordManager().midiCaptureTimelineStart() == regions[(std::size_t) shown].timelineStart)
                ctx.expect (host.pianoRowOnScreen (pitch, scratch) == 1, name + ": the roll did not preview the recorded note");
            for (const auto& region : session.track (0).midiRegions.current())
                ctx.expect (std::none_of (region.notes.begin(), region.notes.end(), [pitch] (const auto& n)
                            { return n.noteNumber == pitch; }), name + ": the preview changed the committed notes before Stop");
            engine.pressStop();
        },
        becomesTake ? std::function<bool()> (pastShownRegion) : std::function<bool()> {},
        name + " never got past the end of the region the roll shows" });
        steps->push_back ({ 500, [&ctx, &session, &host, pitchAt, expectShown, landsAt, pitch, name, earlierTakes]
        {
            const auto& regions = session.track (0).midiRegions.current();
            ctx.expect (pitchAt (landsAt) == pitch && regions[(std::size_t) landsAt].previousTakes.size() == earlierTakes,
                        name + " did not land where it should");
            ctx.expect (host.pianoRollRegion() == landsAt, name + ": the roll is on region "
                                                               + std::to_string (host.pianoRollRegion()));
            expectShown (pitch, name + " after Stop");
        } });
    };
    pass (64, 300, "pass 1", 1, 0, takeIdx);
    pass (67, 300, "pass 2", 2, 0, takeIdx);
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('z'), 'z'), "Cmd+Z did not reach the roll"); } });
    steps->push_back ({ 400, [&ctx, livePitch, expectShown]
    {
        ctx.expect (livePitch() == 64, "undo did not take pass 2 back");
        expectShown (64, "after undo");
    } });
    steps->push_back ({ 100, [&engine] { engine.getUndoManager().redo(); } });
    steps->push_back ({ 400, [&ctx, livePitch, expectShown]
    {
        ctx.expect (livePitch() == 67, "redo did not bring pass 2 back");
        expectShown (67, "after redo");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("alt + T", 't'), "Alt+T did nothing with the roll open"); } });
    steps->push_back ({ 400, [&ctx, livePitch, expectShown]
    {
        ctx.expect (livePitch() == 64, "Alt+T did not bring the earlier take live");
        expectShown (64, "after Alt+T");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("alt + shift + T", 'T'), "Alt+Shift+T did nothing with the roll open"); } });
    steps->push_back ({ 400, [&ctx, livePitch, expectShown]
    {
        ctx.expect (livePitch() == 67, "Alt+Shift+T did not step back to the newest take");
        expectShown (67, "after Alt+Shift+T");
    } });
    // Two take steps and two passes back to the region the roll opened on.
    steps->push_back ({ 100, [&engine]
    {
        for (int i = 0; i < 4; ++i)
            engine.getUndoManager().undo();
    } });
    steps->push_back ({ 400, [&ctx, &host, &track, scratch]
    {
        const auto& regions = track.midiRegions.current();
        ctx.expect (! regions.empty() && regions.front().notes.empty() && host.pianoRollRegion() == 0,
                    "undoing both passes did not bring the roll back to the empty region: "
                        + std::to_string (regions.size()) + " regions, the roll on "
                        + std::to_string (host.pianoRollRegion()));
        for (const int pitch : { 64, 67, 72 })
            ctx.expect (host.pianoRowOnScreen (pitch, scratch) == 0,
                        "after undoing both passes the roll still shows pitch " + std::to_string (pitch));
    } });
    // A pass inside the later region covers neither region, so it lands as a
    // region of its own after both, and the roll moves to it.
    if (laterRegion)
        pass (69, 300, "a pass inside the later region", 0, seed[1].timelineStart, 2);
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pianoRollShowsEachTake { Scenario {
    "gui.piano_roll_shows_each_take", { "gui", "piano", "midi", "take", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoRollShowsEachTake (host, ctx, false); }
} };

const ScenarioRegistrar pianoRollFollowsTake { Scenario {
    "gui.piano_roll_follows_take", { "gui", "piano", "midi", "take", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoRollShowsEachTake (host, ctx, true); }
} };

// The piano roll keeps to its region when the track's regions change under it:
// a region moved and moved back stays shown, a take cycle drops the note
// selection, undoing a pass that made its own region brings the roll back to the
// region it showed before (or the nearest one left) before Cmd+Z or Alt+T
// returns, and a track left with no regions closes the roll.
std::optional<ScenarioResult> runPianoRollKeepsItsRegion (GuiHost& host, ScenarioContext& ctx)
{
   #if ! defined (__linux__)
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("reads the window back through the X server");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalPosition = transport.getPlayhead();
    const bool expanded = host.timelineViewMatches (true);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, &transport, originalDir, originalStage, originalPosition, expanded, restore]
    {
        engine.stop();
        host.closeRegionEditors();
        session.setTrackArmed (0, false);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getUndoManager().clearUndoHistory();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
        transport.setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });

    // A spans bars 1-3, B sits at bar 5, and each has an earlier take.
    MidiRegion a;
    a.lengthInTicks = 5760;
    a.lengthInSamples = session.ticksToSamples (5760, rate);
    a.notes = { { 1, 60, 100, 0, 480 } };
    MidiTakeRef earlier;
    earlier.lengthInTicks = 5760;
    earlier.notes = { { 1, 61, 100, 0, 480 } };
    a.previousTakes = { earlier };
    MidiRegion b;
    b.timelineStart = session.ticksToSamples (7680, rate);
    b.lengthInTicks = 1920;
    b.lengthInSamples = session.ticksToSamples (1920, rate);
    b.notes = { { 1, 62, 100, 0, 480 } };
    MidiTakeRef earlierOfB;
    earlierOfB.lengthInTicks = 1920;
    earlierOfB.notes = { { 1, 63, 100, 0, 480 } };
    b.previousTakes = { earlierOfB };
    auto& track = session.track (0);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { a, b }));
    engine.getUndoManager().clearUndoHistory();
    host.switchToStage (GuiHost::Stage::Recording);
    if (! expanded) host.pressKey ("T", 't');
    host.pressKey ("0", '0');

    const auto scratch = ctx.tempDir() / "window.ppm";
    const auto expectShown = [&host, &ctx, scratch] (int pitch, std::string what)
    {
        for (const int row : { 60, 61, 62, 63, 64 })
        {
            const int shown = host.pianoRowOnScreen (row, scratch);
            ctx.expect (shown == (row == pitch ? 1 : 0), what + ": the roll shows " + std::to_string (shown)
                                                             + " notes at pitch " + std::to_string (row));
        }
    };
    const auto livePitch = [&track] (std::size_t index)
    {
        const auto& regions = track.midiRegions.current();
        return index < regions.size() && regions[index].notes.size() == 1 ? regions[index].notes.front().noteNumber : -1;
    };
    const auto barTen = session.ticksToSamples (17280, rate);

    auto steps = std::make_shared<std::vector<Step>>();
    // Moved the way a timeline drag moves it: one edit in place at its index.
    steps->push_back ({ 300, [&ctx, &session, &engine, &track, b, rate]
    {
        MidiRegion moved = b;
        moved.timelineStart = session.ticksToSamples (1920, rate);
        engine.getUndoManager().beginNewTransaction ("Move region");
        ctx.expect (engine.getUndoManager().perform (new MidiRegionEditAction (session, engine, 0, 1, b, moved)),
                    "the region move did not apply");
        ctx.expect (track.midiRegions.current().size() == 2, "the region move changed the region count");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 1, true), "the piano roll did not open on the moved region"); } });
    steps->push_back ({ 400, [expectShown] { expectShown (62, "the moved region"); } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey (commandChord ('z'), 'z'), "Cmd+Z did not reach the roll"); } });
    steps->push_back ({ 400, [&ctx, &host, &track, b, expectShown]
    {
        const auto& regions = track.midiRegions.current();
        ctx.expect (regions.size() == 2 && regions[1].timelineStart == b.timelineStart, "undo did not move the region back");
        ctx.expect (host.pianoRollRegion() == 1, "undoing the move took the roll to region "
                                                     + std::to_string (host.pianoRollRegion()));
        expectShown (62, "after undoing the move");
    } });

    // Closed first: opening the region the roll already shows would close it.
    steps->push_back ({ 200, [&host, &ctx]
    {
        host.closePianoRoll();
        ctx.expect (host.openRegionEditor (0, 0, true), "the piano roll did not open on the first region");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey (commandChord ('a'), 'a') && ! host.pianoSelection().empty(),
                    "Cmd+A did not select the region's note");
        ctx.expect (host.pressPeerKey ("alt + T", 't'), "Alt+T did nothing with the roll open");
        ctx.expect (host.pianoSelection().empty(), "the selection outlived the take it was made on");
    } });
    steps->push_back ({ 400, [&ctx, livePitch, expectShown]
    {
        ctx.expect (livePitch (0) == 61, "Alt+T did not bring the earlier take live");
        expectShown (61, "after Alt+T");
    } });

    // A pass at bar 10 overlaps nothing, so it makes a region of its own.
    steps->push_back ({ 100, [&ctx, &session, &engine, &transport, barTen]
    {
        transport.setPlayhead (barTen);
        session.setTrackArmed (0, true);
        engine.record();
        ctx.expect (transport.isRecording(), "the pass did not start");
    } });
    steps->push_back ({ 300, [&engine]
    {
        const std::uint8_t on[] { 0x90, 64, 100 };
        engine.postVirtualKeyboardMidi (on, 3);
    } });
    steps->push_back ({ 300, [&engine]
    {
        const std::uint8_t off[] { 0x80, 64, 0 };
        engine.postVirtualKeyboardMidi (off, 3);
    } });
    steps->push_back ({ 300, [&engine] { engine.pressStop(); } });
    steps->push_back ({ 500, [&ctx, &host, &track, expectShown]
    {
        ctx.expect (track.midiRegions.current().size() == 3 && host.pianoRollRegion() == 2,
                    "the pass did not land as a region of its own with the roll on it");
        expectShown (64, "after the pass");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey (commandChord ('z'), 'z'), "Cmd+Z did not reach the roll");
        ctx.expect (host.pianoRollRegion() == 0, "right after undoing the pass the roll is on region "
                                                     + std::to_string (host.pianoRollRegion()));
    } });
    steps->push_back ({ 400, [&ctx, &host, expectShown]
    {
        ctx.expect (host.pianoRollRegion() == 0, "undoing the pass left the roll on region "
                                                     + std::to_string (host.pianoRollRegion()));
        expectShown (61, "after undoing the pass");
    } });

    // An undo the window did not run, with the roll opened on the pass's region:
    // it goes to the nearest region left, and Alt+T at once steps that one.
    steps->push_back ({ 100, [&engine] { engine.getUndoManager().redo(); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        host.closePianoRoll();
        ctx.expect (host.openRegionEditor (0, 2, true), "the piano roll did not open on the pass's region");
    } });
    steps->push_back ({ 300, [&host, &ctx, &engine, livePitch]
    {
        engine.getUndoManager().undo();
        ctx.expect (host.pressPeerKey ("alt + T", 't'), "Alt+T right after an undo did nothing");
        ctx.expect (host.pianoRollRegion() == 1 && livePitch (1) == 63,
                    "Alt+T right after an undo did not step the region the roll went to");
    } });
    steps->push_back ({ 400, [expectShown] { expectShown (63, "after Alt+T following an undo"); } });

    steps->push_back ({ 100, [&track] { track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>()); } });
    steps->push_back ({ 500, [&ctx, &host]
    { ctx.expect (! host.pianoRollOpen(), "the roll stayed open on a track with no regions"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pianoRollKeepsItsRegion { Scenario {
    "gui.piano_roll_keeps_its_region", { "gui", "piano", "midi", "take", "recording", "undo" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoRollKeepsItsRegion (host, ctx); }
} };

// While the transport records, the virtual keyboard's notes go to the take only:
// step record stays out, so the region the roll shows is left as it was and
// becomes the previous take unchanged.
std::optional<ScenarioResult> runPianoStepRecordWhileRecording (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native virtual keyboard");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    const int centre = appconfig::getVkbCentreNote();
    if (! transport.isStopped() || host.virtualKeyboardOpen() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport, no modal and a closed virtual keyboard");
    if (centre < 0 || centre > 115) return ScenarioResult::skip ("requires the typing notes below MIDI 128");
    const auto rate = engine.getCurrentSampleRate();
    if (rate <= 0.0) return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalPosition = transport.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, &transport, originalDir, originalStage, originalPosition, restore]
    {
        engine.stop();
        host.closeVirtualKeyboard();
        host.closeRegionEditors();
        session.setTrackArmed (0, false);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getUndoManager().clearUndoHistory();
        transport.setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });

    const std::vector<MidiNote> earlier { { 1, 64, 100, 0, 240 } };
    MidiRegion region;
    region.lengthInTicks = 480;
    region.lengthInSamples = session.ticksToSamples (480, rate);
    region.notes = earlier;
    auto& track = session.track (0);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    host.switchToStage (GuiHost::Stage::Recording);
    transport.setPlayhead (0);

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.openRegionEditor (0, 0, true), "piano roll did not open"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("K", 'k'), "K did not reach the piano roll"); } });
    steps->push_back ({ 300, [&host, &ctx, &session, &engine, &transport]
    {
        ctx.expect (host.virtualKeyboardOpen(), "virtual keyboard did not open above the piano roll");
        session.setTrackArmed (0, true);
        engine.record();
        ctx.expect (transport.isRecording(), "the take did not start");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.inputVirtualKeyboard ("q"), "the note key did not reach the native keyboard"); } });
    steps->push_back ({ 400, [&ctx, &track, earlier]
    {
        const auto& regions = track.midiRegions.current();
        ctx.expect (regions.size() == 1 && regions.front().notes == earlier,
                    "a virtual-keyboard note was step-entered into the region during the take");
    } });
    steps->push_back ({ 700, [&engine] { engine.pressStop(); } });
    steps->push_back ({ 500, [&ctx, &track, earlier, centre]
    {
        const auto& regions = track.midiRegions.current();
        const bool oneTake = regions.size() == 1 && regions.front().notes.size() == 1
                          && regions.front().notes.front().noteNumber == centre + 12;
        ctx.expect (oneTake, "the take does not hold just the played note");
        ctx.expect (regions.size() == 1 && regions.front().previousTakes.size() == 1
                        && regions.front().previousTakes.front().notes == earlier,
                    "the previous take was changed by the notes of the take after it");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("K", 'k'), "K did not reach the piano roll a second time"); } });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.virtualKeyboardOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "K did not close the virtual keyboard");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pianoStepRecordWhileRecording { Scenario {
    "gui.piano_step_record_while_recording", { "gui", "piano", "midi", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoStepRecordWhileRecording (host, ctx); }
} };

std::optional<ScenarioResult> beginNativePianoCase (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native piano roll");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    if (! transport.isStopped() || ! host.modalStackEmpty() || host.virtualKeyboardOpen()
        || host.pianoRollOpen() || host.audioEditorOpen())
        return ScenarioResult::skip ("requires stopped transport and no open panels");
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &engine, &session, &transport, &track,
                  regions = track.midiRegions.current(), stage = engine.getStage(),
                  editMode = session.editMode, snap = session.midiEditorSnap,
                  position = transport.getPlayhead(), loop = transport.isLoopEnabled(),
                  loopIn = transport.getLoopStart(), loopOut = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled(), punchIn = transport.getPunchIn(), punchOut = transport.getPunchOut()]
    {
        engine.stop();
        host.closeVirtualKeyboard();
        host.closeRegionEditors();
        drainModals (host);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        engine.getUndoManager().clearUndoHistory();
        session.editMode = editMode;
        session.midiEditorSnap = snap;
        transport.setPlayhead (position);
        transport.setLoopRange (loopIn, loopOut);
        transport.setLoopEnabled (loop);
        transport.setPunchRange (punchIn, punchOut);
        transport.setPunchEnabled (punch);
        host.switchToStage (guiStage (stage));
    });
    const auto rate = engine.getTimelineSampleRate();
    MidiRegion first;
    first.lengthInTicks = 7680;
    first.lengthInSamples = session.ticksToSamples (first.lengthInTicks, rate);
    first.notes = { { 1, 60, 100, 960, 480 }, { 2, 64, 90, 2400, 480 } };
    first.ccs = { { 1, 1, 32, 240 }, { 1, 1, 96, 960 } };
    MidiTakeRef earlier;
    earlier.lengthInTicks = first.lengthInTicks;
    earlier.notes = first.notes;
    for (auto& note : earlier.notes) note.noteNumber += 5;
    earlier.ccs = first.ccs;
    first.previousTakes.push_back (earlier);
    MidiRegion second;
    second.timelineStart = session.ticksToSamples (9600, rate);
    second.lengthInTicks = first.lengthInTicks;
    second.lengthInSamples = first.lengthInSamples;
    second.notes = { { 1, 67, 100, 960, 480 } };
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (
        std::vector<MidiRegion> { first, second }));
    engine.getUndoManager().clearUndoHistory();
    session.midiEditorSnap = true;
    session.editMode = EditMode::Grab;
    transport.setPlayhead (session.ticksToSamples (480, rate));
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    host.switchToStage (GuiHost::Stage::Recording);
    host.openPianoRoll (0, 0);
    if (! host.pianoNativeWindowOpen()) return ScenarioResult::fail ("the native piano roll did not open");
    return std::nullopt;
   #endif
}

std::optional<ScenarioResult> runPianoNativeShortcuts (GuiHost& host, ScenarioContext& ctx)
{
    if (auto initial = beginNativePianoCase (host, ctx)) return initial;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    auto& transport = engine.getTransport();
    const auto original = track.midiRegions.current().front().notes;
   #if DUSKSTUDIO_HAS_NATIVE_UI
    const std::array<std::pair<ImGuiKey, const char*>, 9> expectedNames { {
             { ImGuiKey_Enter, "return" }, { ImGuiKey_KeypadEnter, "return" },
             { ImGuiKey_PageUp, "page up" }, { ImGuiKey_PageDown, "page down" },
             { ImGuiKey_Comma, "," }, { ImGuiKey_GraveAccent, "`" }, { ImGuiKey_Backslash, "\\" },
             { ImGuiKey_KeypadAdd, "numpad +" }, { ImGuiKey_KeypadSubtract, "numpad -" } } };
    for (const auto& expected : expectedNames)
        ctx.expect (imgui::shellKeyName (expected.first) == expected.second,
                    "the native shell bridge did not preserve " + std::string (expected.second));
    for (const auto key : { ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, ImGuiKey_LeftAlt, ImGuiKey_RightAlt,
                           ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_LeftSuper, ImGuiKey_RightSuper,
                           ImGuiKey_CapsLock, ImGuiKey_NumLock, ImGuiKey_ScrollLock, ImGuiKey_None })
        ctx.expect (imgui::shellKeyName (key).empty(), "a modifier or unmapped key acquired a shell name");
   #endif
    const bool expanded = (host.tapeExpansionState() & 2) != 0;
    std::array<bool, Session::kNumTracks> armed {};
    for (int t = 0; t < Session::kNumTracks; ++t)
        armed[(std::size_t) t] = ctx.session().track (t).recordArmed.load();
    ctx.cleanup ([&host, &session = ctx.session(), armed, expanded]
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
            session.track (t).recordArmed.store (armed[(std::size_t) t]);
        session.recomputeRtCounters();
        host.setTimelineShown (expanded);
    });
    const auto pitch = [&track]
    {
        const auto& regions = track.midiRegions.current();
        return ! regions.empty() && ! regions.front().notes.empty() ? regions.front().notes.front().noteNumber : -1;
    };
    const auto position = transport.getPlayhead();
    const int centre = std::clamp (appconfig::getVkbCentreNote(), 0, 120);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.focusPiano(), "the native piano child could not receive focus"); },
    [&host] { return host.pianoViewport()[3] > 0; }, "the native piano roll did not draw" });
    for (const auto* key : { "return", "numpad enter", "page up", "page down", ",", "`",
                            "left alt", "right alt", "left ctrl", "right ctrl", "left shift", "right shift",
                            "left super", "right super", "caps lock", "num lock", "scroll lock" })
    {
        steps->push_back ({ 0, [&host, &ctx, key]
        { ctx.expect (host.inputPianoRoll (key), "native input failed for " + std::string (key)); } });
        steps->push_back ({ 30, [&host, &ctx, &engine, &track, &transport, original, armed, expanded, key]
        {
            const auto name = std::string (key);
            ctx.expect (transport.isStopped() && ! engine.getRecordManager().isActive(), name + " started recording");
            ctx.expect (! transport.isPunchEnabled(), name + " toggled punch");
            const auto& regions = track.midiRegions.current();
            ctx.expect (regions.size() == 2 && regions.front().notes == original, name + " changed the MIDI take");
            for (int t = 0; t < Session::kNumTracks; ++t)
                ctx.expect (ctx.session().track (t).recordArmed.load() == armed[(std::size_t) t], name + " changed ARM");
            ctx.expect (host.timelineViewMatches (expanded), name + " toggled the timeline");
            ctx.expect (! host.virtualKeyboardOpen(), name + " opened the virtual keyboard");
            ctx.expect (host.modalStackEmpty(), name + " opened a modal");
            ctx.expect (host.pianoNativeWindowOpen(), name + " closed the piano roll");
        } });
    }
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.inputPianoRoll (commandChord ('\\')), "the native timeline alias was not delivered"); } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.inputPianoRoll (commandChord ('\\')), "the native timeline alias could not restore the view"); },
    [&host, expanded] { return host.timelineViewMatches (! expanded); }, "the native timeline alias did not toggle the view" });
    steps->push_back ({ 0, [] {}, [&host, expanded] { return host.timelineViewMatches (expanded); },
                       "the native timeline alias did not restore the view" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.focusPiano(), "the native piano child could not receive focus");
        ctx.expect (host.inputPianoRoll (commandChord ('A')), "Select All did not reach the native child");
    }, [&host] { return host.pianoViewport()[3] > 0; }, "the native piano roll did not draw" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoSelection() == std::vector<int> { 0, 1 }, "native Select All missed notes");
        ctx.expect (host.inputPianoRoll ("alt + T"), "Alt+T did not reach the native child");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoRollRegion() == 0 && host.pianoSelection().empty(), "take cycling lost the region or kept its selection");
        ctx.expect (host.inputPianoRoll (commandChord ('Z')), "native Undo was unavailable");
    }, [pitch] { return pitch() == 65; }, "native Alt+T did not advance exactly one take" });
    steps->push_back ({ 0, [&host, &ctx, &engine]
    {
        ctx.expect (! engine.getUndoManager().canUndo(), "native take cycling created more than one undo step");
        ctx.expect (host.inputPianoRoll (commandChord ('Y')), "native Redo was unavailable");
    }, [pitch] { return pitch() == 60; }, "native Undo did not restore the original take" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.inputPianoRoll ("alt + shift + T"), "Alt+Shift+T did not reach the native child"); },
    [pitch] { return pitch() == 65; }, "native Redo did not restore the cycled take" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.inputPianoRoll ("home"), "Home did not reach the native child"); },
    [pitch] { return pitch() == 60; }, "native Alt+Shift+T did not return to the original take" });
    steps->push_back ({ 0, [&host, &ctx, &transport, position]
    {
        ctx.expect (transport.getPlayhead() == position, "Home escaped the roll and moved the transport");
        ctx.expect (host.inputPianoRoll ("."), "the stop-and-rewind key did not reach the native child");
    } });
    steps->push_back ({ 0, [&host, &ctx, &transport, position]
    {
        transport.setPlayhead (position);
        ctx.expect (host.inputPianoRoll ("K"), "K did not reach the native child");
    }, [&transport] { return transport.getPlayhead() == 0; }, "the native child did not forward stop-and-rewind" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoNativeWindowOpen() && host.pianoRollOpen(), "K closed the roll instead of covering it");
        ctx.expect (host.inputPianoRoll (commandChord ('Z')), "the covered piano child did not receive the keyboard note");
    }, [&host] { return host.virtualKeyboardOpen(); }, "native K did not open the virtual keyboard" });
    steps->push_back ({ 0, [&host, &ctx, &track, original, centre]
    {
        const auto& regions = track.midiRegions.current();
        if (ctx.expect (regions.size() == 2, "the covering keyboard changed the region count"))
        {
            const auto& notes = regions.front().notes;
            if (ctx.expect (notes.size() == original.size() + 1, "the covering keyboard did not keep Cmd/Ctrl+Z as one note"))
            {
                ctx.expect (std::equal (original.begin(), original.end(), notes.begin()), "Cmd/Ctrl+Z undid the take behind the keyboard");
                ctx.expect (notes.back().noteNumber == centre && notes.back().startTick == 480,
                            "the keyboard note has the wrong pitch or step position");
            }
        }
        ctx.expect (host.inputVirtualKeyboard ("K"), "the native keyboard did not receive its close key");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoNativeWindowOpen(), "closing the keyboard destroyed the piano child");
        ctx.expect (host.inputPianoRoll (commandChord ('A')), "the uncovered piano child did not receive Select All");
    }, [&host] { return ! host.virtualKeyboardOpen(); }, "the keyboard did not finish closing" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoSelection() == std::vector<int> { 0, 1, 2 }, "the uncovered piano child did not select its notes");
        ctx.expect (host.inputPianoRoll ("cursor up"), "the uncovered piano child did not receive Up");
    } });
    steps->push_back ({ 0, [&ctx, &track, original, centre]
    {
        auto expected = original;
        for (auto& note : expected) ++note.noteNumber;
        expected.push_back ({ 1, centre + 1, 100, 480, 120 });
        const auto& regions = track.midiRegions.current();
        ctx.expect (regions.size() == 2 && regions.front().notes == expected,
                    "native Up did not transpose every selected note exactly once");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoNativeShortcuts { Scenario {
    "gui.piano_native_shortcuts", { "gui", "piano", "midi", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 45000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoNativeShortcuts (host, ctx); }
} };

std::optional<ScenarioResult> runPianoNativeEscapeStack (GuiHost& host, ScenarioContext& ctx)
{
    if (auto initial = beginNativePianoCase (host, ctx)) return initial;
    auto& track = ctx.session().track (0);
    auto& undo = ctx.engine().getUndoManager();
    const auto original = track.midiRegions.current().front();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.focusPiano(), "the piano child could not receive focus");
        ctx.expect (host.inputPianoRoll (commandChord ('A')), "native Select All was unavailable");
    }, [&host] { return host.pianoViewport()[3] > 0; }, "the native piano roll did not draw" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoSelection() == std::vector<int> { 0, 1 }, "native Select All missed notes");
        ctx.expect (host.inputPianoRoll ("V"), "the native velocity key was unavailable");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoKeyboardFocus (false), "the child could not yield keyboard focus");
        ctx.expect (host.pressKey ("escape"), "parent Escape was not replayed to the popup");
    }, [&host] { return ! host.contextMenuItems().empty(); }, "the velocity popup did not open" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoNativeWindowOpen() && host.pianoSelection() == std::vector<int> { 0, 1 },
                    "parent Escape dismissed the roll or cleared selection behind its popup");
        ctx.expect (host.pianoKeyboardFocus (true) && host.inputPianoRoll ("V"), "the native velocity popup could not reopen");
    }, [&host] { return host.modalStackEmpty(); }, "parent Escape did not close the velocity popup" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.inputPianoRoll ("escape"), "native Escape was unavailable in the popup"); },
    [&host] { return ! host.contextMenuItems().empty(); }, "the native velocity popup did not reopen" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoNativeWindowOpen() && host.pianoSelection() == std::vector<int> { 0, 1 },
                    "native Escape dismissed the roll or cleared selection behind its popup");
        ctx.expect (host.inputPianoRoll ("escape"), "native selection Escape was unavailable");
    }, [&host] { return host.modalStackEmpty(); }, "native Escape did not close the velocity popup" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoNativeWindowOpen() && host.pianoSelection().empty(), "selection Escape closed the roll or left selected notes");
        ctx.expect (host.inputPianoRoll ("escape"), "native close Escape was unavailable");
    } });
    steps->push_back ({ 0, [&host, &ctx, &track, &undo, original]
    {
        ctx.expect (! host.pianoRollOpen() && host.canvasHasKeyboardFocus(), "closing the child did not restore canvas focus");
        const auto& regions = track.midiRegions.current();
        ctx.expect (regions.size() == 2 && regions.front().notes == original.notes
                    && regions.front().ccs == original.ccs, "the Escape stack changed MIDI events");
        ctx.expect (! undo.canUndo() && ! undo.canRedo(), "the Escape stack created undo history");
        host.openPianoRoll (0, 0);
    }, [&host] { return ! host.pianoNativeWindowOpen(); }, "the piano child did not finish closing" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoRollRegion() == 0 && host.pianoSelection().empty(), "the reopened child retained a stale target or selection");
        ctx.expect (host.pianoKeyboardFocus (true) && host.inputPianoRoll ("escape"), "the reopened child did not receive Escape");
    }, [&host] { return host.pianoNativeWindowOpen() && host.pianoViewport()[3] > 0; }, "the piano child did not reopen" });
    steps->push_back ({ 0, [] {}, [&host] { return ! host.pianoNativeWindowOpen(); }, "the reopened child did not finish closing" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoNativeEscapeStack { Scenario {
    "gui.piano_native_escape_stack", { "gui", "piano", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoNativeEscapeStack (host, ctx); }
} };

std::optional<ScenarioResult> runPianoNativeNavigationReopen (GuiHost& host, ScenarioContext& ctx)
{
    if (auto initial = beginNativePianoCase (host, ctx)) return initial;
    auto& track = ctx.session().track (0);
    const auto original = track.midiRegions.current();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.focusPiano(), "the piano child could not receive focus");
        ctx.expect (host.inputPianoRoll ("command + ]"), "native next-region input was unavailable");
    }, [&host] { return host.pianoViewport()[3] > 0; }, "the native piano roll did not draw" });
    steps->push_back ({ 150, [&host, &ctx]
    {
       #if defined (__linux__)
        ctx.expect (host.pianoRowOnScreen (67, ctx.tempDir() / "second-67.ppm") == 1
                    && host.pianoRowOnScreen (60, ctx.tempDir() / "second-60.ppm") == 0,
                    "the replacement child did not display the second region's notes");
       #endif
        ctx.expect (host.inputPianoRoll ("command + ["), "native previous-region input was unavailable");
    }, [&host] { return host.pianoRollRegion() == 1 && host.pianoViewport()[3] > 0; }, "native next-region did not replace the child" });
    steps->push_back ({ 150, [&host, &ctx]
    {
       #if defined (__linux__)
        ctx.expect (host.pianoRowOnScreen (60, ctx.tempDir() / "first-60.ppm") == 1
                    && host.pianoRowOnScreen (67, ctx.tempDir() / "first-67.ppm") == 0,
                    "the replacement child did not display the first region's notes");
       #endif
        // The old child's queued key must die with it, before it has drawn a frame.
        ctx.expect (host.inputPianoRoll ("command + ]"), "the old child did not queue navigation");
        host.closePianoRoll();
        host.openPianoRoll (0, 0);
        ctx.expect (host.pianoKeyboardFocus (true) && host.inputPianoRoll (commandChord ('A')),
                    "the replacement child did not queue Select All");
    }, [&host] { return host.pianoRollRegion() == 0 && host.pianoViewport()[3] > 0; }, "native previous-region did not replace the child" });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.pianoNativeWindowOpen() && host.pianoRollRegion() == 0,
                    "a discarded child's queued navigation changed its replacement");
        ctx.expect (host.pianoSelection() == std::vector<int> { 0, 1 }, "the replacement child did not draw its own input");
        ctx.expect (host.inputPianoRoll ("cursor up"), "the replacement child did not receive Up");
    } });
    steps->push_back ({ 0, [&ctx, &track, original]
    {
        const auto& regions = track.midiRegions.current();
        if (! ctx.expect (regions.size() == original.size(), "native navigation changed the region count")) return;
        auto expected = original.front().notes;
        for (auto& note : expected) ++note.noteNumber;
        ctx.expect (regions.front().notes == expected && regions.back().notes == original.back().notes,
                    "a native edit after replacement changed the wrong region");
        ctx.expect (regions.front().ccs == original.front().ccs && regions.back().ccs == original.back().ccs,
                    "native navigation changed controllers");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoNativeNavigationReopen { Scenario {
    "gui.piano_native_navigation_reopen", { "gui", "piano", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoNativeNavigationReopen (host, ctx); }
} };

std::optional<ScenarioResult> runPianoRollNoteKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    auto& undo = ctx.engine().getUndoManager();
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &track, &undo, regions = track.midiRegions.current()]
    {
        host.closePianoRoll();
        undo.clearUndoHistory();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion region;
    region.lengthInTicks = 1920;
    region.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 2.0);
    region.notes = { { 1, 60, 101, 480, 120 }, { 3, 67, 85, 960, 240 } };
    const auto original = region.notes;
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    undo.clearUndoHistory();
    host.openPianoRoll (0, 0);
    ctx.later (300, [&host, &ctx, &track, original]
    {
        ctx.expect (host.pressPianoRollKey ("command + A"), "the piano roll did not select all notes");
        ctx.expect (host.pressPianoRollKey ("5"), "the piano roll did not select the sixteenth-note grid");
        // Shift chords arrive as the arrow's key code with the shift modifier and
        // no text, which is what pressPianoRollKey builds from the description.
        struct ArrowAction { const char* key; int semitones; std::int64_t ticks; };
        for (const auto& action : std::array<ArrowAction, 8> {
                 ArrowAction { "cursor up", 1, 0 }, { "cursor down", -1, 0 },
                 { "cursor right", 0, 120 }, { "cursor left", 0, -120 },
                 { "shift + cursor up", 12, 0 }, { "shift + cursor down", -12, 0 },
                 { "shift + cursor right", 0, kMidiTicksPerQuarter },
                 { "shift + cursor left", 0, -kMidiTicksPerQuarter } })
        {
            ctx.expect (host.pressPianoRollKey ("command + A"), "the piano roll did not reselect notes after undo");
            ctx.expect (host.pressPianoRollKey (action.key), std::string (action.key) + " was not handled");
            auto expected = original;
            for (auto& note : expected)
            {
                note.noteNumber += action.semitones;
                note.startTick  += action.ticks;
            }
            const auto& edited = track.midiRegions.current();
            ctx.expect (edited.size() == 1 && edited[0].notes == expected,
                        std::string (action.key) + " changed the wrong note fields or amount");
            ctx.expect (host.pressPianoRollKey ("command + Z"), "the piano roll did not handle undo");
            const auto& restored = track.midiRegions.current();
            ctx.expect (restored.size() == 1 && restored[0].notes == original,
                        "undo did not restore both notes exactly");
        }
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

const ScenarioRegistrar pianoRollNoteKeys { Scenario {
    "gui.piano_roll_note_keys", { "gui", "midi", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoRollNoteKeys (host, ctx); }
} };

std::optional<ScenarioResult> runMidiActivityLed (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the MIDI activity fixture has no strip")) return ctx.verdict();
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiChannel);
    ctx.cleanup ([&host, strip, mode = track.mode.load()]
    {
        host.closeTopModal();
        strip->restoreTrackMode (mode);
    });
    host.switchToStage (GuiHost::Stage::Recording);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiChannel.store (1);
    if (! ctx.expect (strip->openInputSettings ((int) Track::Mode::Midi), "could not open MIDI input settings"))
        return ctx.verdict();
    auto seen = std::make_shared<bool> (false);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [strip, &ctx]
    { ctx.expect (strip->midiActivityVisible(), "the MIDI input activity LED is not visible"); } });
    for (const bool matching : { false, true })
    {
        steps->push_back ({ 100, [seen] { *seen = false; } });
        for (int event = 0; event < 30; ++event)
            steps->push_back ({ 10, [&engine, strip, seen, matching]
            {
                *seen = *seen || strip->midiActivityLit();
                const std::uint8_t cc[] { static_cast<std::uint8_t> (matching ? 0xb0 : 0xb1), 1, 64 };
                engine.postVirtualKeyboardMidi (cc, 3);
            } });
        steps->push_back ({ 10, [strip, seen, matching, &ctx]
        {
            *seen = *seen || strip->midiActivityLit();
            ctx.expect (*seen == matching, matching ? "matching MIDI traffic never lit the activity LED"
                                                    : "the activity LED accepted a filtered MIDI channel");
        } });
        steps->push_back ({ 100, [strip, &ctx]
        { ctx.expect (! strip->midiActivityLit(), "the activity LED stayed lit after traffic stopped"); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar midiActivityLed { Scenario {
    "gui.midi_activity_led", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiActivityLed (host, ctx); }
} };

std::optional<ScenarioResult> runPianoRollNavigation (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    const bool expanded = host.timelineViewMatches (true);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &track, expanded, regions = track.midiRegions.current()]
    {
        host.closePianoRoll();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion first;
    first.lengthInTicks = 1920;
    first.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 2.0);
    first.notes = { { 1, 60, 100, 240, 120 } };
    auto second = first;
    second.timelineStart = first.lengthInSamples * 2;
    second.notes[0].noteNumber = 67;
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { first, second }));
    if (! expanded) host.pressKey ("T", 't');
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.doubleClickMidiRegion (0, 0), "the first MIDI region was not visible for double-click"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pianoRollOpen() && host.pianoRollRegion() == 0, "double-click did not open the first MIDI region");
        ctx.expect (host.pressPeerKey ("command + " + keyCodeDescription (']'), (char) 0x1d),
                    "the piano roll did not handle next region");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pianoRollRegion() == 1, "next region did not select the second MIDI region");
        ctx.expect (host.pressPeerKey ("command + " + keyCodeDescription ('['), (char) 0x1b),
                    "the piano roll did not handle previous region");
    } });
    steps->push_back ({ 300, [&host, &ctx, &track, first, second]
    {
        ctx.expect (host.pianoRollRegion() == 0, "previous region did not return to the first MIDI region");
        const auto& regions = track.midiRegions.current();
        ctx.expect (regions.size() == 2 && regions[0].notes == first.notes && regions[1].notes == second.notes,
                    "navigation changed MIDI notes");
        ctx.expect (host.pressPianoRollKey ("escape"), "the piano roll did not handle Escape");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (! host.pianoRollOpen(), "Escape did not close the piano roll"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoRollNavigation { Scenario {
    "gui.piano_roll_navigation", { "gui", "midi", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoRollNavigation (host, ctx); }
} };

// With no audio device running the piano roll measures time at the rate the
// device last ran at, so [ and ] put the loop, and Shift with them the punch,
// on the timeline sample under the edit cursor, not at a sample worked out at 1 Hz.
std::optional<ScenarioResult> runPianoLoopKeysWithoutDevice (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    const double rate = engine.getCurrentSampleRate();
    if (host.engineDetached() || ! engine.isAudioCallbackRegistered() || rate <= 0.0)
        return ScenarioResult::skip ("requires a running audio device");
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    auto& track = session.track (0);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &engine, &track, &transport, regions = track.midiRegions.current(),
                  loop = transport.isLoopEnabled(), loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled(), punchIn = transport.getPunchIn(), punchOut = transport.getPunchOut()]
    {
        host.closePiano();
        if (! engine.isAudioCallbackRegistered()) engine.reattachAudioCallback();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (loop);
        transport.setPunchRange (punchIn, punchOut);
        transport.setPunchEnabled (punch);
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion region;
    region.timelineStart = (std::int64_t) std::llround (rate * 2.0);
    region.lengthInTicks = 7680;
    region.lengthInSamples = session.ticksToSamples (session.samplesToTicks (region.timelineStart, rate) + region.lengthInTicks, rate)
                           - region.timelineStart;
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    engine.detachAudioCallback();
    if (engine.getCurrentSampleRate() > 0.0)
        return ScenarioResult::fail ("a detached engine still reads a running rate");
    if (! host.openPiano (0, 0)) return ScenarioResult::fail ("piano roll did not open");

    const auto sampleAt = [&session, rate, start = region.timelineStart] (std::int64_t tick)
    { return session.ticksToSamples (session.samplesToTicks (start, rate) + tick, rate); };
    const auto click = [&host, &ctx] (std::int64_t tick)
    { ctx.expect (host.clickPianoGrid (tick, 72), "the piano grid did not take a click at tick " + std::to_string (tick)); };
    const auto press = [&host, &ctx] (std::int64_t tick, const std::string& key)
    {
        ctx.expect (host.pianoEditCursor() == tick, "the click left the edit cursor at tick "
                                                        + std::to_string (host.pianoEditCursor()) + ", not "
                                                        + std::to_string (tick));
        ctx.expect (host.pressPianoRollKey (key), "the piano roll did not handle " + key);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [click] { click (1920); } });
    steps->push_back ({ 150, [press] { press (1920, "["); } });
    steps->push_back ({ 150, [&ctx, &transport, sampleAt, click]
    {
        ctx.expect (transport.getLoopStart() == sampleAt (1920),
                    "[ put the loop start at sample " + std::to_string (transport.getLoopStart()) + ", not "
                        + std::to_string (sampleAt (1920)) + " under the edit cursor");
        click (5760);
    } });
    steps->push_back ({ 150, [press] { press (5760, "]"); } });
    steps->push_back ({ 150, [&ctx, &transport, sampleAt, click]
    {
        ctx.expect (transport.getLoopStart() == sampleAt (1920) && transport.getLoopEnd() == sampleAt (5760)
                        && transport.isLoopEnabled(),
                    "] left the loop at " + std::to_string (transport.getLoopStart()) + " to "
                        + std::to_string (transport.getLoopEnd()) + ", not " + std::to_string (sampleAt (1920))
                        + " to " + std::to_string (sampleAt (5760)));
        click (2880);
    } });
    steps->push_back ({ 150, [press] { press (2880, "shift + " + keyCodeDescription ('{')); } });
    steps->push_back ({ 150, [click] { click (4800); } });
    steps->push_back ({ 150, [press] { press (4800, "shift + " + keyCodeDescription ('}')); } });
    steps->push_back ({ 150, [&ctx, &transport, sampleAt]
    {
        ctx.expect (transport.getPunchIn() == sampleAt (2880) && transport.getPunchOut() == sampleAt (4800)
                        && transport.isPunchEnabled(),
                    "Shift+[ and Shift+] left the punch at " + std::to_string (transport.getPunchIn()) + " to "
                        + std::to_string (transport.getPunchOut()) + ", not " + std::to_string (sampleAt (2880))
                        + " to " + std::to_string (sampleAt (4800)));
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoLoopKeysWithoutDevice { Scenario {
    "gui.piano_loop_keys_without_device", { "gui", "piano", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoLoopKeysWithoutDevice (host, ctx); }
} };

std::optional<ScenarioResult> runPianoNoteCreation (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (0);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &ctx, &track, mode = session.editMode, regions = track.midiRegions.current()]
    {
        host.closePianoRoll();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        ctx.session().editMode = mode;
        ctx.engine().getUndoManager().clearUndoHistory();
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion region;
    region.lengthInTicks = 1920;
    region.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 2.0);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    ctx.engine().getUndoManager().clearUndoHistory();
    host.openPianoRoll (0, 0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx, &session]
    {
        ctx.expect (host.pressPeerKey ("D", 'd') && session.editMode == EditMode::Draw, "D did not select Draw mode");
        ctx.expect (host.pressPianoRollKey ("5"), "the sixteenth-note grid key was not handled");
        ctx.expect (host.clickPianoGrid (240, 60), "the first empty note cell was not visible");
    } });
    steps->push_back ({ 100, [&host, &ctx, &track]
    {
        const auto& regions = track.midiRegions.current();
        if (ctx.expect (regions.size() == 1 && regions[0].notes.size() == 1, "the first grid click did not create one note"))
        {
            const auto& note = regions[0].notes[0];
            ctx.expect (note.noteNumber == 60 && note.startTick == 240 && note.lengthInTicks == 120 && note.velocity == 100,
                        "the new note did not use its cell, snap length and default velocity");
        }
        ctx.expect (host.pressPianoRollKey ("4"), "the eighth-note grid key was not handled");
    } });
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.clickPianoGrid (960, 64), "the second empty note cell was not visible"); } });
    steps->push_back ({ 100, [&host, &ctx, &track]
    {
        const auto& regions = track.midiRegions.current();
        if (ctx.expect (regions.size() == 1 && regions[0].notes.size() == 2, "the second grid click did not create a second note"))
        {
            const auto& note = regions[0].notes[1];
            ctx.expect (note.noteNumber == 64 && note.startTick == 960 && note.lengthInTicks == 240 && note.velocity == 100,
                        "changing snap did not change the next note's duration");
        }
        ctx.expect (host.pressPianoRollKey ("0"), "the free-grid key was not handled");
    } });
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.clickPianoGrid (1200, 67), "the free-grid note cell was not visible"); } });
    steps->push_back ({ 100, [&ctx, &track]
    {
        const auto& regions = track.midiRegions.current();
        if (ctx.expect (regions.size() == 1 && regions[0].notes.size() == 3, "the free-grid click did not create a third note"))
            ctx.expect (regions[0].notes[2].noteNumber == 67 && regions[0].notes[2].lengthInTicks == 480
                        && regions[0].notes[2].velocity == 100,
                        "snap-off note creation did not retain the quarter-note default");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoNoteCreation { Scenario {
    "gui.piano_note_creation", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoNoteCreation (host, ctx); }
} };

std::optional<ScenarioResult> runPianoVelocity (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &ctx, &track, regions = track.midiRegions.current()]
    {
        host.closePianoRoll();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        ctx.engine().getUndoManager().clearUndoHistory();
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion region;
    region.lengthInTicks = 1920;
    region.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 2.0);
    MidiNote note;
    note.noteNumber = 60;
    note.startTick = 240;
    note.lengthInTicks = 240;
    note.velocity = 100;
    note.channel = 2;
    region.notes.push_back (note);
    note.noteNumber = 64;
    note.startTick = 960;
    region.notes.push_back (note);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    ctx.engine().getUndoManager().clearUndoHistory();
    host.openPianoRoll (0, 0);
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto fraction : { 1.1f, -0.1f, 0.5f })
    {
        steps->push_back ({ 500, [&host, &ctx, fraction]
        { ctx.expect (host.dragPianoVelocity (300, fraction), "the velocity bar was unavailable for dragging"); } });
        steps->push_back ({ 100, [&ctx, &track, fraction]
        {
            const auto& notes = track.midiRegions.current()[0].notes;
            if (! ctx.expect (notes.size() == 2, "velocity editing changed the note count")) return;
            const int expected = fraction > 1.0f ? 127 : fraction < 0.0f ? 1 : 64;
            ctx.expect (std::abs (notes[0].velocity - expected) <= (expected == 64 ? 1 : 0),
                        "dragging the velocity bar did not set or clamp its value");
            ctx.expect (notes[0].noteNumber == 60 && notes[0].startTick == 240
                        && notes[0].lengthInTicks == 240 && notes[0].channel == 2,
                        "velocity dragging changed the note's pitch, timing or channel");
            ctx.expect (notes[1].velocity == 100 && notes[1].noteNumber == 64 && notes[1].startTick == 960,
                        "velocity dragging altered the neighboring note");
        } });
    }
    auto height = std::make_shared<int> (0);
    steps->push_back ({ 500, [&host, &ctx, height]
    {
        *height = host.pianoVelocityHeight();
        ctx.expect (host.resizePianoVelocity (24), "the velocity strip resize handle was unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, height]
    { ctx.expect (host.pianoVelocityHeight() == *height + 24, "dragging up did not grow the velocity strip"); } });
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.resizePianoVelocity (-24), "the velocity strip could not be shrunk"); } });
    steps->push_back ({ 100, [&host, &ctx, height]
    {
        ctx.expect (host.pianoVelocityHeight() == *height, "dragging down did not restore the velocity strip");
        ctx.expect (host.wheelPianoVelocity (notch (2.0f)), "the velocity strip did not accept a wheel gesture");
    } });
    steps->push_back ({ 100, [&host, &ctx, height]
    {
        ctx.expect (host.pianoVelocityHeight() == *height + 16, "wheel-up did not grow the velocity strip");
        ctx.expect (host.wheelPianoVelocity (notch (-2.0f)), "the velocity strip did not accept wheel-down");
    } });
    steps->push_back ({ 100, [&host, &ctx, height]
    { ctx.expect (host.pianoVelocityHeight() == *height, "wheel-down did not restore the velocity strip"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoVelocity { Scenario {
    "gui.piano_velocity", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoVelocity (host, ctx); }
} };

std::optional<ScenarioResult> runPianoCc (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &ctx, &track, regions = track.midiRegions.current(), mode = ctx.session().editMode]
    {
        host.closePianoRoll();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        ctx.session().editMode = mode;
        ctx.engine().getUndoManager().clearUndoHistory();
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion region;
    region.lengthInTicks = 1920;
    region.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 2.0);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    ctx.engine().getUndoManager().clearUndoHistory();
    host.openPianoRoll (0, 0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey ("G", 'g'), "G did not select Grab mode");
        ctx.expect (host.togglePianoCc(), "the CC lane button was unavailable");
    } });
    for (const float fraction : { 1.1f, -0.1f, 0.75f })
    {
        steps->push_back ({ 500, [&host, &ctx, fraction]
        { ctx.expect (host.dragPianoCc (240, fraction), "the CC lane was unavailable for drawing"); } });
        steps->push_back ({ 100, [&ctx, &track, fraction]
        {
            const auto& events = track.midiRegions.current()[0].ccs;
            if (! ctx.expect (events.size() == 1, "editing a CC bar did not keep exactly one event")) return;
            const int expected = fraction > 1.0f ? 127 : fraction < 0.0f ? 0 : 95;
            ctx.expect (events[0].controller == 1 && events[0].atTick == 240 && events[0].channel == 1
                        && events[0].value == expected, "CC drawing did not preserve its controller/time or clamp its value");
        } });
    }
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("#6c", 'l'), "L did not select the next CC controller"); } });
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.dragPianoCc (960, 0.5f), "the second controller lane was unavailable"); } });
    steps->push_back ({ 100, [&ctx, &track]
    {
        const auto& events = track.midiRegions.current()[0].ccs;
        if (! ctx.expect (events.size() == 2, "drawing the second controller did not add one event")) return;
        ctx.expect (events[0].controller == 1 && events[0].value == 95 && events[0].atTick == 240,
                    "changing the active controller altered its previous event");
        ctx.expect (events[1].controller == 7 && events[1].value == 64 && events[1].atTick == 960,
                    "the selected controller was not used for the new CC bar");
    } });
    auto height = std::make_shared<int> (0);
    steps->push_back ({ 500, [&host, &ctx, height]
    {
        *height = host.pianoCcHeight();
        ctx.expect (host.resizePianoCc (24), "the CC strip resize handle was unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, height]
    { ctx.expect (host.pianoCcHeight() == *height + 24, "dragging up did not grow the CC strip"); } });
    steps->push_back ({ 500, [&host, &ctx]
    { ctx.expect (host.resizePianoCc (-24), "the CC strip could not be shrunk"); } });
    steps->push_back ({ 100, [&host, &ctx, height]
    { ctx.expect (host.pianoCcHeight() == *height, "dragging down did not restore the CC strip"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoCc { Scenario {
    "gui.piano_cc", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoCc (host, ctx); }
} };

std::optional<ScenarioResult> runPianoEditKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&host, &engine, &session, &track, regions = track.midiRegions.current(),
                  mode = session.editMode]
    {
        host.closePiano();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        session.editMode = mode;
        engine.getUndoManager().clearUndoHistory();
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    // Long enough that a quarter-view pan stops well short of the end on any
    // window, so Cmd+Right, End and Cmd+Left each land somewhere different.
    MidiRegion region;
    region.lengthInTicks = kMidiTicksPerQuarter * 512;
    region.lengthInSamples = session.ticksToSamples (region.lengthInTicks, engine.getCurrentSampleRate());
    region.notes = { { 1, 60, 100, 240, 120 }, { 1, 60, 100, 360, 120 }, { 1, 67, 85, 600, 240 } };
    const auto original = region.notes;
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    engine.getUndoManager().clearUndoHistory();
    if (! host.openPiano (0, 0)) return ScenarioResult::fail ("piano roll did not open");

    // The roll takes the keys itself, so these are pressed the way X11 hands
    // them to the focused component: a chord's key code is the lowercase
    // keysym and its text is a control character.
    const auto command = [&host] (char letter)
    { return host.focusPiano()
          && host.pressPeerKey ("command + " + keyCodeDescription (letter), controlCharacter (letter)); };
    const auto notes = [&track]() -> const std::vector<MidiNote>&
    { return track.midiRegions.current()[0].notes; };
    const auto selectAll = [&ctx, command]
    { ctx.expect (command ('a'), "Select All was not handled"); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx, &session]
    {
        ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('g'), 'g')
                    && session.editMode == EditMode::Grab, "G did not select Grab mode");
        ctx.expect (host.pianoNotePointer (300, 60, true), "the first note was not clickable");
        host.pianoNotePointer (300, 60, false);
    } });
    steps->push_back ({ 150, [&ctx, &host, command, selectAll]
    {
        ctx.expect (host.pianoSelection() == std::vector<int> { 0 },
                    "clicking the first note did not select it alone");
        selectAll();
        ctx.expect (command ('c'), "Copy was not handled");
        ctx.expect (command ('v'), "Paste was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &host, notes, original, command]
    {
        if (ctx.expect (notes().size() == 6, "Paste did not add every copied note"))
            ctx.expect (std::vector<MidiNote> (notes().begin() + 3, notes().end()) == original,
                        "Paste did not land the copies at the edit cursor with their pitch and velocity");
        ctx.expect (host.pianoSelection() == std::vector<int> { 3, 4, 5 },
                    "Paste did not select what it pasted");
        ctx.expect (command ('z'), "Undo after Paste was not handled");
    } });
    steps->push_back ({ 150, [&ctx, notes, original, selectAll, command]
    {
        ctx.expect (notes() == original, "Undo did not remove the pasted notes");
        selectAll();
        ctx.expect (command ('x'), "Cut was not handled");
        ctx.expect (notes().empty(), "Cut did not remove the selected notes");
        ctx.expect (command ('v'), "Paste after Cut was not handled");
    } });
    steps->push_back ({ 150, [&ctx, notes, original, command]
    {
        ctx.expect (notes() == original, "Paste did not restore the cut notes where they came from");
        ctx.expect (command ('z'), "Undo after Paste was not handled");
        ctx.expect (notes().empty(), "Undo did not take the pasted notes away");
        ctx.expect (command ('z'), "Undo after Cut was not handled");
        ctx.expect (notes() == original, "Undo did not bring the cut notes back");
    } });
    steps->push_back ({ 150, [&ctx, &host, notes, original, selectAll, command]
    {
        selectAll();
        ctx.expect (command ('d'), "Duplicate was not handled");
        if (ctx.expect (notes().size() == 6, "Duplicate did not clone every note"))
        {
            auto expected = original;
            for (auto& note : expected) note.startTick += 600;
            ctx.expect (std::vector<MidiNote> (notes().begin() + 3, notes().end()) == expected,
                        "Duplicate did not place the clones after the selection's span");
        }
        ctx.expect (host.pianoSelection() == std::vector<int> { 3, 4, 5 },
                    "Duplicate did not select the clones");
        ctx.expect (command ('z'), "Undo after Duplicate was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &host, notes, original, selectAll]
    {
        ctx.expect (notes() == original, "Undo did not remove the duplicated notes");
        selectAll();
        ctx.expect (host.focusPiano() && host.pressPeerKey (keyCodeDescription ('v'), 'v'),
                    "the velocity key was not handled");
    } });
    steps->push_back ({ 200, [&ctx, &host]
    { ctx.expect (host.clickContextMenuItem ("Set to 64 (mid)"), "the velocity popup did not offer a value"); } });
    steps->push_back ({ 200, [&ctx, &host, notes, command]
    {
        ctx.expect (host.modalStackEmpty(), "the velocity popup stayed open");
        ctx.expect (notes().size() == 3
                    && std::all_of (notes().begin(), notes().end(),
                                    [] (const MidiNote& note) { return note.velocity == 64; }),
                    "the velocity popup did not set the selected notes");
        ctx.expect (command ('z'), "Undo after the velocity popup was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &host, notes, original, selectAll, command]
    {
        ctx.expect (notes() == original, "Undo did not restore the original velocities");
        selectAll();
        ctx.expect (host.focusPiano() && host.pressPeerKey ("shift + G", 'G'), "the glue key was not handled");
        if (ctx.expect (notes().size() == 2, "Shift+G did not join the two contiguous notes"))
            ctx.expect (notes()[0].noteNumber == 60 && notes()[0].startTick == 240
                        && notes()[0].lengthInTicks == 240 && notes()[1] == original[2],
                        "Shift+G joined the wrong notes or the wrong span");
        ctx.expect (command ('z'), "Undo after the glue key was not handled");
    } });
    auto scroll = std::make_shared<double> (0.0);
    steps->push_back ({ 150, [&ctx, &host, notes, original, scroll]
    {
        ctx.expect (notes() == original, "Undo did not take the glued notes apart");
        ctx.expect (host.pianoViewport()[1] < 0.5, "the roll did not open at the region's start");
        ctx.expect (host.focusPiano() && host.pressPeerKey ("command + cursor right"),
                    "the pan-right key was not handled");
        *scroll = host.pianoViewport()[1];
        ctx.expect (*scroll > 0.5, "Cmd+Right did not pan the view");
    } });
    steps->push_back ({ 150, [&ctx, &host, scroll]
    {
        ctx.expect (host.focusPiano() && host.pressPeerKey ("home"), "Home was not handled");
        ctx.expect (host.pianoViewport()[1] < 0.5, "Home did not jump the view to the region start");
        ctx.expect (host.focusPiano() && host.pressPeerKey ("end"), "End was not handled");
        ctx.expect (host.pianoViewport()[1] > *scroll,
                    "End did not jump the view to the region end: it scrolled to "
                        + std::to_string ((int) host.pianoViewport()[1]) + " px, Cmd+Right to "
                        + std::to_string ((int) *scroll));
        ctx.expect (host.focusPiano() && host.pressPeerKey ("command + cursor left"),
                    "the pan-left key was not handled");
    } });
    steps->push_back ({ 150, [&ctx, &host, scroll]
    {
        ctx.expect (host.pianoViewport()[1] > 0.5, "Cmd+Left panned past the region start");
        ctx.expect (host.focusPiano() && host.pressPeerKey ("home"), "Home was not handled a second time");
        ctx.expect (host.pianoViewport()[1] < 0.5, "Home did not return the view to the region start");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pianoEditKeys { Scenario {
    "gui.piano_edit_keys", { "gui", "midi", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoEditKeys (host, ctx); }
} };

std::optional<ScenarioResult> runVirtualKeyboardKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    if (! transport.isStopped() || ! host.modalStackEmpty() || host.virtualKeyboardOpen())
        return ScenarioResult::skip ("requires a stopped transport, no modal and no keyboard");
    const int keyboardInput = engine.getVirtualKeyboardInputIndex();
    if (keyboardInput < 0) return ScenarioResult::skip ("the engine offers no virtual keyboard input");
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiChannel);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, index == 0);
    }
    ctx.keep (session.countInEnabled);
    ctx.cleanup ([&host, &engine, &transport, &track,
                  regions = track.midiRegions.current(),
                  at = transport.getPlayhead(), loop = transport.isLoopEnabled(),
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled()]
    {
        engine.stop();
        host.closeVirtualKeyboard();
        drainModals (host);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        engine.getUndoManager().clearUndoHistory();
        transport.setPlayhead (at);
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
    });
    engine.stop();
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    track.midiInputIndex.store (keyboardInput);
    track.midiChannel.store (1);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    engine.getUndoManager().clearUndoHistory();
    transport.setPlayhead (0);
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);

    const auto rate = engine.getCurrentSampleRate();
    const auto second = (std::int64_t) std::llround (rate);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.pressKey ("K", 'k'), "K was not handled"); } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        if (! ctx.expect (host.virtualKeyboardOpen(), "K did not open the virtual keyboard")) return;
        ctx.expect (host.clickRecord(), "Record was unavailable with the keyboard open");
    } });
    // P and R belong to the layout while the keyboard is up, so each has to
    // sound its note and leave the punch and record buttons alone.
    steps->push_back ({ 200, [&host, &ctx, &transport]
    {
        ctx.expect (transport.isRecording(), "Record did not start with the keyboard open");
        ctx.expect (host.inputVirtualKeyboard ("p"), "P did not reach the keyboard");
    } });
    steps->push_back ({ 400, [&host, &ctx, &transport]
    {
        ctx.expect (! transport.isPunchEnabled(), "P toggled punch instead of playing its note");
        ctx.expect (host.inputVirtualKeyboard ("r"), "R did not reach the keyboard");
    } });
    steps->push_back ({ 400, [&host, &ctx, &transport]
    {
        ctx.expect (transport.isRecording(), "R stopped the recording instead of playing its note");
        ctx.expect (host.inputVirtualKeyboard (" "), "Space did not reach the keyboard");
    } });
    steps->push_back ({ 400, [&ctx, &track, &transport]
    {
        ctx.expect (transport.isStopped(), "Space did not stop the transport from the keyboard");
        const auto& regions = track.midiRegions.current();
        if (! ctx.expect (regions.size() == 1, "the keyboard's letters did not record a MIDI region")) return;
        auto notes = regions.front().notes;
        if (! ctx.expect (notes.size() == 2, "P and R did not each play exactly one note")) return;
        std::sort (notes.begin(), notes.end(),
                   [] (const MidiNote& a, const MidiNote& b) { return a.noteNumber < b.noteNumber; });
        // R and P sit 17 and 28 semitones above the keyboard's centre note.
        ctx.expect (notes[1].noteNumber - notes[0].noteNumber == 11,
                    "the two letters did not play the pitches their layout positions name");
    } });
    // The keys the layout does not hold keep driving the transport behind it.
    steps->push_back ({ 150, [&host, &ctx, &transport, second]
    {
        transport.setPlayhead (second);
        ctx.expect (host.inputVirtualKeyboard ("["), "the loop-in key did not reach the keyboard");
    } });
    steps->push_back ({ 200, [&host, &ctx, &transport, second]
    {
        transport.setPlayhead (second * 3);
        ctx.expect (host.inputVirtualKeyboard ("]"), "the loop-out key did not reach the keyboard");
    } });
    steps->push_back ({ 200, [&host, &ctx, &transport, second]
    {
        ctx.expect (transport.getLoopStart() == second && transport.getLoopEnd() == second * 3
                    && transport.isLoopEnabled(),
                    "the bracket keys did not place and arm the loop range from the keyboard");
        ctx.expect (host.inputVirtualKeyboard ("l"), "the loop toggle did not reach the keyboard");
    } });
    steps->push_back ({ 200, [&host, &ctx, &transport]
    {
        ctx.expect (! transport.isLoopEnabled(), "L did not turn the loop off from the keyboard");
        ctx.expect (host.inputVirtualKeyboard ("k"), "K did not reach the keyboard");
    } });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.virtualKeyboardOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "K did not close the virtual keyboard");
    });
    return std::nullopt;
}

const ScenarioRegistrar virtualKeyboardKeys { Scenario {
    "gui.virtual_keyboard_keys", { "gui", "midi", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 25000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runVirtualKeyboardKeys (host, ctx); }
} };

// However the virtual keyboard closes, the window's shortcuts answer at once,
// with no click to hand the window its keyboard back. Each close starts with
// nothing in the window holding the keyboard, as the keyboard's native child
// leaves it on Windows. The Win32 focus itself is not reproduced here; what
// is checked is that every close puts the keyboard back on the canvas.
std::optional<ScenarioResult> runVirtualKeyboardCloseFocus (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    return ScenarioResult::skip ("built without the native keyboard");
   #endif
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || host.virtualKeyboardOpen())
        return ScenarioResult::skip ("requires a stopped transport, no modal and no keyboard");
    auto& um = engine.getUndoManager();
    ctx.cleanup ([&host, &um]
    {
        host.closeVirtualKeyboard();
        drainModals (host);
        um.clearUndoHistory();
    });
    auto edits = std::make_shared<int> (0);
    struct Route { std::string name; std::function<bool()> close; };
    const std::vector<Route> routes {
        { "Escape in the keyboard",       [&host] { return host.inputVirtualKeyboard ("escape"); } },
        { "K in the keyboard",            [&host] { return host.inputVirtualKeyboard ("k"); } },
        { "the keyboard button",          [&host] { return host.clickVirtualKeyboardButton(); } },
        { "a click outside the keyboard", [&host] { return host.clickVirtualKeyboardDim(); } }
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& route : routes)
    {
        steps->push_back ({ 150, [&host, &ctx, &um, edits]
        {
            um.beginNewTransaction ("Keyboard focus probe");
            um.perform (new ParamEditAction ([edits] { ++*edits; }, [edits] { --*edits; }));
            ctx.expect (host.pressKey ("K", 'k'), "K was not handled");
        } });
        steps->push_back ({ 600, [&host, &ctx, route]
        {
            if (! ctx.expect (host.virtualKeyboardOpen(), "K did not open the keyboard before " + route.name)) return;
            (void) host.unfocusWindow();
            ctx.expect (route.close(), route.name + " did not reach the keyboard");
        } });
        steps->push_back ({ 0, [&host, &ctx, edits, route]
        {
            ctx.expect (host.canvasHasKeyboardFocus(), route.name + " left the canvas without the keyboard");
            ctx.expect (host.pressPeerKey ("command + Z", 0) && *edits == 0,
                        "Undo at the window did nothing after " + route.name);
        }, [&host] { return ! host.virtualKeyboardOpen(); }, route.name + " did not close the keyboard" });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar virtualKeyboardCloseFocus { Scenario {
    "gui.virtual_keyboard_close_focus", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runVirtualKeyboardCloseFocus (host, ctx); }
} };

// The virtual keyboard covers the window as a dialog does, so of the keys outside
// its layout only a dialog's set acts behind it, wherever the key arrives: Delete
// leaves the selected region it hides, A arms nothing and 1 pages no bank, while
// L still reaches the loop. K and Escape at the window close it.
std::optional<ScenarioResult> runVirtualKeyboardHoldsKeys (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without the native keyboard");
   #else
    if (host.virtualKeyboardOpen()) return ScenarioResult::skip ("requires a closed virtual keyboard");
    AudioRegion region;
    if (auto early = seedTapeRegion (host, ctx, region)) return early;
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&host, &session, armed = session.track (0).recordArmed.load()]
    {
        host.closeVirtualKeyboard();
        session.setTrackArmed (0, armed);
    });
    const auto plain = [&host] (char letter) { return host.pressPeerKey (keyCodeDescription (letter), letter); };
    const auto open = [&host] { return host.virtualKeyboardOpen(); };
    const auto closed = [&host] { return ! host.virtualKeyboardOpen(); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickAudioRegion (0, 0), "could not select the region with a timeline click"); } });
    steps->push_back ({ 200, [plain] { plain ('k'); } });
    steps->push_back ({ 0, [&host, &ctx, &session, &transport, plain]
    {
        host.pressPeerKey ("delete", 0);
        plain ('a');
        const bool paged = plain ('1');
        ctx.expect (session.track (0).regions.size() == 1, "Delete behind the virtual keyboard removed the region it hides");
        ctx.expect (! session.anyTrackArmed(), "A behind the virtual keyboard armed a track");
        ctx.expect (! paged, "1 behind the virtual keyboard paged the console");
        ctx.expect (host.virtualKeyboardOpen() && host.modalStackEmpty(), "the keys behind the virtual keyboard closed it");
        const bool loopWas = transport.isLoopEnabled();
        plain ('l');
        ctx.expect (transport.isLoopEnabled() != loopWas, "L behind the virtual keyboard did not reach the loop");
        transport.setLoopEnabled (loopWas);
        plain ('k');
    }, open, "K did not open the virtual keyboard" });
    steps->push_back ({ 0, [plain] { plain ('k'); }, closed, "K at the window did not close the virtual keyboard" });
    steps->push_back ({ 0, [&host] { host.pressPeerKey ("escape"); }, open, "K did not open the virtual keyboard again" });
    steps->push_back ({ 0, [&host] { host.pressPeerKey ("delete", 0); }, closed,
                       "Escape at the window did not close the virtual keyboard" });
    steps->push_back ({ 200, [&ctx, &session]
    { ctx.expect (session.track (0).regions.empty(), "Delete with the virtual keyboard closed did not remove the selected region"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar virtualKeyboardHoldsKeys { Scenario {
    "gui.virtual_keyboard_holds_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runVirtualKeyboardHoldsKeys (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
