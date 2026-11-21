#include "../GuiScenarioSupport.h"
#include "../../../dsp/MultiCompStage.h"
#include "../../../dsp/MultibandCompPresets.h"
#include "../../../engine/audiofile/FileWriter.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The MASTERING page's multiband compressor: its controls are on the page, in the
// compressor's own native panel, and each one is worked here as the pointer works it.
namespace duskstudio::scenario::gui
{
namespace
{
constexpr const char* kPanel = "mastering-comp";
constexpr const char* kNoReadBack = "reads the editor window back through the X server";
// GuiHost's pointer modifier bits: Cmd on macOS, Ctrl elsewhere.
constexpr int kCommand = 2;

void keepComp (ScenarioContext& ctx, CompParams& comp)
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        ctx.keep (comp.atom ((CompParamId) i));
}

std::string symbolOf (CompParamId id) { return compParamSpec (id).symbol; }

bool panelReadable (GuiHost& host)
{
   #if defined (__linux__)
    return ! host.nativeWaylandWindow();
   #else
    (void) host;
    return false;
   #endif
}

bool same (float a, float b) { return ! (a < b) && ! (b < a); }

bool compPointer (GuiHost& host, const std::string& control, float position, bool pressed, int modifiers = 0)
{
    return host.masteringPanelPointer (kPanel, control, position, pressed, modifiers);
}

std::optional<ScenarioResult> requireNativeMastering (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI || ! DUSKSTUDIO_HAS_DUSK_DSP
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI and the Dusk DSP");
   #else
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and mastering player and no modal");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the native mastering panels can embed into");
    return std::nullopt;
   #endif
}

// Every Multiband value but `moved` is still what `before` holds, and `moved` is not.
void expectOnly (ScenarioContext& ctx, const CompParams& comp, const CompParams::Values& before,
                 CompParamId moved, const std::string& what)
{
    ctx.expect (std::abs (comp.get (moved) - before[(std::size_t) moved]) > 1.0e-5f,
                what + " left " + symbolOf (moved) + " at " + std::to_string (comp.get (moved)));
    for (const auto id : compStageParams (CompStage::Multiband))
        if (id != moved)
            ctx.expect (same (comp.get (id), before[(std::size_t) id]),
                        what + " moved " + symbolOf (id) + " from " + std::to_string (before[(std::size_t) id])
                            + " to " + std::to_string (comp.get (id)));
}

// The Multiband controls the panel's last frame did not draw whole and apart.
std::vector<std::string> hiddenControls (GuiHost& host)
{
    std::vector<std::string> hidden;
    for (const auto id : compStageParams (CompStage::Multiband))
        if (! host.masteringControl (kPanel, symbolOf (id)))
            hidden.push_back (symbolOf (id));
    return hidden;
}

std::string joined (const std::vector<std::string>& names)
{
    std::string text;
    for (const auto& name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text;
}

// A press, a move in four steps and a release, each a step apart, so the panel
// draws every edge as the pointer would make it. The release lands over `to`.
void pushDrag (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps, const std::string& from,
               float fromPosition, const std::string& to, float toPosition)
{
    steps.push_back ({ 100, [&host, &ctx, from, fromPosition]
    { ctx.expect (compPointer (host, from, fromPosition, true), "the panel has no " + from + " to press"); } });
    for (int i = 1; i <= 4; ++i)
    {
        const float position = to == from ? fromPosition + (toPosition - fromPosition) * (float) i / 4.0f : toPosition;
        steps.push_back ({ 40, [&host, &ctx, to, position]
        { ctx.expect (compPointer (host, to, position, true), "the panel has no " + to + " to drag over"); } });
    }
    steps.push_back ({ 60, [&host, &ctx, to, toPosition]
    { ctx.expect (compPointer (host, to, toPosition, false), "the panel has no " + to + " to let go over"); } });
}

void pushClick (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps, const std::string& control,
                int modifiers = 0)
{
    steps.push_back ({ 100, [&host, &ctx, control, modifiers]
    { ctx.expect (compPointer (host, control, 0.5f, true, modifiers), "the panel has no " + control); } });
    steps.push_back ({ 60, [&host, &ctx, control, modifiers]
    { ctx.expect (compPointer (host, control, 0.5f, false, modifiers), "the panel lost " + control); } });
}

// On the MASTERING page, with nothing clicked, every parameter of the multiband
// compressor has a control the panel drew whole, inside it and clear of the others:
// in the wide layouts the manual's figures use, at 1366 x 768, in the smallest
// window, and in the compact layout with the compressor picked.
std::optional<ScenarioResult> runMasteringCompPanel (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireNativeMastering (host, ctx)) return skip;
    const auto size = host.mainWindowSize();
    if (size.size() < 4) return ScenarioResult::fail ("the window size and its limits are unavailable");
    auto& mastering = ctx.session().mastering();
    keepComp (ctx, mastering.comp);
    ctx.keep (mastering.compEnabled);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    const int module = host.masteringModuleIndex();
    ctx.cleanup ([&host, size, module]
    {
        drainModals (host);
        host.restoreMasteringModule (module);
        host.resizeMainWindow (size[0], size[1]);
    });
    mastering.comp.resetToDefaults (CompStage::Multiband);
    host.restoreMasteringModule (1);

    struct Layout
    {
        int width, height;
        std::string name;
        bool sideBySide = false;
    };
    const std::vector<Layout> layouts {
        { std::max (1440, size[0]), std::max (900, size[1]), "the window the case found, widened" },
        { 1424, 1120, "a 1424 x 1120 window" },
        { 1920, 1080, "a 1920 x 1080 window" },
        { 1366, 768, "a 1366 x 768 window" },
        { host.masteringSideBySideWidth() + 1, size[3], "the narrowest window with every module side by side", true },
        { size[2], size[3], "the smallest window" },
        { std::max (size[2], 1040), std::max (size[3], 900), "a compact window" },
    };
    auto frames = std::make_shared<std::uint64_t> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& layout : layouts)
    {
        steps->push_back ({ 0, [&host, &ctx, frames, layout]
        {
            *frames = host.panelFrames (kPanel).value_or (0);
            ctx.expect (host.resizeMainWindow (layout.width, layout.height),
                        "could not size the window for " + layout.name);
        } });
        steps->push_back ({ 500, [&host, &ctx, layout]
        {
            const auto hidden = hiddenControls (host);
            const auto now = host.mainWindowSize();
            ctx.note (layout.name + ": " + (now.size() >= 2 ? std::to_string (now[0]) + " x " + std::to_string (now[1])
                                                            : std::string ("unknown size"))
                      + (host.masteringModulePickerShown() ? ", compact" : ""));
            if (layout.sideBySide)
                ctx.expect (! host.masteringModulePickerShown(), "in " + layout.name + " the page shows one module at a time");
            ctx.expect (hidden.empty(), "in " + layout.name + " the multiband panel does not show "
                                            + joined (hidden) + " whole");
        }, [&host, frames]
        {
            const auto drawn = host.panelFrames (kPanel);
            if (! drawn) return false;
            // A layout switch that put up a new window starts its count again.
            if (*drawn < *frames) *frames = 0;
            return *drawn >= *frames + 2 && host.masteringControl (kPanel, "plot");
        }, "the multiband panel did not draw in " + layout.name });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringCompPanel { Scenario {
    "gui.mastering_comp_panel", { "gui", "mastering", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompPanel (host, ctx); }
} };

// The multiband panel's controls through the pointer: each switch, dial and
// crossover moves its own value and no other, and none of them engages the
// compressor, whose header's status light and label both switch it. A double-click
// puts a dial back to the plug-in's default, and a command-click types a value. The
// last two enabled bands stay on. A value set in the session, a preset and a session
// load each redraw the panel, and the band settings come back with a saved session.
std::optional<ScenarioResult> runMasteringCompControls (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireNativeMastering (host, ctx)) return skip;
   #if DUSKSTUDIO_HAS_NATIVE_UI && DUSKSTUDIO_HAS_DUSK_DSP
    static constexpr int kPopPreset = 1;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& mastering = session.mastering();
    auto& comp = mastering.comp;
    const auto& stage = engine.getMasteringChain().getCompressor();
    auto& player = engine.getMasteringPlayer();
    const auto originalDir = currentSessionDirectory (session);
    const auto originalSource = mastering.sourceFile;
    const auto originalMix = player.getLoadedFile();
    const auto originalPosition = player.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    // The reopen makes restore.json's folder the session's, where a mix saved
    // relative to the session folder would not be found, so the mix is left out
    // of it and put back by hand.
    mastering.sourceFile = decltype (mastering.sourceFile) {};
    const bool restoreSaved = SessionSerializer::save (session, restore);
    mastering.sourceFile = originalSource;
    if (! restoreSaved) return ScenarioResult::fail ("could not save the session");
    keepComp (ctx, comp);
    ctx.keep (mastering.compEnabled);
    keepStage (host, ctx);
    if (! ctx.expect (host.mainWindowSize().size() >= 2, "the window size is unavailable")) return ctx.verdict();
    const auto size = host.mainWindowSize();
    ctx.cleanup ([&host, &session, &mastering, &player, originalDir, originalSource, originalMix, originalPosition,
                  restore, size]
    {
        drainModals (host);
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        mastering.sourceFile = originalSource;
        if (originalMix.existsAsFile()) player.loadFile (originalMix);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        host.refreshMasteringSource();
        host.resizeMainWindow (size[0], size[1]);
    });
    if (! host.resizeMainWindow (std::max (1440, size[0]), std::max (900, size[1])))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    comp.resetToDefaults (CompStage::Multiband);
    mastering.compEnabled.store (false);

    auto pop = std::make_shared<CompParams>();
    if (! mbpresets::apply (kPopPreset, stage, *pop)) return ScenarioResult::fail ("the Pop preset did not convert");
    float typedMakeup = 0.0f;
    const auto makeup3 = compBandParam (3, CompBandField::Makeup);
    if (! stage.valueFromText (makeup3, "3", typedMakeup)) return ScenarioResult::fail ("3 dB does not read as a makeup");

    auto values = std::make_shared<CompParams::Values>();
    // Where the low and high splits' handles sit at the plug-in's defaults, and
    // where the high one sat when the session was saved.
    auto lowAtDefault = std::make_shared<float> (0.0f);
    auto highAtDefault = std::make_shared<float> (0.0f);
    auto highSaved = std::make_shared<float> (0.0f);
    const auto take = [&comp, values] { *values = comp.values(); };
    const auto handleX = [&host] (const char* crossover)
    {
        const auto at = host.masteringControl (kPanel, crossover);
        return at ? (*at)[0] : -1.0f;
    };
    // Whether the handle is drawn within a pixel of `x`, itself a drawn position.
    const auto handleAt = [handleX] (const char* crossover, float x)
    {
        const float now = handleX (crossover);
        return now >= 0.0f && x >= 0.0f && std::abs (now - x) < 1.0f;
    };
    const auto saved = ctx.tempDir() / "bands" / "session.json";
    std::filesystem::create_directories (saved.parent_path());

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx, take, lowAtDefault, highAtDefault, handleX]
    {
        take();
        *lowAtDefault = handleX ("mb_crossover_1");
        *highAtDefault = handleX ("mb_crossover_3");
        ctx.expect (*lowAtDefault >= 0.0f && *highAtDefault >= 0.0f, "the low or high crossover handle is not drawn");
    }, [&host]
    {
        return host.masteringPanelsOpen() == host.masteringPanelsExpected() && host.masteringControl (kPanel, "plot");
    },
       "the mastering panels never opened" });
    pushClick (host, ctx, *steps, "comp_enabled");
    steps->push_back ({ 0, [&ctx, &mastering]
    { ctx.expect (mastering.compEnabled.load(), "the MULTIBAND COMP header did not engage the compressor"); } });
    pushClick (host, ctx, *steps, "comp_enabled");
    steps->push_back ({ 0, [&ctx, &mastering, &comp, values]
    {
        ctx.expect (! mastering.compEnabled.load(), "the MULTIBAND COMP header did not switch the compressor out");
        ctx.expect (comp.values() == *values, "the header moved a band setting");
    } });

    pushClick (host, ctx, *steps, "mb_bypass_2");
    steps->push_back ({ 0, [&ctx, &comp, values, take]
    {
        expectOnly (ctx, comp, *values, compBandParam (2, CompBandField::Bypass), "a click on HIGH-MID BYP");
        ctx.expect (comp.get (compBandParam (2, CompBandField::Bypass)) > 0.5f, "BYP did not bypass HIGH-MID");
        take();
    } });
    pushDrag (host, ctx, *steps, "mb_threshold_1", 0.5f, "mb_threshold_1", 1.0f);
    steps->push_back ({ 0, [&ctx, &comp, values, take]
    {
        const auto id = compBandParam (1, CompBandField::Threshold);
        expectOnly (ctx, comp, *values, id, "a drag up LOW-MID THRESH");
        ctx.expect (comp.get (id) > (*values)[(std::size_t) id], "a drag up LOW-MID THRESH lowered it");
        take();
    } });
    pushDrag (host, ctx, *steps, "mb_crossover_2", 0.5f, "plot", 0.75f);
    steps->push_back ({ 0, [&ctx, &comp, values, take]
    {
        expectOnly (ctx, comp, *values, CompParamId::MbCrossover2, "a drag of the middle crossover to the right");
        ctx.expect (comp.get (CompParamId::MbCrossover2) > (*values)[(std::size_t) CompParamId::MbCrossover2],
                    "a drag of the middle crossover to the right lowered it");
        take();
    } });
    pushDrag (host, ctx, *steps, "mb_output", 0.5f, "mb_output", 0.0f);
    steps->push_back ({ 0, [&ctx, &comp, values, take]
    {
        expectOnly (ctx, comp, *values, CompParamId::MbOutput, "a drag down OUTPUT");
        ctx.expect (comp.get (CompParamId::MbOutput) < (*values)[(std::size_t) CompParamId::MbOutput],
                    "a drag down OUTPUT raised it");
        take();
    } });
    pushClick (host, ctx, *steps, "auto_makeup");
    steps->push_back ({ 0, [&host, &ctx, &comp, values, take]
    {
        expectOnly (ctx, comp, *values, CompParamId::AutoMakeup, "a click on AUTO MAKEUP");
        take();
        ctx.expect (host.doubleClickMasteringComp ("mb_threshold_1"), "the panel has no LOW-MID THRESH");
    } });
    steps->push_back ({ 300, [&ctx, &comp, values, take]
    {
        const auto id = compBandParam (1, CompBandField::Threshold);
        ctx.expect (same (comp.get (id), compParamSpec (id).hostDefault),
                    "a double-click left LOW-MID THRESH at " + std::to_string (comp.get (id)));
        expectOnly (ctx, comp, *values, id, "a double-click on LOW-MID THRESH");
        take();
    } });
    pushClick (host, ctx, *steps, "mb_makeup_3", kCommand);
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.typeInMasteringComp ("3"), "the makeup field took no text"); },
        [&host] { return host.masteringCompCapturesKeyboard(); }, "a command-click on HIGH MAKEUP opened no field" });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("return"), "the makeup field did not keep Return"); } });
    steps->push_back ({ 200, [&ctx, &comp, values, makeup3, typedMakeup]
    {
        ctx.expect (same (comp.get (makeup3), typedMakeup),
                    "typing 3 left HIGH MAKEUP at " + std::to_string (comp.get (makeup3)));
        expectOnly (ctx, comp, *values, makeup3, "typing into HIGH MAKEUP");
    }, [&host] { return ! host.masteringCompCapturesKeyboard(); }, "Return left the makeup field open" });

    // Two bands on, both locked; a third comes back on.
    steps->push_back ({ 0, [&comp]
    {
        comp.set (compBandParam (2, CompBandField::Enabled), 0.0f);
        comp.set (compBandParam (3, CompBandField::Enabled), 0.0f);
    } });
    pushClick (host, ctx, *steps, "mb_enabled_0");
    steps->push_back ({ 0, [&ctx, &comp]
    {
        ctx.expect (comp.get (compBandParam (0, CompBandField::Enabled)) > 0.5f,
                    "LOW went off with only two bands enabled");
        ctx.expect (comp.get (compBandParam (1, CompBandField::Enabled)) > 0.5f,
                    "a click on LOW ON switched LOW-MID off");
    } });
    pushClick (host, ctx, *steps, "mb_enabled_2");
    steps->push_back ({ 0, [&ctx, &comp, &mastering]
    {
        ctx.expect (comp.get (compBandParam (2, CompBandField::Enabled)) > 0.5f, "HIGH-MID ON did not switch it on");
        ctx.expect (! mastering.compEnabled.load(), "a band control engaged the compressor");
    } });

    // The session moves the low split; the panel redraws it there.
    steps->push_back ({ 0, [&comp] { comp.set (CompParamId::MbCrossover1, 0.2f); } });
    steps->push_back ({ 0, [&comp] { comp.resetToDefaults (CompStage::Multiband); },
                        [lowAtDefault, handleX]
    {
        const float x = handleX ("mb_crossover_1");
        return x >= 0.0f && x < *lowAtDefault - 4.0f;
    },
                        "the low crossover handle did not follow the session", {}, kSettleBoundMs });

    // A preset from the panel's own picker moves the high split well down; MIX and
    // OUTPUT keep their values and AUTO MAKEUP goes off.
    steps->push_back ({ 0, [&comp]
    {
        comp.set (CompParamId::MbMix, 70.0f);
        comp.set (CompParamId::MbOutput, -2.0f);
        comp.set (CompParamId::AutoMakeup, 1.0f);
    }, [lowAtDefault, handleAt] { return handleAt ("mb_crossover_1", *lowAtDefault); },
       "the panel did not redraw the defaults", {}, kSettleBoundMs });
    pushClick (host, ctx, *steps, "comp_preset");
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.masteringPanelsOpen() == host.masteringPanelsExpected(),
                    "the open preset list took a mastering panel down");
        for (int entry = 0; entry <= mbpresets::kNumPresets; ++entry)
            ctx.expect (host.masteringControl (kPanel, "list:" + std::to_string (entry)).has_value(),
                        "the open preset list does not show entry " + std::to_string (entry));
    }, [&host] { return host.masteringCompCapturesKeyboard(); }, "the preset picker did not open its list" });
    pushClick (host, ctx, *steps, "list:" + std::to_string (kPopPreset + 1));
    steps->push_back ({ 0, [&host, &ctx, &comp, &mastering, pop]
    {
        ctx.expect (host.masteringCompPreset() == kPopPreset + 1,
                    "the picker shows preset " + std::to_string (host.masteringCompPreset()) + ", not Pop");
        ctx.expect (same (comp.get (CompParamId::MbMix), 70.0f) && same (comp.get (CompParamId::MbOutput), -2.0f),
                    "the preset moved MIX or OUTPUT");
        ctx.expect (comp.get (CompParamId::AutoMakeup) < 0.5f, "the preset left AUTO MAKEUP on");
        for (const auto id : compStageParams (CompStage::Multiband))
        {
            if (id == CompParamId::MbMix || id == CompParamId::MbOutput) continue;
            const bool highMidLevel = id >= compBandParam (2, CompBandField::Threshold)
                                   && id <= compBandParam (2, CompBandField::Makeup);
            if (highMidLevel) continue;
            ctx.expect (std::abs (comp.get (id) - pop->get (id)) < 1.0e-6f,
                        "the Pop preset left " + symbolOf (id) + " at " + std::to_string (comp.get (id)));
        }
        ctx.expect (! mastering.compEnabled.load(), "the preset engaged the compressor");
    }, [&host, highAtDefault, handleX]
    {
        return ! host.masteringCompCapturesKeyboard() && handleX ("mb_crossover_3") > 0.0f
            && handleX ("mb_crossover_3") < *highAtDefault - 20.0f;
    }, "the panel did not redraw the preset's high split", {}, kSettleBoundMs });
    // A band moved afterwards leaves the picked name showing.
    pushClick (host, ctx, *steps, "mb_bypass_0");
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.masteringCompPreset() == kPopPreset + 1, "a band change took the preset's name away");
    } });

    // A value left half typed when another session opens goes with the old one.
    pushClick (host, ctx, *steps, "mb_threshold_0", kCommand);
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.typeInMasteringComp ("-7"), "the LOW THRESH field took no text"); },
        [&host] { return host.masteringCompCapturesKeyboard(); }, "a command-click on LOW THRESH opened no field" });

    // The bands as the preset left them, saved and loaded back over other values.
    steps->push_back ({ 0, [&ctx, &session, &comp, values, saved, highSaved, handleX]
    {
        *values = comp.values();
        *highSaved = handleX ("mb_crossover_3");
        applySessionDirectory (session, saved.parent_path());
        ctx.expect (SessionSerializer::save (session, saved), "could not save the session with the preset's bands");
        comp.resetToDefaults (CompStage::Multiband);
        comp.set (compBandParam (0, CompBandField::Threshold), -45.0f);
    } });
    steps->push_back ({ 0, [&host, saved] { reopenSavedSession (host, saved); },
                        [highAtDefault, handleAt] { return handleAt ("mb_crossover_3", *highAtDefault); },
                        "the panel did not redraw the values set over the saved ones", {}, kSettleBoundMs });
    steps->push_back ({ 300, [&host] { host.switchToStage (GuiHost::Stage::Mastering); } });
    steps->push_back ({ 0, [&host, &ctx, &comp, values]
    {
        for (const auto id : compStageParams (CompStage::Multiband))
            ctx.expect (same (comp.get (id), (*values)[(std::size_t) id]),
                        "the reloaded session holds " + symbolOf (id) + " at " + std::to_string (comp.get (id))
                            + ", not the saved " + std::to_string ((*values)[(std::size_t) id]));
        ctx.expect (host.masteringCompPreset() == 0, "after another session loaded the picker still names preset "
                                                         + std::to_string (host.masteringCompPreset()));
        ctx.expect (! host.masteringCompCapturesKeyboard(),
                    "the LOW THRESH value typed for the session before is still being typed in the one loaded");
    }, [&host, highSaved, handleAt]
    {
        return host.masteringControl (kPanel, "plot") && handleAt ("mb_crossover_3", *highSaved);
    }, "the panel did not redraw the reloaded high split", {}, kSettleBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #else
    return ScenarioResult::skip ("requires the native UI and the Dusk DSP");
   #endif
}

const ScenarioRegistrar masteringCompControls { Scenario {
    "gui.mastering_comp_controls", { "gui", "mastering", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompControls (host, ctx); }
} };

// With a mix playing through the engaged compressor every band's meter on the
// panel goes down from the display's top; switched out, they all rest at 0 while
// the mix plays on.
std::optional<ScenarioResult> runMasteringCompMeters (GuiHost& host, ScenarioContext& ctx)
{
    if (auto skip = requireNativeMastering (host, ctx)) return skip;
    auto& engine = ctx.engine();
    auto& player = engine.getMasteringPlayer();
    auto& params = ctx.session().mastering();
    if (host.engineDetached() || ! engine.isAudioCallbackRegistered() || engine.getCurrentSampleRate() <= 0.0)
        return ScenarioResult::skip ("requires a running audio callback");
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return ScenarioResult::fail ("the window size is unavailable");
    const auto originalFile = player.getLoadedFile();
    const auto originalSource = params.sourceFile;
    const auto originalPosition = player.getPlayhead();
    keepStage (host, ctx);
    keepComp (ctx, params.comp);
    ctx.keep (params.compEnabled);
    ctx.keep (params.eqEnabled);
    ctx.keep (params.limiterEnabled);
    ctx.cleanup ([&host, &player, &params, originalFile, originalSource, originalPosition, size]
    {
        player.stop();
        drainModals (host);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        params.sourceFile = originalSource;
        host.refreshMasteringSource();
        host.resizeMainWindow (size[0], size[1]);
    });
    if (! host.resizeMainWindow (std::max (1440, size[0]), std::max (900, size[1])))
        return ScenarioResult::fail ("could not widen the mastering workspace");

    const auto source = ctx.tempDir() / "noise.wav";
    std::vector<float> left (480000), right (left.size());
    std::uint32_t noise = 0x2468u;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        noise = noise * 1664525u + 1013904223u;
        left[i] = right[i] = ((float) (noise >> 8) / 8388608.0f - 1.0f) * 0.4f;
    }
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000.0;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    const float* channels[] = { left.data(), right.data() };
    if (writer == nullptr || ! writer->write (channels, 2, (std::int64_t) left.size()) || ! writer->flush())
        return ScenarioResult::fail ("could not write the noise mix");
    writer.reset();

    params.comp.resetToDefaults (CompStage::Multiband);
    for (int band = 0; band < kCompBandCount; ++band)
        params.comp.set (compBandParam (band, CompBandField::Threshold), -45.0f);
    params.eqEnabled.store (false);
    params.limiterEnabled.store (false);
    params.compEnabled.store (true);
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");
    if (! host.loadMasteringFile (source)) return ScenarioResult::fail ("could not load the noise mix");

    const auto depth = [&host] (int band)
    {
        const auto top = host.masteringControl (kPanel, "meter_" + std::to_string (band), 0.0f);
        const auto foot = host.masteringControl (kPanel, "meter_" + std::to_string (band), 1.0f);
        return top && foot ? (*foot)[1] - (*top)[1] : -1.0f;
    };
    const auto all = [depth] (bool moving)
    {
        for (int band = 0; band < kCompBandCount; ++band)
            if (moving ? ! (depth (band) > 1.0f) : ! same (depth (band), 0.0f)) return false;
        return true;
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Play"), "Play was unavailable"); },
        [&host, all] { return host.masteringPanelsOpen() == host.masteringPanelsExpected() && all (false); },
        "the band meters did not start at 0" });
    steps->push_back ({ 0, [&params] { params.compEnabled.store (false); },
                        [all] { return all (true); }, "the band meters did not move with the mix playing" });
    steps->push_back ({ 0, [&ctx, &player]
    {
        ctx.expect (player.isPlaying(), "the mix stopped before the meters came to rest");
        player.stop();
    }, [all] { return all (false); }, "the band meters did not rest at 0 with the compressor out" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringCompMeters { Scenario {
    "gui.mastering_comp_meters", { "gui", "mastering", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 45000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompMeters (host, ctx); }
} };

// Pure black in a PPM the panel's window was read back into; -1 when it does not read.
int blackPixels (const std::filesystem::path& ppm)
{
    std::FILE* in = std::fopen (ppm.u8string().c_str(), "rb");
    if (in == nullptr) return -1;
    int width = 0, height = 0, maxValue = 0;
    int black = -1;
    if (std::fscanf (in, "P6 %d %d %d", &width, &height, &maxValue) == 3 && std::fgetc (in) != EOF
        && width > 0 && height > 0 && maxValue == 255)
    {
        std::vector<unsigned char> rgb ((std::size_t) width * (std::size_t) height * 3);
        if (std::fread (rgb.data(), 1, rgb.size(), in) == rgb.size())
        {
            black = 0;
            for (std::size_t i = 0; i < rgb.size(); i += 3)
                black += rgb[i] == 0 && rgb[i + 1] == 0 && rgb[i + 2] == 0 ? 1 : 0;
        }
    }
    std::fclose (in);
    return black;
}

// The multiband column is one native panel, header and preset picker included:
// read back as the display shows it, no pixel of it is left unpainted, and its
// header and picker are drawn whole.
std::optional<ScenarioResult> runMasteringCompPanelPainted (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    if (! panelReadable (host)) return ScenarioResult::skip (kNoReadBack);
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return ScenarioResult::fail ("the window size is unavailable");
    keepStage (host, ctx);
    ctx.cleanup ([&host, size] { host.resizeMainWindow (size[0], size[1]); });
    if (! host.resizeMainWindow (std::max (1440, size[0]), std::max (900, size[1])))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    host.switchToStage (GuiHost::Stage::Mastering);
    if (host.masteringPanelsOpen() < 0) return ScenarioResult::skip ("this build has no native mastering panels");

    ctx.waitUntil ([&host]
    {
        return host.masteringPanelsOpen() == host.masteringPanelsExpected() && host.panelFrames (kPanel).value_or (0) > 1;
    }, 10000, [&host, &ctx]
    {
        ctx.expect (host.masteringControl (kPanel, "comp_enabled").has_value(),
                    "the MULTIBAND COMP header is not drawn whole");
        ctx.expect (host.masteringControl (kPanel, "comp_preset").has_value(), "the preset picker is not drawn whole");
        const auto ppm = ctx.tempDir() / "mastering-comp.ppm";
        if (ctx.expect (host.capturePanel (kPanel, ppm), "the multiband panel could not be read back"))
        {
            const int black = blackPixels (ppm);
            ctx.expect (black == 0, "the multiband column left " + std::to_string (black) + " pixels unpainted");
        }
        ctx.complete (ctx.verdict());
    },
    "the mastering panels never opened");
    return std::nullopt;
}

const ScenarioRegistrar masteringCompPanelPainted { Scenario {
    "gui.mastering_comp_panel_painted", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompPanelPainted (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
