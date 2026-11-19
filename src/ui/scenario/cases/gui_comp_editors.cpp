#include "../GuiScenarioSupport.h"
#include "../../../dsp/MultiCompStage.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace duskstudio::scenario::gui
{
namespace
{
using Kind = GuiHost::StripKind;

bool shows (GuiHost& host, Kind kind, int index, const std::string& symbol, float value)
{
    const auto shown = host.compEditorShows (kind, index, symbol);
    return shown && std::abs (*shown - value) < 1.0e-4f;
}

std::string shownText (GuiHost& host, Kind kind, int index, const std::string& symbol)
{
    const auto shown = host.compEditorShows (kind, index, symbol);
    return shown ? std::to_string (*shown) : std::string ("nothing");
}

void keepComp (ScenarioContext& ctx, CompParams& comp)
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
        ctx.keep (comp.atom ((CompParamId) i));
}

// COMP on a channel strip opens Multi-Comp 2's own editor for the stage that
// strip runs: the mono instance on a mono track, rebuilt on the stereo one, the
// popup kept up, when the track turns stereo. A compressor switched off still
// opens with its controls live. A mode tab sets the strip's mode as the face's
// mode menu does, a control engages the compressor as a face knob does, BYPASS
// is the status light's switch, and the session's values reach the editor.
// Escape and a click on the dim close it.
std::optional<ScenarioResult> runCompEditorStrip (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (kStripIndex);
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    if (host.compEditorOpen (Kind::Channel, kStripIndex))
        return ScenarioResult::skip ("requires a closed compressor editor");
    if (track.frozen.load()) return ScenarioResult::skip ("requires an unfrozen first track");
    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr) return ScenarioResult::fail ("channel strip is unavailable");

    keepComp (ctx, track.strip.comp);
    ctx.keep (track.strip.compEnabled);
    ctx.keep (track.strip.compMode);
    ctx.keep (track.strip.compModePicked);
    ctx.cleanup ([&host, strip, mode = track.mode.load()]
    {
        host.closeCompEditor (Kind::Channel, kStripIndex);
        drainModals (host);
        strip->restoreTrackMode (mode);
    });
    strip->restoreTrackMode ((int) Track::Mode::Mono);
    track.strip.compEnabled.store (false);
    track.strip.compMode.store (0);
    track.strip.compModePicked.store (false);

    auto& dsp = engine.getChannelStrip (kStripIndex);
    const MultiCompStage* mono = &dsp.getCompressor (false);
    const MultiCompStage* stereo = &dsp.getCompressor (true);
    const auto drawn = [&host] { return host.compEditorDrawn (Kind::Channel, kStripIndex); };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickStripModule (kStripIndex, 1, true, false), "the strip's COMP label is not on screen"); } });
    steps->push_back ({ 0, [&host, &ctx, &track, mono]
    {
        ctx.expect (host.compEditorStage (Kind::Channel, kStripIndex) == mono,
                    "a mono track's editor does not draw the mono compressor");
        ctx.expect (! track.strip.compEnabled.load(), "opening the editor engaged the compressor");
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "daf_bypass", 1.0f),
                    "BYPASS shows " + shownText (host, Kind::Channel, kStripIndex, "daf_bypass")
                        + " for a compressor switched off");
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "mode", 0.0f), "the editor does not show Opto");
        // A mode tab, as the face's mode menu: the mode is picked, nothing engages.
        ctx.expect (host.compEditorEdit (Kind::Channel, kStripIndex, "mode", 1.0f), "the editor took no mode edit");
        ctx.expect (track.strip.compMode.load() == 1, "the FET tab left the strip's mode at "
                                                      + std::to_string (track.strip.compMode.load()));
        ctx.expect (track.strip.compModePicked.load(), "the FET tab did not mark the mode picked");
        ctx.expect (! track.strip.compEnabled.load(), "a mode tab engaged the compressor");
        // Any other control engages it, as a face knob does.
        ctx.expect (host.compEditorEdit (Kind::Channel, kStripIndex, "fet_input", 12.0f), "the editor took no Input edit");
        ctx.expect (std::abs (track.strip.comp.get (CompParamId::FetInput) - 12.0f) < 1.0e-4f,
                    "the Input edit left the session at " + std::to_string (track.strip.comp.get (CompParamId::FetInput)));
        ctx.expect (track.strip.compEnabled.load(), "working Input did not engage the compressor");
    }, drawn, "the strip's compressor editor never drew a frame" });
    steps->push_back ({ 300, [&host, &ctx, &track]
    {
        ctx.expect (host.stripCompLabel (kStripIndex) == "COMP - FET",
                    "the strip face shows '" + host.stripCompLabel (kStripIndex) + "' after the editor's FET tab and edit");
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "daf_bypass", 0.0f), "BYPASS did not follow the engage");
        ctx.expect (host.compEditorEdit (Kind::Channel, kStripIndex, "daf_bypass", 1.0f), "the editor took no BYPASS edit");
        ctx.expect (! track.strip.compEnabled.load(), "BYPASS in the editor left the compressor engaged");
        // The face's status light, MIDI and a control surface write the session.
        track.strip.compEnabled.store (true);
        track.strip.compMode.store (2);
        track.strip.comp.set (CompParamId::VcaOutput, -6.0f);
    } });
    steps->push_back ({ 300, [&host, &ctx, &track]
    {
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "daf_bypass", 0.0f), "the status light did not reach BYPASS");
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "mode", 2.0f),
                    "a face mode change shows " + shownText (host, Kind::Channel, kStripIndex, "mode") + " in the editor");
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "vca_output", -6.0f),
                    "a session Output change shows " + shownText (host, Kind::Channel, kStripIndex, "vca_output"));
        ctx.expect (host.stripCompLabel (kStripIndex) == "COMP - VCA",
                    "the strip face shows '" + host.stripCompLabel (kStripIndex) + "' for VCA");
        // As an undo of a mode change or a session load turns it, with no
        // dialog opened over the editor.
        track.mode.store ((int) Track::Mode::Stereo);
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (host.compEditorOpen (Kind::Channel, kStripIndex), "the width change closed the editor");
        ctx.expect (shows (host, Kind::Channel, kStripIndex, "mode", 2.0f), "the rebuilt editor lost the mode");
        ctx.expect (host.pressPeerKey ("escape"), "the window did not handle Escape");
    }, [&host, stereo, drawn]
    {
        return ! host.compEditorOpen (Kind::Channel, kStripIndex)
            || (host.compEditorStage (Kind::Channel, kStripIndex) == stereo && drawn());
    }, "the editor was not rebuilt on the stereo compressor" });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (! host.compEditorOpen (Kind::Channel, kStripIndex), "Escape left the compressor editor open");
        ctx.expect (host.clickStripModule (kStripIndex, 1, true, false), "the strip's COMP label is not on screen");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickCompEditorDim(), "the dim behind the compressor editor took no click"); },
      drawn, "the strip's compressor editor did not reopen" });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.compEditorOpen (Kind::Channel, kStripIndex); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "a click on the dim left the compressor editor open");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar compEditorStrip { Scenario {
    "gui.comp_editor_strip", { "gui", "strip", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompEditorStrip (host, ctx); }
} };

// COMP on a bus and on the master opens Multi-Comp 2's own editor for that
// compressor. Its knobs leave the engage switch alone, as the face's do; its
// threshold engages, as the face's handle does; BYPASS is the status light's
// switch. One compressor editor at a time: opening another closes the first.
std::optional<ScenarioResult> runCompEditorBusMaster (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr int kBus = 0;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto& bus = session.bus (kBus).strip;
    auto& master = session.master();
    keepComp (ctx, bus.comp);
    keepComp (ctx, master.comp);
    ctx.keep (bus.compEnabled);
    ctx.keep (master.compEnabled);
    ctx.keep (session.track (kStripIndex).strip.compEnabled);
    ctx.cleanup ([&host]
    {
        host.closeCompEditor (Kind::Bus, kBus);
        host.closeCompEditor (Kind::Master, 0);
        host.closeCompEditor (Kind::Channel, kStripIndex);
        drainModals (host);
    });
    bus.compEnabled.store (false);
    master.compEnabled.store (false);

    const MultiCompStage* busStage = &engine.getBusStrip (kBus).getCompressor();
    const MultiCompStage* masterStage = &engine.getMasterBus().getCompressor();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickStripControl (Kind::Bus, kBus, "comp", 1, false), "the bus COMP label is not on screen"); } });
    steps->push_back ({ 0, [&host, &ctx, &bus, busStage]
    {
        ctx.expect (host.compEditorStage (Kind::Bus, kBus) == busStage, "the bus editor does not draw the bus compressor");
        ctx.expect (host.compEditorEdit (Kind::Bus, kBus, "bus_ratio", 2.0f), "the bus editor took no Ratio edit");
        ctx.expect (std::abs (bus.comp.get (CompParamId::BusRatio) - 2.0f) < 1.0e-4f, "the Ratio edit missed the bus session");
        ctx.expect (! bus.compEnabled.load(), "a bus knob in the editor engaged the compressor");
        ctx.expect (host.compEditorEdit (Kind::Bus, kBus, "bus_threshold", -10.0f), "the bus editor took no Threshold edit");
        ctx.expect (std::abs (bus.comp.get (CompParamId::BusThreshold) + 10.0f) < 1.0e-4f,
                    "the Threshold edit missed the bus session");
        ctx.expect (bus.compEnabled.load(), "the bus editor's Threshold did not engage the compressor");
        bus.comp.set (CompParamId::BusMakeup, 6.0f);
    }, [&host] { return host.compEditorDrawn (Kind::Bus, kBus); }, "the bus compressor editor never drew a frame" });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (shows (host, Kind::Bus, kBus, "bus_makeup", 6.0f),
                    "a session Makeup change shows " + shownText (host, Kind::Bus, kBus, "bus_makeup"));
        ctx.expect (shows (host, Kind::Bus, kBus, "daf_bypass", 0.0f), "BYPASS did not follow the bus engage");
        ctx.expect (host.pressPeerKey ("escape"), "the window did not handle Escape");
    } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (! host.compEditorOpen (Kind::Bus, kBus), "Escape left the bus compressor editor open");
        ctx.expect (host.clickStripControl (Kind::Master, 0, "comp", 1, false), "the master COMP label is not on screen");
    } });
    steps->push_back ({ 0, [&host, &ctx, &master, masterStage]
    {
        ctx.expect (host.compEditorStage (Kind::Master, 0) == masterStage,
                    "the master editor does not draw the master compressor");
        ctx.expect (host.compEditorEdit (Kind::Master, 0, "bus_attack", 3.0f), "the master editor took no Attack edit");
        ctx.expect (std::abs (master.comp.get (CompParamId::BusAttack) - 3.0f) < 1.0e-4f,
                    "the Attack edit missed the master session");
        ctx.expect (! master.compEnabled.load(), "a master knob in the editor engaged the compressor");
        ctx.expect (host.compEditorEdit (Kind::Master, 0, "bus_threshold", -6.0f), "the master editor took no Threshold edit");
        ctx.expect (master.compEnabled.load(), "the master editor's Threshold did not engage the compressor");
        ctx.expect (host.compEditorEdit (Kind::Master, 0, "daf_bypass", 1.0f), "the master editor took no BYPASS edit");
        ctx.expect (! master.compEnabled.load(), "BYPASS in the master editor left the compressor engaged");
        // Another compressor's editor takes the window's place.
        host.openCompEditor (Kind::Bus, kBus);
    }, [&host] { return host.compEditorDrawn (Kind::Master, 0); }, "the master compressor editor never drew a frame" });
    steps->push_back ({ 0, [&host] { host.openCompEditor (Kind::Channel, kStripIndex); },
        [&host] { return host.compEditorDrawn (Kind::Bus, kBus) && ! host.compEditorOpen (Kind::Master, 0); },
        "opening the bus editor did not take the master's place" });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickCompEditorDim(), "the dim behind the compressor editor took no click"); },
        [&host] { return host.compEditorDrawn (Kind::Channel, kStripIndex) && ! host.compEditorOpen (Kind::Bus, kBus); },
        "opening the strip editor did not take the bus's place" });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.compEditorOpen (Kind::Channel, kStripIndex); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "a click on the dim left the compressor editor open");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar compEditorBusMaster { Scenario {
    "gui.comp_editor_bus_master", { "gui", "master", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompEditorBusMaster (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
