#include "../GuiScenarioSupport.h"
#include "../../../dsp/MultiCompStage.h"
#include "../../../dsp/MultibandCompPresets.h"

#include <algorithm>
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

std::string symbolOf (CompParamId id) { return compParamSpec (id).symbol; }

// COMP on a channel strip opens Multi-Comp 2's own editor for the stage that
// strip runs: the instance of the width it last ran its compressor at, or,
// before it has run, the one the track's mode calls for, so the mono instance on
// a mono track, rebuilt on the stereo one, the popup kept up, when the track
// turns stereo. A compressor switched off still
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

    // A silent strip runs no block during the case, so the width it last ran,
    // if any, is from before it: with none, the editor follows the mode.
    auto& dsp = engine.getChannelStrip (kStripIndex);
    const auto expected = [&dsp] (bool trackStereo) -> const MultiCompStage*
    {
        const int ran = dsp.compressorRunWidth();
        return &dsp.getCompressor (ran != 0 ? ran == 2 : trackStereo);
    };
    const int ran = dsp.compressorRunWidth();
    ctx.note (ran == 0 ? std::string ("the strip has not run since it was prepared")
                       : "the strip last ran its compressor " + std::string (ran == 2 ? "stereo" : "mono"));
    // Drawn and handed its values: on native Wayland the first frame can come
    // before the host's first push.
    const auto drawn = [&host]
    {
        return host.compEditorDrawn (Kind::Channel, kStripIndex)
            && host.compEditorShows (Kind::Channel, kStripIndex, "mode").has_value();
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickStripModule (kStripIndex, 1, true, false), "the strip's COMP label is not on screen"); } });
    steps->push_back ({ 0, [&host, &ctx, &track, expected]
    {
        ctx.expect (host.compEditorStage (Kind::Channel, kStripIndex) == expected (false),
                    "a mono track's editor does not draw the compressor the strip runs");
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
    }, [&host, expected, drawn]
    {
        return ! host.compEditorOpen (Kind::Channel, kStripIndex)
            || (host.compEditorStage (Kind::Channel, kStripIndex) == expected (true) && drawn());
    }, "the editor did not follow the strip onto the stereo track's compressor" });
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

int paramIndex (MultiCompStage& stage, const std::string& symbol)
{
    const auto& params = stage.plugin().params();
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i].symbol == symbol) return (int) i;
    return -1;
}

// A value a third of the parameter's range or more from `now`.
float awayFrom (CompParamId id, float now)
{
    const auto& spec = compParamSpec (id);
    const float range = spec.hostMax - spec.hostMin;
    const float mid = spec.hostMin + 0.5f * range;
    return clampCompParam (id, now < mid ? spec.hostMax - 0.1f * range : spec.hostMin + 0.1f * range);
}

// Multi-Comp 2's editor reads every control back from the plug-in as it draws
// its first frame, and a strip or bus that is silent with its compressor
// bypassed runs no block to hand the plug-in the session's values. Opened
// there, the editor still finds them, BYPASS showing the compressor out, both
// in the plug-in it reads and in what its host hands it. Read the moment the
// editor is up, before it has drawn or been pumped.
std::optional<ScenarioResult> runCompEditorOpensOnSession (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr int kBus = 0;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (kStripIndex);
    auto& bus = session.bus (kBus).strip;
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    if (track.frozen.load()) return ScenarioResult::skip ("requires an unfrozen first track");
    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr) return ScenarioResult::fail ("channel strip is unavailable");

    keepComp (ctx, track.strip.comp);
    keepComp (ctx, bus.comp);
    ctx.keep (track.strip.compEnabled);
    ctx.keep (track.strip.compMode);
    ctx.keep (track.strip.compModePicked);
    ctx.keep (bus.compEnabled);
    ctx.cleanup ([&host, strip, mode = track.mode.load()]
    {
        host.closeCompEditor (Kind::Channel, kStripIndex);
        host.closeCompEditor (Kind::Bus, kBus);
        drainModals (host);
        strip->restoreTrackMode (mode);
    });
    strip->restoreTrackMode ((int) Track::Mode::Mono);
    track.strip.compEnabled.store (false);
    bus.compEnabled.store (false);

    // Opens one compressor's editor on values its plug-in does not hold yet
    // and reads what the editor finds there. `mode` is the strip's, or -1.
    const auto opensOn = [&host, &ctx] (Kind kind, int index, MultiCompStage& stage, CompParams& comp,
                                        const std::vector<CompParamId>& ids, int mode, const std::string& name)
    {
        std::vector<std::pair<int, float>> wanted;
        for (const auto id : ids)
        {
            const int at = paramIndex (stage, compParamSpec (id).symbol);
            if (! ctx.expect (at >= 0, name + "'s plug-in lacks " + symbolOf (id))) return;
            const float value = awayFrom (id, stage.editorReading (at));
            comp.set (id, value);
            wanted.push_back ({ at, value });
        }
        host.openCompEditor (kind, index);
        if (! ctx.expect (host.compEditorOpen (kind, index), "the " + name + " compressor editor did not open"))
            return;
        ctx.expect (host.compEditorStage (kind, index) == &stage,
                    "the " + name + " editor does not draw the compressor the case set up");
        for (std::size_t i = 0; i < wanted.size(); ++i)
        {
            const float read = stage.editorReading (wanted[i].first);
            ctx.expect (std::abs (read - wanted[i].second) < 1.0e-3f,
                        "the " + name + " editor opened on " + symbolOf (ids[i]) + " " + std::to_string (read)
                            + " from the plug-in, not the session's " + std::to_string (wanted[i].second));
        }
        if (mode >= 0)
        {
            const int at = paramIndex (stage, "mode");
            const float read = at >= 0 ? stage.editorReading (at) : -1.0f;
            ctx.expect (std::lround (read) == mode, "the " + name + " editor opened on mode "
                                                        + std::to_string (read) + ", not the strip's "
                                                        + std::to_string (mode));
        }
        const int bypass = paramIndex (stage, "daf_bypass");
        ctx.expect (bypass >= 0 && stage.editorReading (bypass) > 0.5f,
                    "the " + name + " editor reads BYPASS in from the plug-in for a compressor switched out");
        const auto handed = host.compEditorShows (kind, index, "daf_bypass");
        ctx.expect (handed && *handed > 0.5f,
                    "the " + name + " editor was handed BYPASS "
                        + (handed ? std::to_string (*handed) : std::string ("nothing")) + " as it opened");
        host.closeCompEditor (kind, index);
    };

    auto& stripStage = engine.getChannelStrip (kStripIndex).getEditorCompressor (false);
    auto& busStage = engine.getBusStrip (kBus).getCompressor();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&track, &stripStage, opensOn]
    {
        // A mode the plug-in is not in either.
        const int modeAt = paramIndex (stripStage, "mode");
        const int held = modeAt >= 0 ? (int) std::lround (stripStage.editorReading (modeAt)) : 0;
        const int mode = (std::clamp (held, 0, 2) + 1) % 3;
        track.strip.compMode.store (mode);
        opensOn (Kind::Channel, kStripIndex, stripStage, track.strip.comp,
                 { CompParamId::FetInput, CompParamId::FetRelease, CompParamId::VcaOutput, CompParamId::OptoGain },
                 mode, "strip");
    } });
    steps->push_back ({ 100, [&bus, &busStage, opensOn]
    {
        opensOn (Kind::Bus, kBus, busStage, bus.comp,
                 { CompParamId::BusMakeup, CompParamId::BusThreshold, CompParamId::BusMix }, -1, "bus");
    }, [&host] { return ! host.compEditorOpen (Kind::Channel, kStripIndex); },
       "the strip compressor editor did not close" });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.compEditorOpen (Kind::Bus, kBus); }, kSettleBoundMs,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "the bus compressor editor did not close");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar compEditorOpensOnSession { Scenario {
    "gui.comp_editor_opens_on_session", { "gui", "strip", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompEditorOpensOnSession (host, ctx); }
} };

// COMP on a bus and on the master opens Multi-Comp 2's own editor for that
// compressor. Its knobs leave the engage switch alone, as the face's do, and
// with the master's compressor out its plug-in still holds what they set, which
// the editor reads back; its threshold engages, as the face's handle does;
// BYPASS is the status light's switch. One compressor editor at a time:
// opening another closes the first.
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
    int masterAttackIndex = -1;
    {
        const auto& params = engine.getMasterBus().getCompressor().plugin().params();
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].symbol == "bus_attack") masterAttackIndex = (int) i;
    }
    if (masterAttackIndex < 0) return ScenarioResult::fail ("the master's plug-in lacks bus_attack");

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
    }, [&host] { return host.compEditorDrawn (Kind::Master, 0); }, "the master compressor editor never drew a frame" });
    steps->push_back ({ 300, [&host, &ctx, &master, masterStage, masterAttackIndex]
    {
        // The editor reads every control back from the plug-in when it opens:
        // with the compressor out, the plug-in still holds the session's.
        const float attack = masterStage->editorReading (masterAttackIndex);
        ctx.expect (std::abs (attack - 3.0f) < 1.0e-4f,
                    "with the master compressor out its plug-in holds Attack at " + std::to_string (attack));
        ctx.expect (host.compEditorEdit (Kind::Master, 0, "bus_threshold", -6.0f), "the master editor took no Threshold edit");
        ctx.expect (master.compEnabled.load(), "the master editor's Threshold did not engage the compressor");
        ctx.expect (host.compEditorEdit (Kind::Master, 0, "daf_bypass", 1.0f), "the master editor took no BYPASS edit");
        ctx.expect (! master.compEnabled.load(), "BYPASS in the master editor left the compressor engaged");
        // Another compressor's editor takes the window's place.
        host.openCompEditor (Kind::Bus, kBus);
    } });
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

// The MULTIBAND COMP label on the mastering page opens Multi-Comp 2's own
// Multiband face over the dimmed window, drawing the mastering chain's
// compressor at the face's design size where the window holds it. A band
// control lands in the session and leaves the compressor as it was, and with
// the compressor out its plug-in still holds what the face set, which the face
// reads back; BYPASS is the header's status light, both ways; a preset picked
// on the page shows on the face. Escape and a click on the dim close it.
std::optional<ScenarioResult> runMasteringCompEditor (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI || ! DUSKSTUDIO_HAS_DUSK_DSP
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI and the Dusk DSP");
   #else
    static constexpr int kPopPreset = 1;
    auto& engine = ctx.engine();
    auto& mastering = ctx.session().mastering();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and mastering player and no modal");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return ScenarioResult::fail ("the window size is unavailable");

    keepComp (ctx, mastering.comp);
    ctx.keep (mastering.compEnabled);
    const auto originalStage = engine.getStage();
    ctx.cleanup ([&host, size, originalStage]
    {
        host.closeMasteringCompEditor();
        drainModals (host);
        host.resizeMainWindow (size[0], size[1]);
        host.switchToStage (guiStage (originalStage));
    });
    if (! host.resizeMainWindow (std::max (1440, size[0]), std::max (900, size[1])))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    host.switchToStage (GuiHost::Stage::Mastering);
    mastering.compEnabled.store (false);

    auto& compressor = engine.getMasteringChain().getCompressor();
    const MultiCompStage* stage = &compressor;
    const auto design = std::vector<int> { (int) compressor.plugin().editorWidth(),
                                           (int) compressor.plugin().editorHeight() };
    // Pop, as the picker writes it.
    auto pop = std::make_shared<CompParams>();
    if (! mbpresets::apply (kPopPreset, compressor, *pop))
        return ScenarioResult::fail ("the Pop preset did not convert");

    // Drawn and handed its values: on native Wayland the first frame can come
    // before the host's first push.
    const auto drawn = [&host]
    {
        return host.masteringCompEditorDrawn() && host.masteringCompEditorShows ("mode").has_value();
    };
    const auto shows = [&host] (const char* symbol, float value)
    {
        const auto shown = host.masteringCompEditorShows (symbol);
        return shown && std::abs (*shown - value) < 1.0e-4f;
    };
    const auto band1Threshold = compBandParam (1, CompBandField::Threshold);
    // The face reads the crossovers back from the plug-in itself every frame,
    // not from the values the host hands it, so the plug-in is what it shows.
    const auto indexOf = [&compressor] (const char* symbol)
    {
        const auto& params = compressor.plugin().params();
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].symbol == symbol) return (int) i;
        return -1;
    };
    const int crossover1Index = indexOf ("mb_crossover_1");
    const int band1ThresholdIndex = indexOf ("mb_threshold_1");
    if (crossover1Index < 0 || band1ThresholdIndex < 0)
        return ScenarioResult::fail ("the Multiband plug-in lacks a crossover or band threshold");
    static constexpr float kCrossover1 = 0.6f;

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickMasteringCompHeader (true), "the MULTIBAND COMP label is not on screen"); } });
    steps->push_back ({ 0, [&host, &ctx, &mastering, stage, design, shows, band1Threshold]
    {
        ctx.expect (host.masteringCompEditorStage() == stage, "the face does not draw the mastering compressor");
        const auto bounds = host.masteringCompEditorBounds();
        ctx.expect (bounds.size() == 4 && bounds[2] == design[0] && bounds[3] == design[1],
                    "the face opened at " + (bounds.size() == 4 ? std::to_string (bounds[2]) + " x "
                                                                    + std::to_string (bounds[3]) : std::string ("nothing"))
                        + ", not its " + std::to_string (design[0]) + " x " + std::to_string (design[1]));
        ctx.expect (shows ("mode", 7.0f), "the face does not show the Multiband mode");
        ctx.expect (shows ("daf_bypass", 1.0f), "BYPASS does not show a compressor switched off");
        ctx.expect (host.masteringCompEditorEdit ("mb_threshold_1", -33.0f), "the face took no band threshold edit");
        ctx.expect (std::abs (mastering.comp.get (band1Threshold) + 33.0f) < 1.0e-4f,
                    "the threshold edit left the session at " + std::to_string (mastering.comp.get (band1Threshold)));
        ctx.expect (host.masteringCompEditorEdit ("mb_crossover_1", kCrossover1), "the face took no crossover edit");
        ctx.expect (! mastering.compEnabled.load(), "a band control engaged the compressor");
    }, drawn, "the Multiband face never drew a frame" });
    steps->push_back ({ 300, [&host, &ctx, &mastering, stage, crossover1Index, band1ThresholdIndex]
    {
        // Still out: the plug-in has not run, yet it holds what the face set.
        ctx.expect (! mastering.compEnabled.load(), "the compressor engaged before BYPASS was touched");
        const float crossover = stage->editorReading (crossover1Index);
        ctx.expect (std::abs (crossover - kCrossover1) < 2.0e-3f,
                    "with the compressor out the face reads its low split back as " + std::to_string (crossover)
                        + ", not the " + std::to_string (kCrossover1) + " it set");
        const float threshold = stage->editorReading (band1ThresholdIndex);
        ctx.expect (std::abs (threshold + 33.0f) < 1.0e-3f,
                    "with the compressor out the plug-in holds the band threshold at " + std::to_string (threshold));
        ctx.expect (host.masteringCompEditorEdit ("daf_bypass", 0.0f), "the face took no BYPASS edit");
        ctx.expect (mastering.compEnabled.load(), "BYPASS off on the face did not engage the compressor");
    } });
    steps->push_back ({ 300, [&ctx, &mastering, shows]
    {
        ctx.expect (shows ("daf_bypass", 0.0f), "BYPASS did not follow the engage");
        // What the header's status light writes.
        mastering.compEnabled.store (false);
    } });
    steps->push_back ({ 300, [&host, &ctx, shows]
    {
        ctx.expect (shows ("daf_bypass", 1.0f), "the status light did not reach BYPASS");
        ctx.expect (host.pressPeerKey ("escape"), "the window did not handle Escape");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.clickMasteringCompHeader (false), "the MULTIBAND COMP status light is not on screen"); },
        [&host] { return ! host.masteringCompEditorOpen(); }, "Escape left the Multiband face open" });
    steps->push_back ({ 100, [&host, &ctx, &mastering]
    {
        ctx.expect (mastering.compEnabled.load(), "the status light did not engage the compressor");
        ctx.expect (host.clickMasteringCompPreset(), "the preset picker is not on screen");
    } });
    steps->push_back ({ 100, [&host, &ctx]
    {
        ctx.expect (host.pressPeerKey ("home", 0), "the preset list took no Home");
        ctx.expect (host.pressPeerKey ("cursor down", 0), "the preset list took no Down");
        ctx.expect (host.pressPeerKey ("cursor down", 0), "the preset list took no second Down");
        ctx.expect (host.pressPeerKey ("return", '\r'), "the preset list took no Return");
    }, [&host] { return ! host.modalStackEmpty() && host.modalHasKeyboardFocus(); },
        "the preset picker did not take the keyboard" });
    steps->push_back ({ 200, [&host, &ctx, &mastering, pop]
    {
        for (const auto id : compStageParams (CompStage::Multiband))
        {
            if (id == CompParamId::MbMix || id == CompParamId::MbOutput) continue;
            const bool highMidLevel = id >= compBandParam (2, CompBandField::Threshold)
                                   && id <= compBandParam (2, CompBandField::Makeup);
            if (highMidLevel) continue;
            ctx.expect (std::abs (mastering.comp.get (id) - pop->get (id)) < 1.0e-6f,
                        std::string ("the Pop preset left ") + compParamSpec (id).symbol + " at "
                            + std::to_string (mastering.comp.get (id)));
        }
        ctx.expect (host.clickMasteringCompHeader (true), "the MULTIBAND COMP label is not on screen");
    }, [&host] { return host.modalStackEmpty(); }, "the preset picker stayed open" });
    steps->push_back ({ 0, [&host, &ctx, &mastering, shows]
    {
        const float split = mastering.comp.get (CompParamId::MbCrossover3);
        ctx.expect (shows ("mb_crossover_3", split), "the face does not show the preset's high split");
        ctx.expect (shows ("mb_enabled_2", 0.0f), "the face does not show high-mid out");
        ctx.expect (shows ("daf_bypass", 0.0f), "BYPASS does not show the engage the status light made");
        ctx.expect (host.clickCompEditorDim(), "the dim behind the Multiband face took no click");
    }, drawn, "the Multiband face did not reopen" });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.masteringCompEditorOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "a click on the dim left the Multiband face open");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar masteringCompEditor { Scenario {
    "gui.mastering_comp_editor", { "gui", "mastering", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompEditor (host, ctx); }
} };

// A press anywhere on a channel strip selects its track and focuses the strip.
void keepTrackFocus (GuiHost& host, ScenarioContext& ctx)
{
    ctx.cleanup ([&host, track = host.tapeSelectedTrack(), strip = host.consoleFocusedStrip()]
                 { host.restoreTrackFocus (track, strip); });
}

// Every value a strip stores away from its default, the handle's and knobs'
// included, so a reset that misses one shows.
void moveEveryStripValue (CompParams& comp)
{
    for (const auto id : compStageParams (CompStage::Strip))
    {
        const auto& spec = compParamSpec (id);
        comp.set (id, spec.hostMax - spec.hostDefault > 1.0e-6f ? spec.hostMax : spec.hostMin);
    }
}

void expectStripDefaults (ScenarioContext& ctx, const CompParams& comp, const std::string& when)
{
    for (const auto id : compStageParams (CompStage::Strip))
        ctx.expect (std::abs (comp.get (id) - compParamSpec (id).hostDefault) < 1.0e-6f,
                    when + " left " + symbolOf (id) + " at " + std::to_string (comp.get (id))
                        + ", not the plug-in's " + std::to_string (compParamSpec (id).hostDefault));
}

// Reset comp in a channel strip's COMP menu returns every strip compressor
// value, in all three modes, to the plug-in's default, and leaves the mode, the
// mode the user picked and the status light as they were, engaged or not.
std::optional<ScenarioResult> runCompResetStrip (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (kStripIndex);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (track.frozen.load()) return ScenarioResult::skip ("requires an unfrozen first track");
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto& strip = track.strip;
    keepComp (ctx, strip.comp);
    ctx.keep (strip.compEnabled);
    ctx.keep (strip.compMode);
    ctx.keep (strip.compModePicked);
    const bool originalCompact = host.setStripCompact (kStripIndex, false);
    ctx.cleanup ([&host, originalCompact]
    {
        drainModals (host);
        host.setStripCompact (kStripIndex, originalCompact);
    });
    keepTrackFocus (host, ctx);
    strip.comp.resetToDefaults (CompStage::Strip);
    strip.compEnabled.store (true);
    strip.compModePicked.store (true);
    strip.compMode.store (0);

    // One knob a mode on the face, then every other value as the editor, MIDI
    // or a loaded session leaves it.
    const auto moved = [&host, &ctx, &strip] (CompParamId id)
    {
        ctx.expect (host.dragStripComp (Kind::Channel, kStripIndex, symbolOf (id), 60),
                    "the strip's " + symbolOf (id) + " knob is not on screen");
        ctx.expect (std::abs (strip.comp.get (id) - compParamSpec (id).hostDefault) > 1.0e-6f,
                    "a drag on the strip's " + symbolOf (id) + " knob left it at its default");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [moved, &strip]
    {
        moved (CompParamId::OptoGain);
        strip.compMode.store (2);
    } });
    steps->push_back ({ 300, [moved, &strip]
    {
        moved (CompParamId::VcaOutput);
        strip.compMode.store (1);
    } });
    steps->push_back ({ 300, [&host, &ctx, &strip, moved]
    {
        moved (CompParamId::FetAttack);
        CompParams faceMoves;
        faceMoves.copyFrom (strip.comp);
        moveEveryStripValue (strip.comp);
        for (const auto id : { CompParamId::OptoGain, CompParamId::VcaOutput, CompParamId::FetAttack })
            strip.comp.set (id, faceMoves.get (id));
        ctx.expect (strip.compEnabled.load() && strip.compMode.load() == 1 && strip.compModePicked.load(),
                    "the strip is not an engaged FET strip with its mode picked before the reset");
        ctx.expect (host.clickStripModule (kStripIndex, 1, true, true), "the strip's COMP header took no right-click");
    } });
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.clickContextMenuItem ("Reset comp"), "the COMP menu has no Reset comp"); },
        [&host] { return ! host.contextMenuItems().empty(); }, "the COMP menu did not open" });
    steps->push_back ({ 300, [&host, &ctx, &strip]
    {
        expectStripDefaults (ctx, strip.comp, "Reset comp on an engaged FET strip");
        ctx.expect (strip.compEnabled.load(), "Reset comp bypassed the compressor");
        ctx.expect (strip.compMode.load() == 1, "Reset comp moved the mode to " + std::to_string (strip.compMode.load()));
        ctx.expect (strip.compModePicked.load(), "Reset comp cleared the mode the user picked");
        ctx.expect (host.stripCompLabel (kStripIndex) == "COMP - FET",
                    "the strip face shows '" + host.stripCompLabel (kStripIndex) + "' after Reset comp");
        // And from the other side: a bypassed VCA strip whose mode was never picked.
        moveEveryStripValue (strip.comp);
        strip.compEnabled.store (false);
        strip.compMode.store (2);
        strip.compModePicked.store (false);
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickStripModule (kStripIndex, 1, true, true), "the strip's COMP header took no second right-click"); } });
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.clickContextMenuItem ("Reset comp"), "the COMP menu has no Reset comp"); },
        [&host] { return ! host.contextMenuItems().empty(); }, "the COMP menu did not open again" });
    steps->push_back ({ 300, [&ctx, &strip]
    {
        expectStripDefaults (ctx, strip.comp, "Reset comp on a bypassed VCA strip");
        ctx.expect (! strip.compEnabled.load(), "Reset comp engaged the compressor");
        ctx.expect (strip.compMode.load() == 2, "Reset comp moved the VCA mode to " + std::to_string (strip.compMode.load()));
        ctx.expect (! strip.compModePicked.load(), "Reset comp marked the mode picked");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar compResetStrip { Scenario {
    "gui.comp_reset_strip", { "gui", "strip", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompResetStrip (host, ctx); }
} };

// On a bypassed channel strip, dragging the gain-reduction handle, turning a
// knob or switching LIMIT on engages the compressor and switching LIMIT off does
// not. A double-click on the handle returns only that control to its default
// and leaves the compressor engaged or bypassed as it was.
std::optional<ScenarioResult> runCompStripFaceEngages (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (kStripIndex);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (track.frozen.load()) return ScenarioResult::skip ("requires an unfrozen first track");
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto& strip = track.strip;
    keepComp (ctx, strip.comp);
    ctx.keep (strip.compEnabled);
    ctx.keep (strip.compMode);
    ctx.keep (strip.compModePicked);
    const bool originalCompact = host.setStripCompact (kStripIndex, false);
    ctx.cleanup ([&host, originalCompact]
    {
        drainModals (host);
        host.setStripCompact (kStripIndex, originalCompact);
    });
    keepTrackFocus (host, ctx);
    strip.comp.resetToDefaults (CompStage::Strip);
    strip.compEnabled.store (false);
    strip.compMode.store (1);

    const auto value = [&strip] (CompParamId id) { return strip.comp.get (id); };
    const auto moved = [&strip] (CompParamId id)
    { return std::abs (strip.comp.get (id) - compParamSpec (id).hostDefault) > 1.0e-6f; };
    // Every value but the handle's, as the double-click finds them.
    auto others = std::make_shared<CompParams>();
    const auto othersKept = [&ctx, &strip, others] (const std::string& when)
    {
        for (const auto id : compStageParams (CompStage::Strip))
            if (id != CompParamId::OptoPeakReduction)
                ctx.expect (std::abs (strip.comp.get (id) - others->get (id)) < 1.0e-6f,
                            when + " moved " + symbolOf (id) + " to " + std::to_string (strip.comp.get (id)));
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.dragStripComp (Kind::Channel, kStripIndex, "handle", 30), "the strip's GR handle is not on screen"); } });
    steps->push_back ({ 100, [&ctx, &strip, moved]
    {
        ctx.expect (moved (CompParamId::FetInput), "dragging the FET strip's handle left INPUT at its default");
        ctx.expect (strip.compEnabled.load(), "dragging the handle did not engage the compressor");
        strip.compEnabled.store (false);
    } });
    steps->push_back ({ 600, [&host, &ctx]
    { ctx.expect (host.dragStripComp (Kind::Channel, kStripIndex, "fet_attack", 60), "the strip's FET ATK knob is not on screen"); } });
    steps->push_back ({ 100, [&ctx, &strip, moved]
    {
        ctx.expect (moved (CompParamId::FetAttack), "dragging the FET ATK knob left it at its default");
        ctx.expect (strip.compEnabled.load(), "turning a FET knob did not engage the compressor");
        strip.compEnabled.store (false);
        strip.compMode.store (0);
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickStripComp (Kind::Channel, kStripIndex, "opto_limit", 1), "the strip's LIMIT switch is not on screen"); } });
    steps->push_back ({ 100, [&ctx, &strip, value]
    {
        ctx.expect (value (CompParamId::OptoLimit) > 0.5f, "a click on LIMIT did not switch it on");
        ctx.expect (strip.compEnabled.load(), "switching LIMIT on did not engage the compressor");
        strip.compEnabled.store (false);
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickStripComp (Kind::Channel, kStripIndex, "opto_limit", 1), "the strip's LIMIT switch is not on screen"); } });
    steps->push_back ({ 100, [&ctx, &strip, value, others]
    {
        ctx.expect (value (CompParamId::OptoLimit) < 0.5f, "a second click on LIMIT did not switch it off");
        ctx.expect (! strip.compEnabled.load(), "switching LIMIT off engaged the compressor");
        strip.compEnabled.store (true);
        strip.comp.set (CompParamId::OptoPeakReduction, 40.0f);
        others->copyFrom (strip.comp);
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickStripComp (Kind::Channel, kStripIndex, "handle", 2), "the strip's GR handle is not on screen"); } });
    steps->push_back ({ 300, [&ctx, &strip, moved, othersKept, others]
    {
        ctx.expect (! moved (CompParamId::OptoPeakReduction), "a double-click on the engaged strip's handle left PEAK REDUCTION at "
                                                               + std::to_string (strip.comp.get (CompParamId::OptoPeakReduction)));
        othersKept ("a double-click on the engaged strip's handle");
        ctx.expect (strip.compEnabled.load(), "a double-click on the handle bypassed the compressor");
        strip.compEnabled.store (false);
        strip.comp.set (CompParamId::OptoPeakReduction, 40.0f);
        others->copyFrom (strip.comp);
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.clickStripComp (Kind::Channel, kStripIndex, "handle", 2), "the strip's GR handle is not on screen"); } });
    steps->push_back ({ 300, [&ctx, &strip, moved, othersKept]
    {
        ctx.expect (! moved (CompParamId::OptoPeakReduction), "a double-click on the bypassed strip's handle left PEAK REDUCTION at "
                                                               + std::to_string (strip.comp.get (CompParamId::OptoPeakReduction)));
        othersKept ("a double-click on the bypassed strip's handle");
        ctx.expect (! strip.compEnabled.load(), "a double-click on the bypassed strip's handle engaged the compressor");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar compStripFaceEngages { Scenario {
    "gui.comp_strip_face_engages", { "gui", "strip", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompStripFaceEngages (host, ctx); }
} };

// On a bypassed bus and on the bypassed master, RAT, ATK, REL and MAK on the
// strip face move their values and leave the compressor off; dragging the
// gain-reduction handle turns it on, and a double-click on the handle of the
// bypassed compressor returns only the threshold to its default.
std::optional<ScenarioResult> runCompBusMasterFace (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr int kBus = 0;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return ScenarioResult::fail ("the window size is unavailable");
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto& bus = session.bus (kBus).strip;
    auto& master = session.master();
    keepComp (ctx, bus.comp);
    keepComp (ctx, master.comp);
    ctx.keep (bus.compEnabled);
    ctx.keep (master.compEnabled);
    ctx.cleanup ([&host, size] { host.resizeMainWindow (size[0], size[1]); });
    // Tall enough that neither face is compact, which hides the knobs.
    if (! host.resizeMainWindow (std::max (1440, size[0]), std::max (1000, size[1])))
        return ScenarioResult::fail ("could not enlarge the window");
    bus.comp.resetToDefaults (CompStage::Bus);
    master.comp.resetToDefaults (CompStage::Bus);
    bus.compEnabled.store (false);
    master.compEnabled.store (false);

    struct Face { Kind kind; int index; const char* name; CompParams* comp; std::atomic<bool>* engaged; };
    const Face faces[] { { Kind::Bus, kBus, "bus", &bus.comp, &bus.compEnabled },
                         { Kind::Master, 0, "master", &master.comp, &master.compEnabled } };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto& face : faces)
    {
        for (const auto id : { CompParamId::BusRatio, CompParamId::BusAttack, CompParamId::BusRelease, CompParamId::BusMakeup })
        {
            const auto before = std::make_shared<float> (0.0f);
            steps->push_back ({ 300, [&host, &ctx, face, id, before]
            {
                *before = face.comp->get (id);
                ctx.expect (host.dragStripComp (face.kind, face.index, symbolOf (id), 150),
                            std::string ("the ") + face.name + "'s " + symbolOf (id) + " knob is not on screen");
            } });
            steps->push_back ({ 100, [&ctx, face, id, before]
            {
                ctx.expect (std::abs (face.comp->get (id) - *before) > 1.0e-6f,
                            std::string ("a drag on the ") + face.name + "'s " + symbolOf (id) + " knob left it at "
                                + std::to_string (*before));
                ctx.expect (! face.engaged->load(),
                            std::string ("turning the ") + face.name + "'s " + symbolOf (id) + " knob engaged the compressor");
            } });
        }
        steps->push_back ({ 300, [&host, &ctx, face]
        {
            ctx.expect (host.dragStripComp (face.kind, face.index, "handle", 30),
                        std::string ("the ") + face.name + "'s GR handle is not on screen");
        } });
        steps->push_back ({ 100, [&ctx, face]
        {
            ctx.expect (std::abs (face.comp->get (CompParamId::BusThreshold)
                                  - compParamSpec (CompParamId::BusThreshold).hostDefault) > 1.0e-6f,
                        std::string ("dragging the ") + face.name + "'s handle left the threshold at its default");
            ctx.expect (face.engaged->load(), std::string ("dragging the ") + face.name + "'s handle did not engage the compressor");
            face.engaged->store (false);
            face.comp->set (CompParamId::BusThreshold, -12.0f);
        } });
        const auto others = std::make_shared<CompParams>();
        steps->push_back ({ 700, [&host, &ctx, face, others]
        {
            others->copyFrom (*face.comp);
            ctx.expect (host.clickStripComp (face.kind, face.index, "handle", 2),
                        std::string ("the ") + face.name + "'s GR handle is not on screen");
        } });
        steps->push_back ({ 300, [&ctx, face, others]
        {
            const std::string when = std::string ("a double-click on the bypassed ") + face.name + "'s handle";
            ctx.expect (std::abs (face.comp->get (CompParamId::BusThreshold)
                                  - compParamSpec (CompParamId::BusThreshold).hostDefault) < 1.0e-6f,
                        when + " left the threshold at " + std::to_string (face.comp->get (CompParamId::BusThreshold)));
            for (const auto id : compStageParams (CompStage::Bus))
                if (id != CompParamId::BusThreshold)
                    ctx.expect (std::abs (face.comp->get (id) - others->get (id)) < 1.0e-6f,
                                when + " moved " + symbolOf (id) + " to " + std::to_string (face.comp->get (id)));
            ctx.expect (! face.engaged->load(), when + " engaged the compressor");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar compBusMasterFace { Scenario {
    "gui.comp_bus_master_face", { "gui", "master", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runCompBusMasterFace (host, ctx); }
} };

// The Multiband face closes when a dialog opens over the window, Save As here,
// and when the MASTERING stage is left, both from the keyboard the face leaves
// with the window.
std::optional<ScenarioResult> runMasteringCompEditorCloses (GuiHost& host, ScenarioContext& ctx)
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
        return ScenarioResult::skip ("requires a window the compressor editor can embed into");
    const auto size = host.mainWindowSize();
    if (size.size() < 2) return ScenarioResult::fail ("the window size is unavailable");

    keepComp (ctx, ctx.session().mastering().comp);
    ctx.keep (ctx.session().mastering().compEnabled);
    keepStage (host, ctx);
    ctx.cleanup ([&host, size]
    {
        host.closeMasteringCompEditor();
        drainModals (host);
        host.resizeMainWindow (size[0], size[1]);
    });
    if (! host.resizeMainWindow (std::max (1440, size[0]), std::max (900, size[1])))
        return ScenarioResult::fail ("could not widen the mastering workspace");
    host.switchToStage (GuiHost::Stage::Mastering);

    const auto drawn = [&host]
    {
        return host.masteringCompEditorDrawn() && host.masteringCompEditorShows ("mode").has_value();
    };
    const auto saveAsUp = [&host] { return host.modalText().rfind ("Save session as...", 0) == 0; };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickMasteringCompHeader (true), "the MULTIBAND COMP label is not on screen"); } });
    steps->push_back ({ 300, [&host, &ctx] { ctx.expect (host.pressPeerKey ("command + shift + S"), "the window took no Cmd+Shift+S"); },
        drawn, "the Multiband face never drew a frame" });
    steps->push_back ({ 0, [] {}, saveAsUp, "Cmd+Shift+S over the Multiband face did not bring up Save As" });
    steps->push_back ({ 0, [&host, &ctx, saveAsUp]
    {
        ctx.expect (saveAsUp(), "Save As went down before the Multiband face closed");
        host.closeTopModal();
    }, [&host] { return ! host.masteringCompEditorOpen(); }, "Save As opened over the Multiband face and left it open",
       {}, kSettleBoundMs });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.clickMasteringCompHeader (true), "the MULTIBAND COMP label is not on screen after Save As"); },
        [&host] { return host.modalStackEmpty(); }, "Save As did not close" });
    steps->push_back ({ 300, [&host, &ctx] { ctx.expect (host.pressPeerKey ("command + 2"), "the window took no Cmd+2"); },
        drawn, "the Multiband face did not reopen" });
    steps->push_back ({ 0, [] {}, [&host] { return host.stageViewMatches (GuiHost::Stage::Mixing); },
                       "Cmd+2 over the Multiband face did not switch to MIXING", {}, kSettleBoundMs });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.masteringCompEditorOpen(); }, kSettleBoundMs,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "leaving the MASTERING stage left the Multiband face open");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar masteringCompEditorCloses { Scenario {
    "gui.mastering_comp_editor_closes", { "gui", "mastering", "comp" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringCompEditorCloses (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
