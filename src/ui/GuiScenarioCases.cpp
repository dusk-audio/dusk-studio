#include "GuiHost.h"
#include "AppConfig.h"
#include "../foundation/Fs.h"

#include "../engine/AudioEngine.h"
#include "../engine/audiofile/FileWriter.h"
#include "../engine/PluginSlot.h"
#include "../engine/audiofile/FileWriter.h"
#include "../engine/audiofile/FileReader.h"
#include "../engine/scenario/Scenario.h"
#include "../engine/scenario/ScenarioContext.h"
#include "../engine/scenario/cases/OopStubHarness.h"
#include "../dsp/ChannelStrip.h"
#include "../session/Session.h"
#include "../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if DUSKSTUDIO_HAS_ALSA
 #include <alsa/asoundlib.h>
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (_WIN32)
 #include <cerrno>
 #include <csignal>
#endif

// The scenario suite's window-driven cases: what the release checklist used to
// check by opening editors by hand. Each drives the live window through GuiHost
// and puts back what it changed, so the next case starts on a clean strip.
namespace duskstudio::scenario
{
namespace
{
constexpr int kStripIndex = 0;
constexpr int kAuxLane    = 0;
constexpr int kAuxSlot    = 0;

// A delay before each step, run from the message loop - the shape the capture
// harness uses, and the only one that lets a plug-in editor actually come up
// between two steps.
struct Step
{
    int delayMs;
    std::function<void()> run;
};

void runSteps (ScenarioContext& ctx, std::shared_ptr<std::vector<Step>> steps,
               std::function<void()> onDone)
{
    // The pending timer holds the runner and the runner holds itself weakly, so
    // it lives exactly as long as it has a step left.
    auto runFrom = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakRunner = runFrom;

    *runFrom = [&ctx, steps, weakRunner, onDone] (std::size_t index)
    {
        if (ctx.isComplete()) return;

        if (index >= steps->size())
        {
            onDone();
            return;
        }

        (*steps)[index].run();

        const auto next = index + 1;
        const int delay = next < steps->size() ? (*steps)[next].delayMs : 0;
        if (auto runner = weakRunner.lock())
            ctx.later (delay, [runner, next] { (*runner) (next); });
    };

    ctx.later (steps->empty() ? 0 : steps->front().delayMs, [runFrom] { (*runFrom) (0); });
}

// A failed open is a legitimate outcome for a fixture that ships no editor, and
// it leaves the alert that says so on screen. Take it down so the next step sees
// the window the user would.
void dismissAlert (GuiHost& host)
{
    if (! host.modalStackEmpty()) host.closeTopModal();
}

GuiHost::Stage guiStage (AudioEngine::Stage stage)
{
    switch (stage)
    {
        case AudioEngine::Stage::Mixing:    return GuiHost::Stage::Mixing;
        case AudioEngine::Stage::Aux:       return GuiHost::Stage::Aux;
        case AudioEngine::Stage::Mastering: return GuiHost::Stage::Mastering;
        case AudioEngine::Stage::Recording: break;
    }
    return GuiHost::Stage::Recording;
}

// Puts the stage back to whatever the case found, however the case ends. A
// stage left on Mixing is the window the NEXT case opens on, so every case
// that switches stages registers this before it does.
void keepStage (GuiHost& host, ScenarioContext& ctx)
{
    ctx.cleanup ([&host, stage = guiStage (ctx.engine().getStage())] { host.switchToStage (stage); });
}

void drainModals (GuiHost& host)
{
    for (int guard = 0; guard < 32 && ! host.modalStackEmpty(); ++guard) host.closeTopModal();
}

StripHandle* readyStrip (GuiHost& host, ScenarioContext& ctx)
{
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    auto* strip = host.strip (kStripIndex);
    if (strip == nullptr) return nullptr;
    strip->closeEditor();
    strip->unloadNativePlugins();
    strip->refreshInsertButton();
    return strip;
}

// ------------------------------------------------------- editor open / close

enum class Format { Clap, Lv2, Vst3 };

struct EditorLeg
{
    std::string label;
    std::filesystem::path file;
    std::string pluginId;
    Format format;
    bool hasEditor = false;
    bool loaded = false;
    bool editorSeen = false;
    bool editorUnavailable = false;
};

bool loadLeg (StripHandle& strip, EditorLeg& leg, std::string& errorOut)
{
    switch (leg.format)
    {
        case Format::Clap: return strip.loadNativeClap (leg.file, leg.pluginId, errorOut);
        case Format::Lv2:  return strip.loadNativeLv2  (leg.file, leg.pluginId, errorOut);
        case Format::Vst3: return strip.loadNativeVst3 (leg.file, leg.pluginId, errorOut);
    }
    return false;
}

std::optional<ScenarioResult> runEditorOpenCloseLoop (GuiHost& host, ScenarioContext& ctx)
{
    constexpr int kCycles = 5;

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    struct Candidate
    {
        const char* label; const char* fixture; const char* pluginId; Format format;
        bool hasEditor;
    };
    // multi_bus.clap ships no CLAP_EXT_GUI, the LV2 fixture no UI and the VST3
    // one only a lifecycle-probe view with no platform to attach to, so they
    // cover the refused-open half. no_window.clap reports a GUI, so it is the
    // fixture whose editor teardown actually runs, and on a display that can
    // embed editors it has to open.
    constexpr Candidate kCandidates[] = {
        { "CLAP",           "multi_bus.clap", "studio.dusk.test.multi-bus",        Format::Clap, false },
        { "CLAP/no-window", "no_window.clap", "studio.dusk.test.no-window",        Format::Clap, true  },
        { "LV2",            "file_state.lv2", "urn:duskstudio:test:control-state", Format::Lv2,  false },
        { "VST3",           "relayout.vst3",  "",                                  Format::Vst3, false },
    };

    auto legs = std::make_shared<std::vector<EditorLeg>>();
    for (const auto& candidate : kCandidates)
    {
        if (const auto file = ctx.fixture (candidate.fixture))
            legs->push_back ({ candidate.label, *file, candidate.pluginId, candidate.format,
                               candidate.hasEditor });
        else
            ctx.note (std::string (candidate.label) + ": " + candidate.fixture
                      + " did not resolve");
    }

    if (legs->empty())
        return ScenarioResult::skip ("no editor fixture resolved");

    auto steps = std::make_shared<std::vector<Step>>();
    for (std::size_t index = 0; index < legs->size(); ++index)
    {
        steps->push_back ({ 100, [&ctx, strip, legs, index]
        {
            auto& leg = (*legs)[index];
            strip->unloadNativePlugins();
            std::string error;
            leg.loaded = loadLeg (*strip, leg, error);
            ctx.expect (leg.loaded, leg.label + ": the fixture did not load (" + error + ")");
            strip->refreshInsertButton();
        } });

        for (int cycle = 0; cycle < kCycles; ++cycle)
        {
            steps->push_back ({ 150, [&ctx, &host, strip, legs, index]
            {
                auto& leg = (*legs)[index];
                if (! leg.loaded || leg.editorUnavailable) return;

                if (strip->openEditor())
                {
                    leg.editorSeen = true;
                    return;
                }

                // Judged at the end: an editorless fixture says so through the
                // alert, and the rest of its cycles have nothing left to close.
                leg.editorUnavailable = true;
                ctx.note (leg.label + ": no editor came up, so its close cycles were skipped");
                dismissAlert (host);
            } });

            steps->push_back ({ 400, [&ctx, &host, strip, legs, index]
            {
                auto& leg = (*legs)[index];
                if (! leg.loaded || leg.editorUnavailable) return;

                strip->closeEditor();
                ctx.expect (! strip->hasOpenEditor(),
                            leg.label + ": the editor was still open after closing it");
                ctx.expect (host.modalStackEmpty(),
                            leg.label + ": closing the editor left a modal up");
            } });
        }

        steps->push_back ({ 200, [strip] { strip->unloadNativePlugins(); } });
    }

    steps->push_back ({ 200, [strip] { strip->refreshInsertButton(); } });

    runSteps (ctx, steps, [&ctx, &host, legs]
    {
        int opened = 0;
        for (const auto& leg : *legs)
        {
            if (leg.editorSeen) ++opened;
            else if (leg.loaded && leg.hasEditor && host.canEmbedPluginEditors())
                ctx.expect (false, leg.label + ": its editor did not open on a display that embeds editors");
        }

        ctx.expect (host.modalStackEmpty(), "the run left a modal up");
        if (opened == 0 && ctx.verdict().status == ScenarioStatus::Pass)
        {
            ctx.complete (ScenarioResult::skip (
                "no fixture editor could be embedded on this display"));
            return;
        }

        ctx.note ("editors opened and closed for " + std::to_string (opened) + " of "
                  + std::to_string (legs->size()) + " fixtures");
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

// ------------------------------------------------------- aux attach failure

std::optional<ScenarioResult> runAuxAttachFailure (GuiHost& host, ScenarioContext& ctx)
{
    const auto fixture = ctx.fixture ("multi_bus.clap");
    if (! fixture)
        return ScenarioResult::skip ("missing fixture: multi_bus.clap");

    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Aux);
    auto* lane = host.auxLane (kAuxLane);
    if (lane == nullptr)
        return ScenarioResult::skip ("the aux stage realised no lane to drive");

    auto steps = std::make_shared<std::vector<Step>>();

    steps->push_back ({ 300, [&ctx, lane, fixture]
    {
        ctx.expect (lane->loadNativeClap (kAuxSlot, *fixture, "studio.dusk.test.multi-bus"),
                    "the fixture did not load on the aux lane");
    } });

    // The lane embeds its native editor from the slot row, so this is the call
    // that tries - and here refuses - the attach.
    steps->push_back ({ 300, [lane] { lane->rebuildSlots(); } });

    steps->push_back ({ 400, [&ctx, &host, lane]
    {
        ctx.expect (! lane->attachEditor (kAuxSlot),
                    "the aux lane embedded an editor for a plug-in that has no GUI");
        // A lane attach has no user-initiated open to answer and no on-screen
        // parent, so it logs and never raises a dialog - an invisible modal
        // here would gate native-editor reopen everywhere else. The stderr line
        // it logs instead is the black-box runner's assertion.
        ctx.expect (host.modalStackEmpty(), "the refused aux attach put a modal up");
    } });

    steps->push_back ({ 200, [lane] { lane->unloadSlot (kAuxSlot); } });

    steps->push_back ({ 200, [&ctx, &host]
    {
        host.switchToStage (GuiHost::Stage::Mixing);
        ctx.expect (host.modalStackEmpty(), "the run left a modal up");
    } });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// --------------------------------------------------- CLAP with no window

std::optional<ScenarioResult> runClapNoWindowMessage (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_CLAP
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without native CLAP hosting");
   #else
    static constexpr int kBlankNoticeTimeoutMs = 20000;

    const auto fixture = ctx.fixture ("no_window.clap");
    if (! fixture)
        return ScenarioResult::skip ("missing fixture: no_window.clap");

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    std::string error;
    if (! strip->loadNativeClap (*fixture, "studio.dusk.test.no-window", error))
        return ScenarioResult::fail ("the fixture did not load: " + error);
    strip->refreshInsertButton();

    const auto finish = [&ctx, &host, strip]
    {
        strip->closeEditor();
        strip->unloadNativePlugins();
        strip->refreshInsertButton();
        ctx.expect (host.modalStackEmpty(), "the run left a modal up");
        ctx.complete (ctx.verdict());
    };

    ctx.later (200, [&ctx, &host, strip, finish]
    {
        if (! strip->openEditor())
        {
            dismissAlert (host);
            strip->unloadNativePlugins();
            ctx.complete (ScenarioResult::skip (
                "the CLAP editor could not be embedded on this display"));
            return;
        }

        // The host waits out its own grace period and then two polls a second
        // apart before it calls a container empty, so this is a wait, not a
        // fixed delay.
        ctx.waitUntil ([strip] { return strip->pluginWindowMissing(); },
                       kBlankNoticeTimeoutMs, finish,
                       "the host never noticed the plug-in had put no window in the container");
    });
    return std::nullopt;
   #endif
}

// ------------------------------------------------ LV2 editor reflects state

std::optional<ScenarioResult> runLv2EditorReflectsState (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_LV2
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without native LV2 hosting");
   #else
    static constexpr double kGain = 0.75;
    static constexpr double kTolerance = 1.0e-4;

    const auto fixture = ctx.fixture ("file_state.lv2");
    if (! fixture)
        return ScenarioResult::skip ("missing fixture: file_state.lv2");

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    std::string error;
    if (! strip->loadNativeLv2 (*fixture, "urn:duskstudio:test:control-state", error))
        return ScenarioResult::fail ("the fixture did not load: " + error);
    strip->refreshInsertButton();

    auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeLv2Slot();
    bool moved = false;
    for (int i = 0; i < slot.paramCount(); ++i)
        if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == "Gain")
        {
            slot.setParamValue (info->id, kGain);
            moved = true;
        }
    if (! moved)
    {
        strip->unloadNativePlugins();
        return ScenarioResult::fail ("the fixture exposes no Gain control");
    }
    ctx.later (200, [&ctx, &host, strip]
    {
        if (! strip->openEditor())
        {
            dismissAlert (host);
            strip->unloadNativePlugins();
            strip->refreshInsertButton();
            // The fixture declares no ui:ui, so lilv finds nothing to embed and
            // the host says so instead of showing an empty panel.
            ctx.complete (ScenarioResult::skip (
                "the control-state fixture ships no plug-in UI to open"));
            return;
        }

        ctx.later (400, [&ctx, &host, strip]
        {
            double shown = 0.0;
            if (ctx.expect (strip->readEditorControl ("Gain", shown),
                            "the open editor reported no Gain control"))
                ctx.expect (std::abs (shown - kGain) < kTolerance,
                            "the editor shows " + std::to_string (shown)
                                + " where the plug-in holds " + std::to_string (kGain));

            strip->closeEditor();
            strip->unloadNativePlugins();
            strip->refreshInsertButton();
            ctx.expect (host.modalStackEmpty(), "the run left a modal up");
            ctx.complete (ctx.verdict());
        });
    });
    return std::nullopt;
   #endif
}

// ------------------------------------------------------ sandboxed editors

#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (_WIN32)
// Both sandboxed cases put the app's own plug-in manager into sandbox mode for
// the length of one scenario, so both hand it back exactly as it was on every
// way out, the watchdog's included: the context runs this when the case ends.
void restoreInProcessHosting (ScenarioContext& ctx, PluginSlot& slot, bool oopWasEnabled)
{
    slot.unload();
    auto& manager = ctx.engine().getPluginManager();
    manager.setOopEnabled (oopWasEnabled);
    manager.setHostExecutableOverride ({}, {});
}

struct SandboxState
{
    bool loadDone = false;
    bool loadOk = false;
    std::string loadError;
    int childPid = -1;
    bool editorOpenFailed = false;
    bool childDiedEarly = false;
};

bool processGone (int pid)
{
    return ::kill (pid, 0) != 0 && errno == ESRCH;
}

std::optional<ScenarioResult> runOopEditorClosesBeforeChild (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr int kChildExitTimeoutMs = 15000;

    const auto childBinary = oopstub::hostBinary();
    if (! childBinary)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    const auto fixture = ctx.fixture ("relayout.vst3");
    if (! fixture)
        return ScenarioResult::skip ("missing fixture: relayout.vst3");

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    auto& engine = ctx.engine();
    auto& manager = engine.getPluginManager();
    auto& slot = engine.getChannelStrip (kStripIndex).getPluginSlot();
    slot.unload();

    // The plug-in reference the child is asked to load, minted in process the
    // way the session's own would have been.
    using HostFile   = std::decay_t<decltype (ctx.session().getSessionDirectory())>;
    using HostString = std::decay_t<decltype (ctx.session().getSessionDirectory()
                                                  .getFullPathName())>;
    HostString loadError;
    auto probe = manager.createPluginInstance (
        HostFile (HostString::fromUTF8 (fixture->u8string().c_str())),
        ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, loadError);
    if (probe == nullptr)
        return ScenarioResult::skip ("the fixture did not load through the JUCE host: "
                                     + loadError.toStdString());
    const auto descriptor = manager.descriptorForInstance (*probe);
    probe.reset();

    auto state = std::make_shared<SandboxState>();
    const bool oopWasEnabled = manager.isOopEnabled();
    oopstub::useStub (manager, *childBinary, "--ipc-host");
    ctx.cleanup ([&ctx, &host, strip, &slot, oopWasEnabled]
    {
        strip->closeEditor();
        dismissAlert (host);
        restoreInProcessHosting (ctx, slot, oopWasEnabled);
        strip->refreshInsertButton();
    });

    slot.loadFromDescriptorAsync (descriptor, [state] (bool ok, auto error)
    {
        state->loadOk = ok;
        state->loadError = error.toStdString();
        state->loadDone = true;
    });

    ctx.waitUntil ([state] { return state->loadDone; }, 30000,
    [&ctx, &host, strip, &slot, state]
    {
        if (! state->loadOk || ! slot.isRemote())
        {
            ctx.complete (ScenarioResult::skip (
                "the slot never went out of process: " + state->loadError));
            return;
        }

        state->childPid = slot.getRemoteChildPid();
        ctx.note ("child pid: " + std::to_string (state->childPid));
        if (state->childPid <= 0)
        {
            ctx.complete (ScenarioResult::fail ("a remote slot reported no child pid"));
            return;
        }

        strip->refreshInsertButton();

        auto steps = std::make_shared<std::vector<Step>>();
        steps->push_back ({ 200, [&ctx, strip, state]
        {
            if (! strip->openEditor())
            {
                ctx.note ("the sandboxed editor did not come up on this display");
                state->editorOpenFailed = true;
            }
        } });
        steps->push_back ({ 600, [&host, strip] { strip->closeEditor(); dismissAlert (host); } });
        steps->push_back ({ 300, [&slot, state]
        {
            // A child that died on its own makes everything below trivially
            // true, so the ordering has to be reported as unobserved instead.
            state->childDiedEarly = slot.wasCrashed() || ! slot.isRemote();
            slot.unload();
        } });

        runSteps (ctx, steps, [&ctx, &host, strip, &slot, state]
        {
            if (state->childDiedEarly)
            {
                strip->refreshInsertButton();
                ctx.complete (ScenarioResult::skip (
                    "the sandbox child exited before the editor was closed, so the "
                    "ordering could not be observed"));
                return;
            }

            if (state->editorOpenFailed)
            {
                strip->refreshInsertButton();
                ctx.complete (host.canEmbedPluginEditors()
                    ? ScenarioResult::fail ("the sandboxed editor did not open")
                    : ScenarioResult::skip ("this display cannot embed plug-in editors"));
                return;
            }

            // The child must be gone once the slot is, and the editor must have
            // let go before it: a surviving child would keep an editor window
            // alive over a slot that no longer exists.
            ctx.waitUntil ([state] { return processGone (state->childPid); },
                           kChildExitTimeoutMs,
            [&ctx, &host, strip, &slot]
            {
                ctx.expect (! slot.isRemote(), "the slot still reports a live child");
                ctx.expect (! strip->hasOpenEditor(), "the sandboxed editor is still open");
                ctx.expect (host.modalStackEmpty(), "the run left a modal up");
                strip->refreshInsertButton();
                ctx.complete (ctx.verdict());
            },
            "the sandbox child outlived the slot that owned it");
        });
    },
    "the sandboxed load never completed");
    return std::nullopt;
}

std::optional<ScenarioResult> runOopEditorFailureNoStrand (GuiHost& host, ScenarioContext& ctx)
{
    const auto childBinary = oopstub::hostBinary();
    if (! childBinary)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    auto& engine = ctx.engine();
    auto& manager = engine.getPluginManager();
    auto& slot = engine.getChannelStrip (kStripIndex).getPluginSlot();
    slot.unload();

    auto state = std::make_shared<SandboxState>();
    // The one stub mode that answers the load and then hands back a reply the
    // editor RPC cannot read, which is the failure this covers.
    const bool oopWasEnabled = manager.isOopEnabled();
    oopstub::useStub (manager, *childBinary, "--ipc-load-reply-stub");
    ctx.cleanup ([&ctx, &slot, oopWasEnabled] { restoreInProcessHosting (ctx, slot, oopWasEnabled); });

    slot.loadFromDescriptorAsync (oopstub::stubDescriptor(), [state] (bool ok, auto error)
    {
        state->loadOk = ok;
        state->loadError = error.toStdString();
        state->loadDone = true;
    });

    ctx.waitUntil ([state] { return state->loadDone; }, 30000,
    [&ctx, &host, strip, &slot, state]
    {
        if (! state->loadOk || ! slot.isRemote())
        {
            ctx.complete (ScenarioResult::skip (
                "the slot never went out of process: " + state->loadError));
            return;
        }

        strip->refreshInsertButton();
        ctx.expect (! strip->openEditor(),
                    "the sandboxed open reported an editor it could not have");
        ctx.expect (host.modalStackEmpty(), "the failed sandboxed open put a modal up");

        // Nothing but this later step can complete the scenario, so a refused
        // open that stranded the window would time out instead of passing.
        ctx.later (200, [&ctx, &host, strip, &slot]
        {
            ctx.expect (! strip->hasOpenEditor(), "an editor came up after the refused open");
            ctx.expect (host.modalStackEmpty(), "the refused open left a modal up");

            slot.unload();
            strip->refreshInsertButton();
            ctx.complete (ctx.verdict());
        });
    },
    "the sandboxed load never completed");
    return std::nullopt;
}
#endif

// -------------------------------------------------------------- automation

bool laneHolds (const std::vector<AutomationPoint>& points, AutomationParam param, float value, float tolerance)
{
    return std::any_of (points.begin(), points.end(), [&] (const AutomationPoint& point)
    {
        return std::abs (denormalizeAutomationValue (param, point.value) - value) < tolerance;
    });
}

bool timesAscend (const std::vector<AutomationPoint>& points)
{
    for (std::size_t i = 1; i < points.size(); ++i)
        if (points[i].timeSamples <= points[i - 1].timeSamples) return false;
    return true;
}

// The strips' own timers record the rides while the transport rolls. Write
// takes the fader, pan, mute and solo; Touch records the fader and pan while
// held and returns to the earlier ride on release; a mute click in Touch
// records nothing; a bus's fader, pan and mute, the master fader and an aux
// return's level and mute record like a track's; a Write pass leaves the ride
// after where it stopped alone.
std::optional<ScenarioResult> runAutomation (GuiHost& host, ScenarioContext& ctx)
{
    static constexpr float kDbTolerance = 0.05f;
    // The aux lanes record from their own strips, which the Aux stage builds
    // the first time it is shown and which keep running once it is hidden.
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Aux);
    host.switchToStage (GuiHost::Stage::Mixing);
    auto* strip = host.strip (kStripIndex);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kStripIndex);
    auto& params = track.strip;
    auto& bus = session.bus (0).strip;
    auto& master = session.master();
    auto& aux = session.auxLane (0).params;

    const auto lane = [&track] (AutomationParam p) -> auto&
    { return track.automationLanes[(std::size_t) p]; };
    const auto clearAll = [&track, &bus, &master, &aux]
    {
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        bus.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        master.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        aux.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        for (auto& l : track.automationLanes) l.publishPoints ({});
        for (auto& l : bus.automationLanes) l.publishPoints ({});
        for (auto& l : master.automationLanes) l.publishPoints ({});
        for (auto& l : aux.automationLanes) l.publishPoints ({});
    };
    ctx.cleanup ([&engine, &session, &params, &bus, &master, &aux, clearAll]
    {
        engine.stop();
        clearAll();
        params.faderDb.store (0.0f, std::memory_order_relaxed);
        params.pan.store (0.0f, std::memory_order_relaxed);
        params.faderTouched.store (false, std::memory_order_relaxed);
        params.panTouched.store (false, std::memory_order_relaxed);
        params.mute.store (false, std::memory_order_relaxed);
        session.setTrackSoloed (kStripIndex, false);
        bus.faderDb.store (0.0f, std::memory_order_relaxed);
        bus.pan.store (0.0f, std::memory_order_relaxed);
        bus.mute.store (false, std::memory_order_relaxed);
        master.faderDb.store (0.0f, std::memory_order_relaxed);
        aux.returnLevelDb.store (0.0f, std::memory_order_relaxed);
        aux.mute.store (false, std::memory_order_relaxed);
    });

    struct Marks
    {
        std::size_t muteCount = 0;
        std::int64_t stoppedAt = 0;
    };
    auto marks = std::make_shared<Marks>();
    auto steps = std::make_shared<std::vector<Step>>();
    const auto roll = [&engine]
    {
        engine.getTransport().setPlayhead (0);
        engine.play();
    };

    // First pass, WRITE: two positions for each continuous control, and a
    // mute on and off, a solo on.
    steps->push_back ({ 0, [&params, &track, &bus, &master, &aux, clearAll, roll]
    {
        clearAll();
        for (auto* mode : { &track.automationMode, &bus.automationMode,
                            &master.automationMode, &aux.automationMode })
            mode->store ((int) AutomationMode::Write, std::memory_order_release);
        params.faderDb.store (-20.0f, std::memory_order_relaxed);
        params.pan.store (-0.5f, std::memory_order_relaxed);
        bus.faderDb.store (-8.0f, std::memory_order_relaxed);
        bus.pan.store (-0.5f, std::memory_order_relaxed);
        master.faderDb.store (-2.0f, std::memory_order_relaxed);
        aux.returnLevelDb.store (-12.0f, std::memory_order_relaxed);
        roll();
    } });
    steps->push_back ({ 400, [&params, &bus, &master, &aux, strip]
    {
        params.faderDb.store (-10.0f, std::memory_order_relaxed);
        params.pan.store (0.5f, std::memory_order_relaxed);
        bus.faderDb.store (-4.0f, std::memory_order_relaxed);
        bus.pan.store (0.5f, std::memory_order_relaxed);
        bus.mute.store (true, std::memory_order_relaxed);
        master.faderDb.store (-4.0f, std::memory_order_relaxed);
        aux.returnLevelDb.store (-6.0f, std::memory_order_relaxed);
        aux.mute.store (true, std::memory_order_relaxed);
        strip->clickMute();
    } });
    steps->push_back ({ 400, [&bus, &aux, strip]
    {
        strip->clickMute();
        strip->clickSolo();
        bus.mute.store (false, std::memory_order_relaxed);
        aux.mute.store (false, std::memory_order_relaxed);
    } });
    // The strips splice a pass on their first timer tick after the stop.
    steps->push_back ({ 400, [&engine] { engine.stop(); } });
    steps->push_back ({ 100, [&ctx, &params, &track, &bus, &master, &aux, lane, marks, roll]
    {
        const auto& fader = lane (AutomationParam::FaderDb).pointsConst();
        ctx.expect (laneHolds (fader, AutomationParam::FaderDb, -20.0f, kDbTolerance)
                        && laneHolds (fader, AutomationParam::FaderDb, -10.0f, kDbTolerance)
                        && timesAscend (fader),
                    "WRITE did not record both fader positions in order");
        const auto& pan = lane (AutomationParam::Pan).pointsConst();
        ctx.expect (laneHolds (pan, AutomationParam::Pan, -0.5f, 0.01f)
                        && laneHolds (pan, AutomationParam::Pan, 0.5f, 0.01f),
                    "WRITE did not record both pan positions");
        const auto onThenOff = [] (const std::vector<AutomationPoint>& points)
        {
            const auto on = std::find_if (points.begin(), points.end(),
                                          [] (const AutomationPoint& p) { return p.value > 0.5f; });
            return on != points.end()
                && std::any_of (on, points.end(), [] (const AutomationPoint& p) { return p.value < 0.5f; });
        };
        const auto& mute = lane (AutomationParam::Mute).pointsConst();
        ctx.expect (onThenOff (mute), "WRITE did not record the mute going on and off");
        ctx.expect (laneHolds (lane (AutomationParam::Solo).pointsConst(), AutomationParam::Solo, 1.0f, 0.1f),
                    "WRITE did not record the solo");
        const auto& busFader = bus.automationLanes[(std::size_t) AutomationParam::FaderDb].pointsConst();
        ctx.expect (laneHolds (busFader, AutomationParam::FaderDb, -8.0f, kDbTolerance)
                        && laneHolds (busFader, AutomationParam::FaderDb, -4.0f, kDbTolerance),
                    "a bus fader in WRITE did not record");
        const auto& busPan = bus.automationLanes[(std::size_t) AutomationParam::Pan].pointsConst();
        ctx.expect (laneHolds (busPan, AutomationParam::Pan, -0.5f, 0.01f)
                        && laneHolds (busPan, AutomationParam::Pan, 0.5f, 0.01f),
                    "a bus pan in WRITE did not record");
        ctx.expect (onThenOff (bus.automationLanes[(std::size_t) AutomationParam::Mute].pointsConst()),
                    "a bus mute in WRITE did not record going on and off");
        const auto& masterFader = master.automationLanes[(std::size_t) AutomationParam::FaderDb].pointsConst();
        ctx.expect (laneHolds (masterFader, AutomationParam::FaderDb, -2.0f, kDbTolerance)
                        && laneHolds (masterFader, AutomationParam::FaderDb, -4.0f, kDbTolerance),
                    "the master fader in WRITE did not record");
        const auto& auxReturn = aux.automationLanes[(std::size_t) AutomationParam::FaderDb].pointsConst();
        ctx.expect (laneHolds (auxReturn, AutomationParam::FaderDb, -12.0f, kDbTolerance)
                        && laneHolds (auxReturn, AutomationParam::FaderDb, -6.0f, kDbTolerance),
                    "an aux return in WRITE did not record");
        ctx.expect (onThenOff (aux.automationLanes[(std::size_t) AutomationParam::Mute].pointsConst()),
                    "an aux mute in WRITE did not record going on and off");
        marks->muteCount = mute.size();

        // Second pass, TOUCH: hold the fader and pan for the first part only.
        track.automationMode.store ((int) AutomationMode::Touch, std::memory_order_release);
        for (auto* mode : { &bus.automationMode, &master.automationMode, &aux.automationMode })
            mode->store ((int) AutomationMode::Off, std::memory_order_release);
        params.faderTouched.store (true, std::memory_order_relaxed);
        params.faderDb.store (-3.0f, std::memory_order_relaxed);
        params.panTouched.store (true, std::memory_order_relaxed);
        params.pan.store (-0.8f, std::memory_order_relaxed);
        roll();
    } });
    steps->push_back ({ 250, [&params, strip]
    {
        params.faderTouched.store (false, std::memory_order_relaxed);
        params.panTouched.store (false, std::memory_order_relaxed);
        strip->clickMute();
    } });
    // The first pass rode -10 dB from 400 ms on.
    steps->push_back ({ 700, [&ctx, &engine, &params]
    {
        const float live = params.liveFaderDb.load (std::memory_order_relaxed);
        ctx.note ("TOUCH: the fader plays " + std::to_string (live) + " dB after the release");
        ctx.expect (std::abs (live - (-10.0f)) < 0.5f,
                    "after TOUCH let go the fader did not return to the earlier ride (-10 dB)");
        engine.stop();
    } });
    steps->push_back ({ 100, [&ctx, &params, &track, lane, marks, roll, strip]
    {
        ctx.expect (laneHolds (lane (AutomationParam::FaderDb).pointsConst(), AutomationParam::FaderDb,
                               -3.0f, kDbTolerance),
                    "TOUCH did not record the fader while it was held");
        ctx.expect (laneHolds (lane (AutomationParam::Pan).pointsConst(), AutomationParam::Pan, -0.8f, 0.01f),
                    "TOUCH did not record the pan while it was held");
        ctx.expect (lane (AutomationParam::Mute).pointsConst().size() == marks->muteCount,
                    "a mute click in TOUCH was recorded");
        strip->clickMute();

        // Third pass, WRITE over the start only.
        track.automationMode.store ((int) AutomationMode::Write, std::memory_order_release);
        params.faderDb.store (-15.0f, std::memory_order_relaxed);
        roll();
    } });
    steps->push_back ({ 250, [&engine, marks]
    {
        engine.stop();
        marks->stoppedAt = engine.getTransport().getPlayhead();
    } });
    steps->push_back ({ 100, [&ctx, &track, lane, marks]
    {
        const auto& fader = lane (AutomationParam::FaderDb).pointsConst();
        const auto stoppedAt = marks->stoppedAt;
        ctx.note ("third pass stopped at " + std::to_string (stoppedAt) + "; the lane ends at "
                  + (fader.empty() ? std::string ("-") : std::to_string (fader.back().timeSamples)));
        ctx.expect (std::any_of (fader.begin(), fader.end(),
                                 [stoppedAt] (const AutomationPoint& p) { return p.timeSamples > stoppedAt + 4800; }),
                    "a WRITE pass erased the automation after where it stopped");
        ctx.expect (timesAscend (fader), "the fader lane is out of time order");
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
    } });

    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

// A mode set outside a strip's own menu - drawing automation arms READ, a
// session load sets the aux lanes it does not rebuild - shows on every strip's
// label within a tick, and READ locks every strip's fader.
std::optional<ScenarioResult> runModeShownOnEveryStrip (GuiHost& host, ScenarioContext& ctx)
{
    struct Probe { GuiHost::StripKind kind; int index; const char* name; };
    static constexpr Probe kProbes[] = {
        { GuiHost::StripKind::Channel, kStripIndex, "channel" },
        { GuiHost::StripKind::Bus,     0,           "bus" },
        { GuiHost::StripKind::Master,  0,           "master" },
        { GuiHost::StripKind::Aux,     0,           "aux return" },
    };

    // The aux lanes are built the first time their stage shows, and keep
    // running once it is hidden.
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Aux);
    host.switchToStage (GuiHost::Stage::Mixing);

    auto& session = ctx.session();
    const std::array<std::atomic<int>*, 4> modes {
        &session.track (kStripIndex).automationMode, &session.bus (0).strip.automationMode,
        &session.master().automationMode, &session.auxLane (0).params.automationMode };
    const auto setAll = [modes] (AutomationMode mode)
    {
        for (auto* m : modes) m->store ((int) mode, std::memory_order_release);
    };
    ctx.cleanup ([setAll] { setAll (AutomationMode::Off); });

    const auto check = [&ctx, &host] (bool read)
    {
        for (const auto& probe : kProbes)
        {
            std::string label;
            bool faderEnabled = false;
            const std::string name (probe.name);
            if (! ctx.expect (host.automationView (probe.kind, probe.index, label, faderEnabled),
                              name + ": the strip is not built"))
                continue;
            ctx.expect (! label.empty() && label.front() == (read ? 'R' : 'O'),
                        name + ": the mode label shows '" + label + "'");
            ctx.expect (faderEnabled != read, name + (read ? ": the fader takes input in READ"
                                                           : ": the fader stayed locked after READ"));
        }
    };

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [setAll] { setAll (AutomationMode::Read); } });
    steps->push_back ({ 150, [check, setAll] { check (true); setAll (AutomationMode::Off); } });
    steps->push_back ({ 150, [check] { check (false); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

std::optional<ScenarioResult> runTimelineKeys (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording
        && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    const auto originalDir = currentSessionDirectory (session);
    const auto playhead = engine.getTransport().getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    const auto originalView = host.tapeView();
    const bool timeline = host.setTimelineShown (true);
    ctx.cleanup ([&host, &engine, &session, originalDir, restore, timeline, playhead, originalView]
    {
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        for (int i = 0; i < Session::kNumTracks; ++i) session.setTrackArmed (i, false);
        host.setTimelineShown (timeline);
        host.restoreTapeView (originalView);
        engine.getTransport().locate (playhead);
    });
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        session.track (i).regions.clear();
        session.track (i).midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        session.setTrackArmed (i, false);
    }
    AudioRegion region;
    region.lengthInSamples = (std::int64_t) engine.getCurrentSampleRate() * 30;
    session.track (0).regions.push_back (region);
    const auto key = [&host, &ctx] (const std::string& description, char text)
    { ctx.expect (host.pressPeerKey (description, text), "timeline shortcut was not handled"); };
    const auto fit = std::make_shared<double> (0.0);
    const auto anchor = std::make_shared<std::int64_t> (0);
    const auto beforeWheel = std::make_shared<std::vector<double>>();
    const auto view = [&host] { return host.tapeView(); };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [key] { key ("0", '0'); } });
    steps->push_back ({ 100, [view, fit, key] { *fit = view()[0]; key ("=", '='); } });
    steps->push_back ({ 100, [&ctx, view, fit, key]
    { ctx.expect (std::abs (view()[0] - *fit * 1.15) < 0.0001, "equals did not zoom in"); key ("=", '+'); } });
    steps->push_back ({ 100, [&ctx, view, fit, key]
    { ctx.expect (std::abs (view()[0] - *fit * 1.15 * 1.15) < 0.0001, "plus did not zoom in"); key ("-", '-'); } });
    steps->push_back ({ 100, [&ctx, view, fit, &host, anchor]
    {
        ctx.expect (std::abs (view()[0] - *fit * 1.15) < 0.0001, "minus did not zoom out");
        *anchor = host.tapeRulerSample (0.6f);
        ctx.expect (host.tapeWheel (0.6f, 1.0f, true, false), "command wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, view, fit, &host, anchor, beforeWheel]
    {
        ctx.expect (std::abs (view()[0] - *fit * 1.15 * 1.15) < 0.0001, "command wheel did not zoom");
        ctx.expect (std::abs (host.tapeRulerSample (0.6f) - *anchor) <= 2, "wheel zoom moved the cursor's sample");
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, -1.0f, false, true), "shift wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, &host, view, beforeWheel]
    {
        ctx.expect (view()[1] > (*beforeWheel)[1] && std::abs (view()[0] - (*beforeWheel)[0]) < 0.0001,
                    "shift wheel did not scroll the zoomed timeline horizontally");
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, -1.0f, false, false), "plain wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, view, beforeWheel, key]
    {
        ctx.expect (view()[1] > (*beforeWheel)[1], "plain wheel did not scroll horizontally");
        key ("0", '0');
    } });
    steps->push_back ({ 100, [&ctx, view, fit, key]
    {
        ctx.expect (std::abs (view()[0] - *fit) < 0.0001 && std::abs (view()[1]) < 0.5, "fit did not reset zoom and scroll");
        key ("T", 't');
    } });
    steps->push_back ({ 100, [&ctx, view, key]
    { ctx.expect (view()[3] < 0.5, "T did not hide timeline"); key ("T", 't'); } });
    steps->push_back ({ 100, [&ctx, view]
    { ctx.expect (view()[3] > 0.5, "T did not restore timeline"); } });
    steps->push_back ({ 100, [&session]
    { for (int i = 0; i < Session::kNumTracks; ++i) session.setTrackArmed (i, true); } });
    steps->push_back ({ 300, [&host, &ctx, view, beforeWheel]
    {
        for (int i = 0; i < 8; ++i) host.tapeWheel (0.6f, 1.0f, true, true);
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, -1.0f, false, false), "overflow wheel unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, view, beforeWheel]
    {
        ctx.expect (view()[2] > (*beforeWheel)[2] && std::abs (view()[1] - (*beforeWheel)[1]) < 0.5,
                    "plain wheel did not scroll overflowing rows vertically");
        *beforeWheel = view();
        ctx.expect (host.tapeWheel (0.6f, 1.0f, false, true), "overflow shift wheel unavailable");
    } });
    steps->push_back ({ 100, [&ctx, view, beforeWheel]
    {
        ctx.expect (view()[2] < (*beforeWheel)[2] && std::abs (view()[1] - (*beforeWheel)[1]) < 0.5,
                    "shift wheel did not scroll overflowing rows vertically");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineKeys { Scenario {
    "gui.timeline_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineKeys (host, ctx); }
} };

std::optional<ScenarioResult> runFileKeys (GuiHost& host, ScenarioContext& ctx)
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
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        engine.getTransport().locate (originalPlayhead);
    });
    if (! host.openSession (restore)) return ScenarioResult::fail ("could not establish saved fixture");
    const auto song = ctx.tempDir() / "NewSong";
    const auto copied = ctx.tempDir() / "CopiedSong";
    const auto sourceDir = ctx.tempDir() / "Input";
    std::filesystem::create_directory (sourceDir);
    const auto source = sourceDir / "Input.wav";
    const auto bounce = ctx.tempDir() / "Keyboard bounce.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    std::vector<float> signal (2400, 0.1f);
    const float* channels[] = { signal.data() };
    if (! writer || ! writer->write (channels, 1, 2400) || ! writer->flush())
        return ScenarioResult::fail ("could not create import fixture");
    writer.reset();
    const auto key = [&host, &ctx] (char letter, bool shift = false)
    {
       #if defined (__APPLE__)
        const std::string modifier = "command + ";
       #else
        const std::string modifier = "ctrl + ";
       #endif
        ctx.expect (host.pressPeerKey (modifier + (shift ? "shift + " : "") + letter, letter), "File shortcut was not handled");
    };
    const auto path = [&host, &ctx] (const std::filesystem::path& value, bool directory = false)
    {
        if (! ctx.expect (directory ? host.clickFileBrowserControl (true) : host.focusFileName(),
                          "File shortcut did not open a browser")) return;
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : value.string()) host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    };
    const auto button = [&host, &ctx] (const std::string& name)
    { ctx.expect (host.clickModalButton (name), "dialog button unavailable: " + name); };
    const auto saved = [&ctx] (const std::filesystem::path& dir, float fader)
    {
        Session probe;
        if (ctx.expect (SessionSerializer::load (probe, dir / "session.json"), "saved session did not load"))
            ctx.expect (std::abs (probe.track (0).strip.faderDb.load() - fader) < 0.001f, "Save did not persist fader");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [key] { key ('n'); } });
    steps->push_back ({ 200, [path, song] { path (song); } });
    steps->push_back ({ 150, [button] { button ("Save"); } });
    steps->push_back ({ 600, [&ctx, &session, song, key]
    {
        ctx.expect (currentSessionDirectory (session) == song && std::filesystem::exists (song / "session.json"), "New did not create a session");
        ctx.expect (session.track (0).regions.empty(), "New did not clear regions");
        key ('i');
    } });
    steps->push_back ({ 200, [path, sourceDir, &host] { path (sourceDir, true); host.pressPeerKey ("Return", 0); } });
    steps->push_back ({ 500, [&host, &ctx]
    {
        ctx.expect (host.clickFileBrowserControl (false), "Import file list unavailable");
        host.pressPeerKey ("Home", 0);
    } });
    steps->push_back ({ 150, [button] { button ("Open"); } });
    steps->push_back ({ 250, [button] { button ("Import"); } });
    steps->push_back ({ 400, [&session, &ctx, key]
    {
        ctx.expect (session.track (0).regions.size() == 1, "Import shortcut did not import audio");
        session.track (0).strip.faderDb.store (-8.0f);
        key ('s');
    } });
    steps->push_back ({ 250, [saved, song, key] { saved (song, -8.0f); key ('s', true); } });
    steps->push_back ({ 200, [path, copied] { path (copied); } });
    steps->push_back ({ 150, [button] { button ("Save"); } });
    steps->push_back ({ 500, [&ctx, &session, copied, saved, key]
    {
        ctx.expect (currentSessionDirectory (session) == copied, "Save As did not change session directory");
        saved (copied, -8.0f);
        const auto& regions = session.track (0).regions;
        if (ctx.expect (regions.size() == 1, "Save As lost imported region"))
            ctx.expect (regions[0].file.existsAsFile()
                        && regions[0].file.getParentDirectory().getFullPathName().toStdString() == (copied / "audio").string(),
                        "Save As did not copy audio to new session");
        session.track (0).strip.faderDb.store (-3.0f);
        key ('s');
    } });
    steps->push_back ({ 250, [saved, copied, key] { saved (copied, -3.0f); key ('o'); } });
    steps->push_back ({ 200, [path, song] { path (song / "session.json"); } });
    steps->push_back ({ 150, [button] { button ("Open"); } });
    steps->push_back ({ 600, [&ctx, &session, song, key]
    {
        ctx.expect (currentSessionDirectory (session) == song && std::abs (session.track (0).strip.faderDb.load() + 8.0f) < 0.001f,
                    "Open did not restore the chosen session");
        key ('b');
    } });
    steps->push_back ({ 200, [path, bounce] { path (bounce); } });
    steps->push_back ({ 150, [button] { button ("Save"); } });
    runSteps (ctx, steps, [&host, &ctx, &session, bounce, key, button]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000,
            [&host, &ctx, &session, bounce, key, button]
            {
                auto reader = dusk::audio::FileReader::open (bounce);
                ctx.expect (reader != nullptr && reader->info().numChannels == 2 && reader->info().bitsPerSample == 24
                            && reader->info().numFrames > 2400, "Bounce shortcut did not render stereo 24-bit WAV");
                auto quitSteps = std::make_shared<std::vector<Step>>();
                quitSteps->push_back ({ 200, [&session, key]
                { session.track (0).strip.faderDb.store (-2.0f); key ('q'); } });
                quitSteps->push_back ({ 200, [&host, &ctx, button]
                { ctx.expect (! host.modalStackEmpty(), "dirty Quit did not prompt"); button ("Cancel"); } });
                quitSteps->push_back ({ 200, [&host, &ctx, &session]
                {
                    ctx.expect (host.modalStackEmpty() && std::abs (session.track (0).strip.faderDb.load() + 2.0f) < 0.001f,
                                "Quit cancellation did not retain the session");
                } });
                runSteps (ctx, quitSteps, [&ctx] { ctx.complete (ctx.verdict()); });
            }, "keyboard bounce did not complete");
    });
    return std::nullopt;
}

const ScenarioRegistrar fileKeys { Scenario {
    "gui.file_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFileKeys (host, ctx); }
} };

std::optional<ScenarioResult> runBuiltinMidiLearn (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    return ScenarioResult::skip ("requires native UI");
   #endif
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& strip = engine.getChannelStrip (0);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || session.midiLearnPending.load() >= 0)
        return ScenarioResult::skip ("requires stopped transport, no modal and no pending learn");
    if (engine.getMidiInputDevices().empty()) return ScenarioResult::skip ("requires an enumerated MIDI input for injection");
    if (strip.getPluginSlot().isLoaded() || strip.isBuiltinLoaded() || strip.isNativeClapLoaded()
        || strip.isNativeLv2Loaded() || strip.isNativeVst3Loaded() || strip.isNativeAuLoaded()
        || strip.isNativeMultisampleLoaded() || strip.builtinReloadFailed())
        return ScenarioResult::skip ("requires an empty first insert");
    const auto bindings = session.midiBindings.current();
    const auto capture = session.midiLearnCapture.load();
    const auto mode = strip.insertMode.load();
    const auto stage = engine.getStage();
    ctx.cleanup ([&host, &engine, &session, &strip, bindings, capture, mode, stage]
    {
        drainModals (host);
        host.closeBuiltin (0);
        engine.suspendProcessing();
        strip.unloadBuiltin();
        strip.insertMode.store (mode);
        engine.resumeProcessing();
        session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (bindings));
        session.midiLearnPending.store (-1);
        session.midiLearnCapture.store (capture);
        if (auto* handle = host.strip (0)) handle->refreshInsertButton();
        host.switchToStage (stage == AudioEngine::Stage::Recording ? GuiHost::Stage::Recording
                            : stage == AudioEngine::Stage::Mixing ? GuiHost::Stage::Mixing
                            : stage == AudioEngine::Stage::Aux ? GuiHost::Stage::Aux : GuiHost::Stage::Mastering);
    });
    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>>());
    host.switchToStage (GuiHost::Stage::Mixing);
    std::string error;
    engine.suspendProcessing();
    const bool loaded = strip.loadBuiltin ("dusk.builtin.utility", error);
    if (loaded) strip.insertMode.store (ChannelStrip::kInsertPlugin);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load Utility: " + error);
    if (auto* handle = host.strip (0)) handle->refreshInsertButton();
    ctx.expect (strip.insertLastTouchedParamIndex() == -1, "fresh unit already has a touched parameter");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx]
    { ctx.expect (host.clickInsert (0, false), "insert button unavailable"); } });
    steps->push_back ({ 600, [&host, &ctx]
    { ctx.expect (host.builtinPointer (0, "gain_db", 0.25f, true), "native Gain pointer down failed"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.builtinPointer (0, "gain_db", 0.25f, false), "native Gain pointer up failed"); } });
    steps->push_back ({ 150, [&host, &ctx, &strip]
    {
        ctx.expect (std::abs (strip.getBuiltinSlot().getParamValue (0)) > 1.0f, "Gain control did not change parameter");
        ctx.expect (strip.insertLastTouchedParamIndex() == 0, "Gain was not tracked as last touched");
        host.closeBuiltin (0);
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickInsert (0, true), "insert context menu unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("MIDI Learn last-touched parameter"), "last-touched Learn action unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("MIDI Learn (this track)..."), "track Learn action unavailable"); } });
    steps->push_back ({ 150, [&engine, &session, &ctx]
    {
        const auto pending = session.midiLearnPending.load();
        ctx.expect (pending >= 0 && unpackLearnTargetKind (pending) == MidiBindingTarget::TrackPluginParam
                    && unpackLearnTargetIndex (pending) == 0, "Learn did not target the first insert");
        dusk::MidiBuffer midi;
        const std::uint8_t cc[] = { 0xb2, 74, 96 };
        midi.addEvent (cc, 3, 0);
        engine.stageTestMidiInjection (0, std::move (midi));
    } });
    runSteps (ctx, steps, [&ctx, &session]
    {
        ctx.waitUntil ([&session] { return session.midiLearnPending.load() < 0; }, 3000,
            [&ctx, &session]
            {
                const auto& bindingsNow = session.midiBindings.current();
                ctx.expect (std::any_of (bindingsNow.begin(), bindingsNow.end(), [] (const MidiBinding& binding)
                {
                    return binding.channel == 3 && binding.dataNumber == 74 && binding.trigger == MidiBindingTrigger::CC
                        && binding.target == MidiBindingTarget::TrackPluginParam && binding.targetIndex == 0 && binding.paramIndex == 0;
                }), "captured CC did not bind the last-touched Gain parameter");
                ctx.complete (ctx.verdict());
            }, "MIDI Learn did not consume the injected CC");
    });
    return std::nullopt;
}

const ScenarioRegistrar builtinMidiLearn { Scenario {
    "gui.builtin_midi_learn", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBuiltinMidiLearn (host, ctx); }
} };

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
        ctx.expect (state.size() == 4 && state[0] == expected, "navigation did not select expected region");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [drag] { drag ("start", 0, 12000, 0, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 36000, 0); drag ("end", 48000, 36000, 0, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 0); drag ("gain", 24000, 24000, -60, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 6); drag ("gain", 24000, 24000, -100, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, 12); drag ("gain", 24000, 24000, 400, false); } });
    steps->push_back ({ 150, [checkRegion, drag] { checkRegion (12000, 24000, -24); drag ("wave", 18000, 30000, 0, true); } });
    steps->push_back ({ 150, [&host, &ctx, navigate]
    {
        const auto state = host.audioEditorSelection();
        ctx.expect (state.size() == 4 && state[1] == 1 && std::abs (state[2] - 18000) < 128
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
    steps->push_back ({ 150, [gain, normalized, &host, &ctx]
    {
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

std::optional<ScenarioResult> runMasteringExportWorkflow (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalFile = player.getLoadedFile();
    const auto originalPosition = player.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, &player, originalDir, originalStage, originalFile, originalPosition, restore]
    {
        engine.observeAudioRegistrationForScenario ({});
        drainModals (host);
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        host.refreshMasteringSource();
        host.switchToStage (guiStage (originalStage));
    });
    const auto source = ctx.tempDir() / "mixdown.wav";
    const auto output = ctx.tempDir() / "Finished master.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 2;
    auto writer = dusk::audio::FileWriter::create (source, spec);
    std::vector<float> signal (4800);
    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = 0.1f * (float) std::sin (6.283185307179586 * 440.0 * (double) i / 48000.0);
    const float* channels[] = { signal.data(), signal.data() };
    if (! writer || ! writer->write (channels, 2, 4800) || ! writer->flush())
        return ScenarioResult::fail ("could not write mixdown");
    writer.reset();
    applySessionDirectory (session, ctx.tempDir());
    host.switchToStage (GuiHost::Stage::Mastering);
    auto registration = std::make_shared<std::vector<bool>>();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "latest mixdown button unavailable"); } });
    steps->push_back ({ 600, [&host, &ctx, &engine, &player, source, registration]
    {
        ctx.expect (player.isLoaded() && player.getLoadedFile().getFullPathName().toStdString() == source.string(),
                    "latest mixdown did not load the session mix");
        ctx.expect (engine.isAudioCallbackRegistered(), "audio callback was not registered before export");
        engine.observeAudioRegistrationForScenario ([registration] (bool attached) { registration->push_back (attached); });
        ctx.expect (host.clickMasteringButton ("Export master..."), "export button unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("WAV 24-bit \xe2\x80\x94 session rate"), "archive preset unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx, output]
    {
        ctx.expect (host.focusFileName(), "export file browser unavailable");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : output.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Save"), "export destination was not accepted"); } });
    runSteps (ctx, steps, [&host, &ctx, &engine, output, registration]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000,
            [&ctx, &engine, output, registration]
            {
                ctx.expect (*registration == std::vector<bool> { false, true },
                            "export did not deregister and restore the live audio callback");
                ctx.expect (engine.isAudioCallbackRegistered(), "audio callback was not registered after export");
                engine.observeAudioRegistrationForScenario ({});
                auto reader = dusk::audio::FileReader::open (output);
                if (ctx.expect (reader != nullptr, "master export did not create a readable WAV"))
                {
                    const auto& info = reader->info();
                    ctx.expect (info.numChannels == 2 && info.bitsPerSample == 24, "export format is not stereo 24-bit WAV");
                    ctx.expect (std::abs (info.sampleRate - engine.getCurrentSampleRate()) < 0.01,
                                "archive export did not use session rate");
                    ctx.expect (info.numFrames == (std::int64_t) std::llround (info.sampleRate * 5.1),
                                "export length does not include source and tail");
                    std::vector<float> left (4096), right (4096);
                    float* channelsOut[] = { left.data(), right.data() };
                    ctx.expect (reader->read (channelsOut, 2, 0, 4096) == 4096, "could not read exported audio");
                    const auto peak = *std::max_element (left.begin(), left.end());
                    ctx.expect (peak > 0.001f && peak <= 1.0f, "export did not carry the loaded mix signal");
                }
                ctx.complete (ctx.verdict());
            }, "master export did not finish");
    });
    return std::nullopt;
}

const ScenarioRegistrar masteringExportWorkflow { Scenario {
    "gui.mastering_export_workflow", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringExportWorkflow (host, ctx); }
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

std::optional<ScenarioResult> runMultiImportTargets (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    for (const int index : { 22, 23 })
    {
        const auto& track = session.track (index);
        if (! track.regions.empty() || track.frozen.load() || track.mode.load() != (int) Track::Mode::Mono)
            return ScenarioResult::skip ("requires empty mono destination tracks");
    }
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.setTimelineShown (shown);
    });
    const auto importDir = ctx.tempDir() / "session";
    std::filesystem::create_directories (importDir / "audio");
    applySessionDirectory (session, importDir);
    std::vector<std::filesystem::path> files;
    for (int index = 0; index < 2; ++index)
    {
        files.push_back (ctx.tempDir() / (index == 0 ? "First stem.wav" : "Second stem.wav"));
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 1;
        auto writer = dusk::audio::FileWriter::create (files.back(), spec);
        std::vector<float> silence ((size_t) (2400 * (index + 1)));
        const float* data[] = { silence.data() };
        if (! writer || ! writer->write (data, 1, (std::int64_t) silence.size()) || ! writer->flush())
            return ScenarioResult::fail ("could not write multi-import fixtures");
    }
    const auto rows = [&ctx, &host] (int first, int second)
    {
        ctx.expect (host.multiImportRows() == std::vector<std::string> {
            "First stem.wav\t" + std::to_string (first), "Second stem.wav\t" + std::to_string (second) },
            "multi-import file labels or selected tracks are wrong");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx, files]
    { ctx.expect (host.dropFilesOnTrack (0, files), "multi-file drop was rejected"); } });
    steps->push_back ({ 250, [&host, &ctx, rows]
    {
        rows (-1, -1);
        ctx.expect (! host.clickModalButton ("Import"), "unassigned files can be imported");
        ctx.expect (host.clickModalButton ("Auto-assign"), "Auto-assign is unavailable");
    } });
    steps->push_back ({ 200, [&host, &ctx, rows]
    {
        rows (0, 1);
        ctx.expect (host.clickModalButton ("Clear"), "Auto-assign did not become Clear");
    } });
    steps->push_back ({ 200, [&host, &ctx, rows]
    {
        rows (-1, -1);
        ctx.expect (! host.clickModalButton ("Import"), "Clear left Import enabled");
    } });
    for (int row = 0; row < 2; ++row)
    {
        steps->push_back ({ 200, [&host, &ctx, row]
        { ctx.expect (host.clickMultiImportTarget (row), "file target dropdown is unavailable"); } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.pressPeerKey ("end", 0), "dropdown End key was not handled"); } });
        steps->push_back ({ 150, [&host, &ctx]
        { ctx.expect (host.pressPeerKey ("return", '\r'), "dropdown Return key was not handled"); } });
    }
    steps->push_back ({ 200, [&host, &ctx, rows]
    {
        rows (23, 22);
        ctx.expect (host.clickModalButton ("Import"), "assigned files cannot be imported");
    } });
    steps->push_back ({ 700, [&ctx, &host, &session, &engine]
    {
        ctx.expect (host.modalStackEmpty(), "multi-import left a modal open");
        for (int index = 0; index < 2; ++index)
        {
            const auto& regions = session.track (23 - index).regions;
            if (! ctx.expect (regions.size() == 1, "chosen destination did not receive one region")) continue;
            const auto expected = (std::int64_t) std::llround (2400.0 * (index + 1) * engine.getCurrentSampleRate() / 48000.0);
            ctx.expect (regions[0].file.existsAsFile() && regions[0].numChannels == 1
                        && regions[0].lengthInSamples == expected, "wrong source reached the assigned track");
        }
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar multiImportTargets { Scenario {
    "gui.multi_import_targets", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMultiImportTargets (host, ctx); }
} };

std::optional<ScenarioResult> runImportModeConfirmation (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (0);
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty() || ! track.regions.empty() || ! track.midiRegions.current().empty() || track.frozen.load())
        return ScenarioResult::skip ("requires an empty unfrozen track and no modal");
    if (engine.getStage() != AudioEngine::Stage::Recording && engine.getStage() != AudioEngine::Stage::Mixing)
        return ScenarioResult::skip ("requires a stage with the timeline");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    const bool shown = host.setTimelineShown (true);
    ctx.cleanup ([&host, &session, originalDir, restore, shown]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.setTimelineShown (shown);
    });
    const auto importDir = ctx.tempDir() / "session";
    std::filesystem::create_directories (importDir / "audio");
    applySessionDirectory (session, importDir);
    track.mode.store ((int) Track::Mode::Mono);
    for (int channels = 1; channels <= 2; ++channels)
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = channels;
        auto writer = dusk::audio::FileWriter::create (ctx.tempDir() / (channels == 1 ? "Mono.wav" : "Stereo.wav"), spec);
        std::array<float, 4800> silence {};
        const float* data[] = { silence.data(), silence.data() };
        if (! writer || ! writer->write (data, channels, 4800) || ! writer->flush())
            return ScenarioResult::fail ("could not write audio import fixture");
    }
    const unsigned char midi[] = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xe0,
        'M', 'T', 'r', 'k', 0, 0, 0, 13,
        0, 0x90, 60, 100, 0x83, 0x60, 0x80, 60, 0, 0, 0xff, 0x2f, 0
    };
    std::ofstream output (ctx.tempDir() / "Notes.mid", std::ios::binary);
    output.write (reinterpret_cast<const char*> (midi), sizeof (midi));
    output.close();
    if (! output) return ScenarioResult::fail ("could not write MIDI import fixture");
    struct Leg { const char* file; const char* from; const char* to; Track::Mode mode; bool midi; };
    const std::array<Leg, 3> legs {{
        { "Stereo.wav", "Mono", "Stereo", Track::Mode::Stereo, false },
        { "Notes.mid", "Stereo", "MIDI", Track::Mode::Midi, true },
        { "Mono.wav", "MIDI", "Mono", Track::Mode::Mono, false }
    }};
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto leg : legs)
    {
        auto before = std::make_shared<std::array<int, 3>>();
        steps->push_back ({ 250, [&host, &ctx, &track, leg, before]
        {
            *before = { track.mode.load(), (int) track.regions.size(), (int) track.midiRegions.current().size() };
            ctx.expect (host.dropFilesOnTrack (0, { ctx.tempDir() / leg.file }), "file drop was rejected");
        } });
        const auto unchanged = [&ctx, &track, before]
        {
            ctx.expect (track.mode.load() == (*before)[0] && (int) track.regions.size() == (*before)[1]
                        && (int) track.midiRegions.current().size() == (*before)[2], "unconfirmed import changed the track");
        };
        for (const bool accept : { false, true })
        {
            steps->push_back ({ 250, [&host, &ctx]
            { ctx.expect (host.clickModalButton ("Import"), "target picker Import button is unavailable"); } });
            steps->push_back ({ 250, [&host, &ctx, leg, unchanged, accept]
            {
                const auto text = host.confirmationText();
                const std::string title = std::string ("Switch track to ") + leg.to + "?";
                const std::string message = std::string ("Track 1 is currently in ") + leg.from
                    + " mode. Importing this " + (leg.midi ? "MIDI" : "audio")
                    + " file will switch the track to " + leg.to + " mode. Proceed?";
                ctx.expect (text == std::vector<std::string> { title, message }, "mode-switch confirmation text is wrong");
                unchanged();
                ctx.expect (host.clickModalButton (accept ? "Switch" : "Cancel"), "mode-switch action is unavailable");
            } });
            if (! accept) steps->push_back ({ 250, unchanged });
        }
        steps->push_back ({ 500, [&ctx, &host, &track, leg, before]
        {
            ctx.expect (host.modalStackEmpty(), "confirmed import left a modal open");
            ctx.expect (track.mode.load() == (int) leg.mode, "Switch did not change track mode");
            ctx.expect ((int) track.regions.size() == (*before)[1] + (leg.midi ? 0 : 1)
                        && (int) track.midiRegions.current().size() == (*before)[2] + (leg.midi ? 1 : 0),
                        "Switch did not import exactly one region");
            if (leg.midi && ! track.midiRegions.current().empty())
                ctx.expect (track.midiRegions.current().back().notes.size() == 1, "imported MIDI note is missing");
            if (! leg.midi && ! track.regions.empty())
                ctx.expect (track.regions.back().file.existsAsFile()
                            && track.regions.back().numChannels == (leg.mode == Track::Mode::Stereo ? 2 : 1),
                            "imported audio file or channel layout is wrong");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar importModeConfirmation { Scenario {
    "gui.import_mode_confirmation", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runImportModeConfirmation (host, ctx); }
} };

std::optional<ScenarioResult> runDpImportConfirmation (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! ctx.engine().getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no open modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save initial session");
    ctx.cleanup ([&host, &session, originalDir, restore]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    for (const auto* name : { "ZZ0000_1.wav", "ZZ0001_1.wav", "ZZ0001_2.wav", "ZZ0003_2.wav" })
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 1;
        spec.bitsPerSample = 16;
        auto writer = dusk::audio::FileWriter::create (ctx.tempDir() / name, spec);
        std::array<float, 256> silence {};
        const float* channels[] = { silence.data() };
        if (! writer || ! writer->write (channels, 1, 256) || ! writer->flush())
            return ScenarioResult::fail ("could not create DP audio fixture");
    }
    const auto read = [] (const std::filesystem::path& path)
    {
        std::ifstream input (path);
        return std::string (std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>());
    };
    const auto before = read (restore);
    const auto unchanged = [&ctx, &session, before, read]
    {
        const auto after = ctx.tempDir() / "after.json";
        ctx.expect (SessionSerializer::save (session, after), "could not save comparison snapshot");
        ctx.expect (! before.empty() && read (after) == before, "DP confirmation changed the session before import");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickFileMenu(), "File menu is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Import DP Song (experimental)..."), "DP import menu item is unavailable"); } });
    steps->push_back ({ 250, [&host, &ctx]
    {
        ctx.expect (host.focusFileName(), "DP file browser did not open");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : (ctx.tempDir() / "ZZ0000_1.wav").string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
        ctx.expect (host.clickModalButton ("Open"), "DP browser Open button is unavailable");
    } });
    steps->push_back ({ 500, [&host, &ctx, unchanged]
    {
        const auto text = host.dpImportSummary();
        if (ctx.expect (text.size() == 3, "DP confirmation is not visible and ready to import"))
        {
            ctx.expect (text[0] == "Import DP Song", "DP confirmation title is wrong");
            for (const auto* part : { "3 tracks", "1 stereo pair", "48.0 kHz / 16-bit" })
                ctx.expect (text[1].find (part) != std::string::npos, std::string ("DP summary omits ") + part);
            ctx.expect (text[2].find ("ZZ0003: right channel without left; imported as mono.") != std::string::npos,
                        "DP confirmation omits the orphan-channel warning");
        }
        unchanged();
        ctx.expect (host.clickModalButton ("Cancel"), "DP confirmation Cancel is unavailable");
    } });
    steps->push_back ({ 250, [&host, &ctx, unchanged]
    {
        ctx.expect (host.modalStackEmpty(), "Cancel left the DP confirmation open");
        unchanged();
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar dpImportConfirmation { Scenario {
    "gui.dp_import_confirmation", { "gui", "import" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runDpImportConfirmation (host, ctx); }
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
    const auto select = [&host, &ctx] (std::int64_t tick, int pitch, int modifiers, std::vector<int> expected)
    {
        ctx.expect (host.pianoNotePointer (tick, pitch, true, modifiers), "note click failed");
        host.pianoNotePointer (tick, pitch, false, modifiers);
        auto actual = host.pianoSelection();
        std::sort (actual.begin(), actual.end());
        ctx.expect (actual == expected, "note click produced the wrong selection");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("G", 'g'), "Grab key was not handled"); } });
    steps->push_back ({ 150, [select] { select (1320, 60, 0, { 0 }); } });
    steps->push_back ({ 150, [select] { select (2520, 64, 1, { 0, 1 }); } });
    steps->push_back ({ 150, [select] { select (2520, 64, 1, { 0 }); } });
    steps->push_back ({ 150, [select] { select (2520, 64, 2, { 0, 1 }); } });
    steps->push_back ({ 150, [select] { select (1320, 60, 2, { 1 }); } });
    steps->push_back ({ 150, [select] { select (1320, 60, 0, { 0 }); } });
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

// ---------------------------------------------------------------- autosave

bool nearly (float a, float b) { return std::abs (a - b) < 1.0e-4f; }

// Track 1's fader as a session file holds it; a large sentinel when the file
// will not load.
float savedFaderOf (const std::filesystem::path& sessionJson)
{
    Session probe;
    if (! SessionSerializer::load (probe, sessionJson)) return 1000.0f;
    return probe.track (0).strip.faderDb.load (std::memory_order_relaxed);
}

std::optional<ScenarioResult> runSplitModuleButtons (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    const bool originalCompact = host.setStripCompact (0, true);
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore, originalCompact]
    {
        host.closeStripModuleEditors (0);
        drainModals (host);
        host.setStripCompact (0, originalCompact);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    auto& params = session.track (0).strip;
    params.eqEnabled.store (true);
    params.compEnabled.store (true);
    params.auxSendsBypassed.store (false);
    for (int send = 0; send < 4; ++send) params.auxSendDb[(size_t) send].store (-3.0f * (send + 1));
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool compact : { true, false })
    {
        steps->push_back ({ 200, [&host, compact] { host.setStripCompact (0, compact); } });
        for (int module = 0; module < (compact ? 3 : 2); ++module)
        {
            for (const bool bypassed : { true, false })
            {
                steps->push_back ({ 200, [&host, &ctx, module]
                { ctx.expect (host.clickStripModule (0, module, false, false), "status light is unavailable"); } });
                steps->push_back ({ 200, [&host, &ctx, &params, module, bypassed]
                {
                    const bool actual = module == 0 ? ! params.eqEnabled.load()
                                      : module == 1 ? ! params.compEnabled.load() : params.auxSendsBypassed.load();
                    ctx.expect (actual == bypassed, "status light did not toggle section bypass");
                    for (int editor = 0; editor < 3; ++editor)
                        ctx.expect (! host.stripModuleEditorOpen (0, editor), "status light opened an editor");
                    for (int send = 0; send < 4; ++send)
                        ctx.expect (std::abs (params.auxSendDb[(size_t) send].load() + 3.0f * (send + 1)) < 0.001f,
                                    "bypass changed a send level");
                } });
            }
            steps->push_back ({ 200, [&host, &ctx, module]
            { ctx.expect (host.clickStripModule (0, module, true, false), "module label is unavailable"); } });
            steps->push_back ({ 500, [&host, &ctx, module]
            {
                ctx.expect (host.stripModuleEditorOpen (0, module), "module label did not open its editor");
                host.closeStripModuleEditors (0);
            } });
            for (const bool label : { false, true })
            {
                steps->push_back ({ 200, [&host, &ctx, module, label]
                { ctx.expect (host.clickStripModule (0, module, label, true), "module right-click failed"); } });
                steps->push_back ({ 200, [&host, &ctx, module]
                { ctx.expect (host.clickContextMenuItem (module == 2 ? "Open AUX editor..." : "Open editor..."),
                              "section menu did not offer its editor"); } });
                steps->push_back ({ 500, [&host, &ctx, module]
                {
                    ctx.expect (host.stripModuleEditorOpen (0, module), "section menu did not open its editor");
                    host.closeStripModuleEditors (0);
                } });
            }
        }
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar splitModuleButtons { Scenario {
    "gui.split_module_buttons", { "gui", "strip" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSplitModuleButtons (host, ctx); }
} };

std::optional<ScenarioResult> runInsertContextMenu (GuiHost& host, ScenarioContext& ctx)
{
    const auto fixture = ctx.fixture ("relayout.vst3");
    if (! fixture) return ScenarioResult::skip ("requires the VST3 fixture");
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& strip = engine.getChannelStrip (0);
    auto& slot = strip.getPluginSlot();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (slot.isLoaded()) return ScenarioResult::skip ("requires an empty standard plugin slot");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        if (auto* component = host.strip (0)) component->closeEditor();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    auto* component = readyStrip (host, ctx);
    if (component == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto steps = std::make_shared<std::vector<Step>>();
    const auto menu = [&host, &ctx, steps] (const std::string& label)
    {
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickInsert (0, true), "insert did not receive a right-click"); } });
        steps->push_back ({ 200, [&host, &ctx, label]
        { ctx.expect (host.clickContextMenuItem (label), "insert menu item is unavailable: " + label); } });
    };
    const auto button = [&host, &ctx, steps] (const std::string& label)
    {
        steps->push_back ({ 200, [&host, &ctx, label]
        { ctx.expect (host.clickModalButton (label), "insert dialog button is unavailable: " + label); } });
    };
    menu ("Add insert...");
    button ("Plugin (VST3 / CLAP / LV2 / AU)");
    button ("Browse file...");
    steps->push_back ({ 200, [&host, &ctx, fixture]
    {
        ctx.expect (host.focusFileName(), "insert file browser did not open");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : fixture->string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    button ("Open");
    steps->push_back ({ 1200, [&ctx, &slot, component]
    {
        ctx.expect (slot.isLoadedStandardVst3(), "Add insert did not load the VST3 fixture");
        component->closeEditor();
    } });
    menu ("Open editor");
    steps->push_back ({ 500, [&ctx, component]
    {
        ctx.expect (component->hasOpenEditor(), "Open editor did not open the loaded plugin");
        component->closeEditor();
    } });
    menu ("Replace insert...");
    button ("Cancel");
    steps->push_back ({ 200, [&ctx, &slot]
    { ctx.expect (slot.isLoadedStandardVst3(), "cancelling Replace removed the plugin"); } });
    menu ("Remove plugin");
    steps->push_back ({ 300, [&ctx, &slot, component]
    {
        ctx.expect (! slot.isLoaded(), "Remove plugin left the plugin loaded");
        ctx.expect (! component->hasOpenEditor(), "Remove plugin left its editor open");
    } });
    menu ("Add insert...");
    button ("Hardware Insert");
    steps->push_back ({ 300, [&ctx, &strip]
    { ctx.expect (strip.insertMode.load() == ChannelStrip::kInsertHardware, "Hardware Insert did not change the insert mode"); } });
    button ("Done");
    menu ("Edit hardware insert...");
    button ("Done");
    menu ("Replace insert...");
    button ("Cancel");
    steps->push_back ({ 200, [&ctx, &strip]
    { ctx.expect (strip.insertMode.load() == ChannelStrip::kInsertHardware, "cancelling Replace removed the hardware insert"); } });
    menu ("Remove hardware insert");
    steps->push_back ({ 300, [&ctx, &session, &strip]
    {
        ctx.expect (strip.insertMode.load() == ChannelStrip::kInsertEmpty, "Remove hardware insert did not empty the slot");
        ctx.expect (! session.track (0).hardwareInsert.enabled.load(), "removed hardware insert remains enabled");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar insertContextMenu { Scenario {
    "gui.insert_context_menu", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    { "relayout.vst3" }, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runInsertContextMenu (host, ctx); }
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
    { ctx.expect (host.focusPiano() && host.pressPeerKey ("Q", 'q'), "Q was not handled"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("1/16 @ 75%"), "75-percent quantize row was not clicked"); } });
    steps->push_back ({ 150, [&host, &ctx, checkTicks]
    {
        checkTicks (12, 229);
        ctx.expect (host.focusPiano() && host.pressPeerKey ("Q", 'q'), "second Q was not handled");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("1/16 @ 100%"), "full-strength quantize row was not clicked"); } });
    steps->push_back ({ 150, [&host, &ctx, checkTicks]
    {
        checkTicks (0, 240);
        ctx.expect (host.focusPiano() && host.pressPeerKey ("S", 's'), "S was not handled");
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
        { ctx.expect (host.focusPiano() && host.pressPeerKey ("L", 'l'), "L was not handled"); } });
        steps->push_back ({ 100, [&host, &ctx, controller]
        { ctx.expect (host.pianoOptions()[2] == controller, "L cycled to the wrong CC"); } });
    }
    for (const int colour : { 1, 2, 0 })
    {
        steps->push_back ({ 0, [&host, &ctx]
        { ctx.expect (host.focusPiano() && host.pressPeerKey ("C", 'c'), "C was not handled"); } });
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
        ctx.expect (host.scrollPiano (-1.0f, false, false), "vertical wheel was not delivered");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        const auto after = host.pianoViewport();
        ctx.expect (after[2] > (*before)[2] && std::abs (after[0] - (*before)[0]) < 1.0e-6,
                    "unmodified wheel did not scroll the pitch range");
        *before = after;
        ctx.expect (host.scrollPiano (-1.0f, false, true), "Shift wheel was not delivered");
    } });
    steps->push_back ({ 150, [&host, &ctx, before]
    {
        const auto after = host.pianoViewport();
        ctx.expect (after[1] > (*before)[1] && std::abs (after[2] - (*before)[2]) < 0.1,
                    "Shift wheel did not scroll horizontally");
        *before = after;
        ctx.expect (host.scrollPiano (1.0f, true, false), "command wheel was not delivered");
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
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (! host.virtualKeyboardOpen(), "K did not close the virtual keyboard"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pianoStepRecord { Scenario {
    "gui.piano_step_record", { "gui", "piano", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPianoStepRecord (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringTargets (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& meters = ctx.session().mastering();
    if (! engine.getTransport().isStopped() || engine.getMasteringPlayer().isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    const auto originalStage = engine.getStage();
    const auto originalTarget = meters.targetPresetIndex.load();
    const auto originalIntegrated = meters.meterIntegratedLufs.load();
    const auto originalPeak = meters.meterTruePeakDb.load();
    host.switchToStage (GuiHost::Stage::Mastering);
    engine.suspendProcessing();
    ctx.cleanup ([&host, &engine, &meters, originalStage, originalTarget, originalIntegrated, originalPeak]
    {
        drainModals (host);
        host.restoreMasteringTarget (originalTarget);
        meters.targetPresetIndex.store (originalTarget);
        meters.meterIntegratedLufs.store (originalIntegrated);
        meters.meterTruePeakDb.store (originalPeak);
        engine.resumeProcessing();
        host.switchToStage (guiStage (originalStage));
    });
    const std::array<const char*, 6> names { "Off", "Spotify", "Apple Music", "YouTube", "Tidal", "Broadcast (EBU R128)" };
    const std::array<float, 6> targets { 0.0f, -14.0f, -16.0f, -14.0f, -14.0f, -23.0f };
    auto steps = std::make_shared<std::vector<Step>>();
    for (int index = 0; index < (int) names.size(); ++index)
    {
        steps->push_back ({ 100, [&host, &ctx]
        { ctx.expect (host.clickMasteringTarget(), "mastering target picker did not open"); } });
        steps->push_back ({ 300, [&host, &ctx, index]
        {
            ctx.expect (host.clickModalAt (0.5f, (4.0f + 26.0f * (static_cast<float> (index) + 0.5f)) / 164.0f),
                        "mastering target row did not receive a click");
        } });
        steps->push_back ({ 300, [&host, &ctx, &meters, names, index]
        {
            ctx.expect (meters.targetPresetIndex.load() == index, "target picker expected " + std::to_string (index) + " but selected " + std::to_string (meters.targetPresetIndex.load()));
            ctx.expect (host.masteringTargetText().find (names[(std::size_t) index]) == 0,
                        "target picker label differs from selected platform: " + host.masteringTargetText());
        } });
        for (int band = 0; band < 4; ++band)
        {
            steps->push_back ({ 0, [&meters, targets, index, band]
            {
                const std::array<float, 4> offsets { 0.5f, 2.0f, 2.1f, -100.0f };
                meters.meterIntegratedLufs.store (band == 3 ? -100.0f : targets[(std::size_t) index] + offsets[(std::size_t) band]);
                meters.meterTruePeakDb.store (band == 3 ? -100.0f : band == 0 ? -1.0f : -0.9f);
            } });
            steps->push_back ({ 100, [&host, &ctx, index, band]
            {
                const bool neutral = index == 0 || band == 3;
                const std::array<std::uint32_t, 3> colours { 0xff1a3a1a, 0xff3a3a1a, 0xff3a1a1a };
                ctx.expect (host.masteringLoudnessColour (false) == (neutral ? 0xff1a2228 : colours[(std::size_t) band]),
                            "integrated loudness colour differs from target band");
                ctx.expect (host.masteringLoudnessColour (true) == (neutral ? 0xff121214 : band == 0 ? 0xff1a3a1a : 0xff3a1a1a),
                            "true-peak colour differs from platform ceiling");
            } });
        }
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringTargets { Scenario {
    "gui.mastering_targets", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringTargets (host, ctx); }
} };

std::optional<ScenarioResult> runMasteringLoad (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& player = engine.getMasteringPlayer();
    if (! engine.getTransport().isStopped() || player.isPlaying())
        return ScenarioResult::skip ("requires stopped transport and mastering player");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto originalFile = player.getLoadedFile();
    const auto originalPosition = player.getPlayhead();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &player, originalDir, originalStage, originalFile, originalPosition, restore]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        host.refreshMasteringSource();
        host.switchToStage (guiStage (originalStage));
    });
    const auto write = [] (const std::filesystem::path& path, int frames)
    {
        dusk::audio::WriteSpec spec;
        spec.sampleRate = 48000;
        spec.numChannels = 2;
        auto writer = dusk::audio::FileWriter::create (path, spec);
        std::vector<float> silence (static_cast<std::size_t> (frames));
        const float* channels[] = { silence.data(), silence.data() };
        return writer && writer->write (channels, 2, frames) && writer->flush();
    };
    const auto chosen = ctx.tempDir() / "Chosen mix.wav";
    const auto bounce = ctx.tempDir() / "bounce.wav";
    const auto mixdown = ctx.tempDir() / "mixdown.wav";
    if (! write (chosen, 4800) || ! write (bounce, 9600))
        return ScenarioResult::fail ("could not write the mastering load fixtures");
    applySessionDirectory (session, ctx.tempDir());
    host.switchToStage (GuiHost::Stage::Mastering);
    const auto check = [&ctx, &session, &player] (const std::filesystem::path& path, int frames)
    {
        ctx.expect (player.isLoaded(), "mastering player is not loaded");
        ctx.expect (player.getLoadedFile().getFullPathName().toStdString() == path.string(),
                    "mastering player loaded the wrong file");
        ctx.expect (session.mastering().sourceFile.getFullPathName().toStdString() == path.string(),
                    "session did not retain the loaded source path");
        ctx.expect (player.getLengthSamples() == frames, "mastering source length differs from fixture");
        ctx.expect (std::abs (player.getSourceSampleRate() - 48000.0) < 0.01,
                    "mastering source rate differs from fixture");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load mix..."), "Load mix button is unavailable"); } });
    steps->push_back ({ 200, [&host, &ctx, chosen]
    {
        ctx.expect (host.focusFileName(), "mastering file browser did not open");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : chosen.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the mix path"); } });
    steps->push_back ({ 600, [&host, &ctx, check, chosen]
    {
        check (chosen, 4800);
        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "Load latest mixdown button is unavailable");
    } });
    steps->push_back ({ 600, [&host, &ctx, check, write, bounce, mixdown]
    {
        check (bounce, 9600);
        ctx.expect (write (mixdown, 14400), "could not write the preferred mixdown");
        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "second latest-mixdown click failed");
    } });
    steps->push_back ({ 600, [check, mixdown] { check (mixdown, 14400); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringLoad { Scenario {
    "gui.mastering_load", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringLoad (host, ctx); }
} };

std::optional<ScenarioResult> runAuxSources (GuiHost& host, ScenarioContext& ctx)
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
        if (auto* lane = host.auxLane (0)) lane->captureSources (false);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        auto& track = session.track (i);
        track.name = i == 0 ? "Lead voice" : std::to_string (i + 1);
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        track.strip.auxSendDb[0].store (i % 2 == 0 ? -6.0f : ChannelStripParams::kAuxSendOffDb);
        track.strip.auxSendDb[1].store (-3.0f);
    }
    host.switchToStage (GuiHost::Stage::Aux);
    auto* lane = host.auxLane (0);
    if (lane == nullptr || ! lane->captureSources (true))
        return ScenarioResult::fail ("first aux Sources panel is not showing");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 500, [lane, &ctx, &session]
    {
        const auto rows = lane->sourceRows();
        ctx.expect (rows.size() == Session::kNumTracks, "Sources did not paint all 24 channel rows");
        for (int i = 0; i < Session::kNumTracks && i < (int) rows.size(); ++i)
        {
            const auto number = std::to_string (i + 1);
            const auto expected = number + "\t" + (i == 0 ? "Lead voice" : "Trk " + number)
                                  + (i % 2 == 0 ? "\t-6.0 dB\tmeter" : "\t-inf\tmeter");
            ctx.expect (rows[(std::size_t) i] == expected, "unexpected source row: " + rows[(std::size_t) i]);
        }
        session.track (0).strip.auxSendDb[0].store (ChannelStripParams::kAuxSendOffDb);
        session.track (23).strip.auxSendDb[0].store (-3.5f);
        session.track (23).name = "Last channel";
    } });
    steps->push_back ({ 500, [lane, &ctx]
    {
        const auto rows = lane->sourceRows();
        ctx.expect (! rows.empty() && rows.front() == "1\tLead voice\t-inf\tmeter",
                    "Sources did not refresh a disabled send");
        ctx.expect (rows.size() == Session::kNumTracks && rows.back() == "24\tLast channel\t-3.5 dB\tmeter",
                    "Sources did not refresh the last channel name and send level");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar auxSources { Scenario {
    "gui.aux_sources", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxSources (host, ctx); }
} };

std::optional<ScenarioResult> runSoundfontConversion (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_MULTISAMPLE
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without multisample instruments");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto& strip = engine.getChannelStrip (0);
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (track.mode.load() == (int) Track::Mode::Midi || track.midiInputIndex.load() >= 0)
        return ScenarioResult::skip ("requires an audio track with no MIDI input");
    if (strip.getPluginSlot().isLoaded()) return ScenarioResult::skip ("requires an empty standard plugin slot");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    const auto soundfont = ctx.tempDir() / "Conversion fixture.sfz";
    if (! SessionSerializer::save (session, restore)
        || ! dusk::fs::writeStringToFile (soundfont, "<region> key=60 sample=*sine\n"))
        return ScenarioResult::fail ("could not prepare the soundfont fixture");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        if (auto* component = host.strip (0)) component->closeEditor();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickInsert (0), "insert button did not receive a click"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Soundfont (.sfz / .sf2 / .bank.xml)"), "soundfont chooser did not open"); } });
    steps->push_back ({ 150, [&host, &ctx, soundfont]
    {
        ctx.expect (host.focusFileName(), "soundfont filename entry was not available");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : soundfont.string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the soundfont path"); } });
    steps->push_back ({ 1200, [&ctx, &engine, &track, &strip, soundfont]
    {
        ctx.expect (strip.isNativeMultisampleLoaded(), "soundfont was not loaded");
        ctx.expect (! strip.nativeMultisampleReloadFailed(), "soundfont reported a load failure");
        ctx.expect (strip.getNativeMultisampleSlot().getLoadedSoundfontPath() == soundfont.string(),
                    "loaded soundfont path differs from the chosen file");
        ctx.expect (track.nativeMultisamplePath.toStdString() == soundfont.string(),
                    "session did not retain the soundfont path");
        ctx.expect (track.mode.load() == (int) Track::Mode::Midi, "audio track did not convert to MIDI");
        ctx.expect (engine.getVirtualKeyboardInputIndex() >= 0
                    && track.midiInputIndex.load() == engine.getVirtualKeyboardInputIndex(),
                    "converted track did not select the virtual keyboard");
        ctx.expect (track.inputMonitor.load(), "converted track did not enable input monitoring");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar soundfontConversion { Scenario {
    "gui.soundfont_conversion", { "gui", "plugins", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSoundfontConversion (host, ctx); }
} };

std::optional<ScenarioResult> runPluginBrowseFile (GuiHost& host, ScenarioContext& ctx)
{
    const auto fixture = ctx.fixture ("relayout.vst3");
    if (! fixture) return ScenarioResult::skip ("requires the VST3 fixture");
    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    auto& session = ctx.session();
    auto& slot = engine.getChannelStrip (0).getPluginSlot();
    if (slot.isLoaded()) return ScenarioResult::skip ("requires an empty standard plugin slot");
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        if (auto* strip = host.strip (0)) strip->closeEditor();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickInsert (0), "insert button did not receive a click"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Plugin (VST3 / CLAP / LV2 / AU)"), "insert chooser did not open"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Browse file..."), "picker did not offer Browse file"); } });
    steps->push_back ({ 150, [&host, &ctx, fixture]
    {
        ctx.expect (host.focusFileName(), "file browser filename entry was not available");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (const char ch : fixture->string())
            host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Open"), "Open did not accept the fixture path"); } });
    steps->push_back ({ 1200, [&host, &ctx, &slot]
    {
        ctx.expect (slot.isLoaded(), "Browse file did not load a plugin");
        ctx.expect (slot.isLoadedStandardVst3(), "Browse file did not use the VST3 loader");
        ctx.expect (slot.getLoadedName().toStdString() == "Dusk Runtime Relayout Fixture",
                    "Browse file loaded a different plugin");
        ctx.expect (! slot.isLoadedPluginInstrument(), "effect Browse loaded an instrument");
        ctx.expect (! host.focusFileName(), "file browser remained open after accepting the path");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pluginBrowseFile { Scenario {
    "gui.plugin_browse_file", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    { "relayout.vst3" }, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginBrowseFile (host, ctx); }
} };

std::optional<ScenarioResult> runPluginPicker (GuiHost& host, ScenarioContext& ctx)
{
    const auto fixture = ctx.fixture ("multi_bus.clap");
    const auto descriptions = ctx.engine().getPluginManager().getClapEffectDescriptions();
    if (! fixture || std::none_of (descriptions.begin(), descriptions.end(), [] (const auto& row)
        { return row.name == "Picker Alpha"; }))
        return ScenarioResult::skip ("requires the seeded plugin-picker cache");
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const auto originalDir = currentSessionDirectory (session);
    const auto originalStage = engine.getStage();
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, originalDir, originalStage, restore]
    {
        drainModals (host);
        if (auto* strip = host.strip (0)) { strip->closeEditor(); strip->unloadNativePlugins(); }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    const auto filter = [&host, &ctx] (const std::string& value)
    {
        ctx.expect (host.clickModalAt (0.3f, 0.1f), "picker filter did not receive focus");
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        host.pressPeerKey ("Backspace");
        for (const char ch : value) host.pressPeerKey (std::string (1, ch), ch);
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickInsert (0), "insert button did not receive a click"); } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickModalButton ("Plugin (VST3 / CLAP / LV2 / AU)"), "insert chooser did not open"); } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        const auto headers = host.pickerRows (true);
        ctx.expect (! headers.empty() && headers.front() == "BUILT-IN", "built-in group was not first");
        filter ("picker");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (true) == std::vector<std::string> { "SCENARIO MAKER A", "SCENARIO MAKER B" },
                    "manufacturer headings did not group the matching plugins");
        ctx.expect (host.pickerRows (false) == std::vector<std::string> { "Picker Alpha  (CLAP)", "Picker Beta  (LV2-Native)" },
                    "filtered plugin rows omitted names or format labels");
        filter ("pIcKeR aLpHa");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (false) == std::vector<std::string> { "Picker Alpha  (CLAP)" },
                    "name filter was not case insensitive");
        filter ("no-such-picker-plugin");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (true).empty() && host.pickerRows (false).empty(),
                    "unmatched filter retained rows or empty groups");
        filter ("picker");
        ctx.expect (host.clickModalButton ("Group: Maker"), "grouping button was missing");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        ctx.expect (host.pickerRows (true) == std::vector<std::string> { "DELAY", "EQ" },
                    "type grouping did not use plugin categories");
        filter ("");
    } });
    steps->push_back ({ 150, [&host, &ctx, filter]
    {
        const auto headers = host.pickerRows (true);
        ctx.expect (! headers.empty() && headers.front() == "BUILT-IN", "type grouping displaced built-in units");
        filter ("Picker Alpha");
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (host.clickPickerRow ("Picker Alpha  (CLAP)"), "plugin row did not receive a click"); } });
    steps->push_back ({ 500, [&host, &ctx, &engine, &session, fixture]
    {
        ctx.expect (host.pickerRows (true).empty() && host.pickerRows (false).empty(),
                    "picking a plugin did not dismiss the picker");
        ctx.expect (engine.getChannelStrip (0).isNativeClapLoaded(), "picker did not load the native CLAP instance");
        ctx.expect (session.track (0).nativeClapPath.toStdString() == fixture->string()
                    && session.track (0).nativeClapPluginId.toStdString() == "studio.dusk.test.multi-bus",
                    "the picked plugin identity was not saved to the track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar pluginPicker { Scenario {
    "gui.plugin_picker_filter_and_load", { "gui", "plugins" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginPicker (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsDefaults (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    auto& session = ctx.session();
    const bool tape = appconfig::getTapeStripExpandedDefault();
    const bool follow = appconfig::getFollowPlayheadDefault();
    ctx.expect (host.tapeExpansionState() == (tape ? 3 : 0), "launch ignored the tape-strip default");
    ctx.expect (host.timelineChaseState() == (follow ? 3 : 0), "launch ignored the timeline Chase default");
    const auto config = dusk::fs::userConfigDir() / "Dusk Studio" / "app-config.properties";
    const bool hadConfig = std::filesystem::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, config, hadConfig, configText, originalDir, restore]
    {
        host.closeRegionEditors();
        host.closeAudioSettings();
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; std::filesystem::remove (config, error); }
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    MidiRegion midi;
    midi.lengthInSamples = 48000;
    midi.lengthInTicks = 960;
    session.track (0).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    const auto wave = ctx.tempDir() / "silence.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (wave, spec);
    std::vector<float> silence (48000, 0.0f);
    const float* channels[] = { silence.data() };
    if (! writer || ! writer->write (channels, 1, 48000) || ! writer->flush())
        return ScenarioResult::fail ("could not create the audio editor fixture");
    writer.reset();
    AudioRegion audio;
    using File = std::decay_t<decltype (audio.file)>;
    audio.file = File (wave.u8string().c_str());
    audio.lengthInSamples = 48000;
    audio.numChannels = 1;
    session.track (1).regions = { audio };
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool isMidi : { true, false })
    {
        steps->push_back ({ 200, [&host, &ctx, isMidi]
        { ctx.expect (host.openRegionEditor (isMidi ? 0 : 1, 0, isMidi), "region editor did not open"); } });
        steps->push_back ({ 200, [&host, &ctx, follow]
        {
            ctx.expect (host.regionEditorChase() == (follow ? 1 : 0), "editor ignored the Chase default");
            host.closeRegionEditors();
        } });
    }
    for (const bool inverted : { true, false })
    {
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
        steps->push_back ({ 200, [&host] { host.inputAudioSettings ("scroll-down"); } });
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickAudioSettingsControl ("tape-default"), "tape default checkbox was not visible"); } });
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickAudioSettingsControl ("follow-default"), "follow default checkbox was not visible"); } });
        steps->push_back ({ 200, [&host, &ctx, tape, follow, inverted]
        {
            ctx.expect (appconfig::getTapeStripExpandedDefault() == (inverted ? ! tape : tape),
                        "tape default did not persist through the checkbox");
            ctx.expect (appconfig::getFollowPlayheadDefault() == (inverted ? ! follow : follow),
                        "follow default did not persist through the checkbox");
            ctx.expect (host.tapeExpansionState() == (tape ? 3 : 0), "default changed the existing timeline layout");
            ctx.expect (host.timelineChaseState() == (follow ? 3 : 0), "default changed existing timeline Chase");
            host.closeAudioSettings();
        } });
    }
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (! host.audioSettingsOpen(), "audio settings did not finish closing"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsDefaults { Scenario {
    "gui.settings_defaults", { "gui", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsDefaults (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsUiScale (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    const auto originalScale = static_cast<float> (host.uiScale());
    const auto originalSaved = appconfig::getUiScaleOverride();
    const auto config = dusk::fs::userConfigDir() / "Dusk Studio" / "app-config.properties";
    const bool hadConfig = std::filesystem::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    ctx.cleanup ([&host, originalScale, config, hadConfig, configText]
    {
        host.closeAudioSettings();
        host.restoreUiScale (originalScale);
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; std::filesystem::remove (config, error); }
    });
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("scroll-down"); } });
    for (const float position : { 0.4f, 0.5f, 0.6f, 0.7f })
        steps->push_back ({ 200, [&host, &ctx, position]
        { ctx.expect (host.pointerAudioSettings ("ui-scale", position, true),
                      "the UI scale slider was not visible"); } });
    steps->push_back ({ 300, [&host, &ctx, originalSaved]
    {
        ctx.expect (host.uiScale() > 1.3 && host.uiScale() < 1.6,
                    "dragging did not preview the global UI scale");
        ctx.expect (nearly (appconfig::getUiScaleOverride(), originalSaved),
                    "the UI scale was persisted before release");
        ctx.expect (host.pointerAudioSettings ("ui-scale", 0.7f, false),
                    "the scale slider could not receive release");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        const auto saved = appconfig::getUiScaleOverride();
        ctx.expect (saved > 1.5f && saved < 1.6f, "release did not persist the slider value");
        ctx.expect (std::abs (host.uiScale() - saved) < 0.001,
                    "the persisted scale differs from the displayed interface");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (! host.audioSettingsOpen(), "settings did not close");
        ctx.expect (std::abs (host.uiScale() - appconfig::getUiScaleOverride()) < 0.001,
                    "closing Settings discarded the scale");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsUiScale { Scenario {
    "gui.settings_ui_scale", { "gui", "settings" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsUiScale (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsAutosave (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings");
   #else
    namespace fs = std::filesystem;
    auto& session = ctx.session();
    const auto config = dusk::fs::userConfigDir() / "Dusk Studio" / "app-config.properties";
    const bool hadConfig = fs::exists (config);
    const auto configText = dusk::fs::loadFileAsString (config);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, config, hadConfig, configText, originalDir, restore]
    {
        if (hadConfig) dusk::fs::writeStringToFile (config, configText);
        else { std::error_code error; fs::remove (config, error); }
        host.closeAudioSettings();
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    const auto dir = ctx.tempDir() / "cadence";
    fs::create_directories (dir);
    const auto saved = dir / "session.json";
    const auto autosave = dir / "session.json.autosave";
    session.track (0).strip.faderDb.store (0.0f);
    if (! SessionSerializer::save (session, saved) || ! host.openSession (saved))
        return ScenarioResult::fail ("could not open the autosave fixture");
    appconfig::setAutosaveIntervalSeconds (300);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not open"); } });
    steps->push_back ({ 200, [&host] { host.closeAudioSettings(); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "settings did not reopen"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.inputAudioSettings ("scroll-down"), "settings did not accept scrolling"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("autosave"), "autosave dropdown is not visible"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("home"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("enter"); } });
    steps->push_back ({ 200, [&ctx, &session]
    {
        ctx.expect (appconfig::getAutosaveIntervalSeconds() == 15,
                    "the first autosave option did not select 15 seconds");
        session.track (0).strip.faderDb.store (-12.0f);
    } });
    steps->push_back ({ 16000, [&host, &ctx, autosave]
    {
        ctx.expect (! fs::exists (autosave), "the new cadence applied before Settings closed");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 16000, [&host, &ctx, autosave, saved]
    {
        ctx.expect (fs::exists (autosave), "the real autosave timer did not adopt 15 seconds");
        ctx.expect (nearly (savedFaderOf (autosave), -12.0f), "autosave omitted the edit");
        ctx.expect (nearly (savedFaderOf (saved), 0.0f), "autosave changed the canonical session");
        ctx.expect (host.openAudioSettings(), "settings did not reopen for the maximum cadence");
    } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("scroll-down"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("autosave"), "autosave dropdown is not visible"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("end"); } });
    steps->push_back ({ 200, [&host] { host.inputAudioSettings ("enter"); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (appconfig::getAutosaveIntervalSeconds() == 300,
                    "the last autosave option did not select 5 minutes");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (! host.audioSettingsOpen(), "audio settings did not finish closing"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsAutosave { Scenario {
    "gui.settings_autosave_cadence", { "gui", "settings", "autosave" }, Needs::Engine | Needs::Gui,
    {}, {}, 45000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsAutosave (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsRescan (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI || ! DUSKSTUDIO_HAS_ALSA
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires native settings and ALSA sequencer");
   #else
    snd_seq_t* raw = nullptr;
    if (snd_seq_open (&raw, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        return ScenarioResult::skip ("ALSA sequencer is unavailable");
    auto seq = std::shared_ptr<snd_seq_t> (raw, [] (snd_seq_t* value) { snd_seq_close (value); });
    const auto name = "dusk-rescan-" + std::to_string (snd_seq_client_id (seq.get()));
    snd_seq_set_client_name (seq.get(), name.c_str());
    auto port = std::make_shared<int> (-1);
    auto& engine = ctx.engine();
    const auto originalPosition = engine.getTransport().getPlayhead();
    ctx.cleanup ([&host, &engine, seq, port, originalPosition]
    {
        host.closeAudioSettings();
        engine.stop();
        engine.getTransport().setPlayhead (originalPosition);
        if (*port >= 0) snd_seq_delete_simple_port (seq.get(), *port);
        engine.refreshMidiInputs();
    });
    const auto listed = [&engine, name]
    {
        const auto& outputs = engine.getMidiOutputDevices();
        return std::any_of (outputs.begin(), outputs.end(), [&name] (const auto& output)
        { return output.name.find (name) != std::string::npos; });
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx, &engine]
    {
        ctx.expect (host.openAudioSettings(), "native audio settings did not open");
        engine.play();
    } });
    steps->push_back ({ 200, [seq, port, &ctx]
    {
        *port = snd_seq_create_simple_port (seq.get(), "destination",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        ctx.expect (*port >= 0, "could not create the private MIDI destination");
    } });
    steps->push_back ({ 700, [&host, &ctx, &engine, listed]
    {
        ctx.expect (engine.getTransport().isPlaying(), "fixture was not playing");
        ctx.expect (! listed(), "automatic hot-plug refreshed the port before Rescan");
        ctx.expect (host.clickAudioSettingsControl ("rescan"), "Rescan devices button was not drawn");
    } });
    steps->push_back ({ 400, [&host, &ctx, &engine, listed]
    {
        ctx.expect (listed(), "Rescan devices did not enumerate the new MIDI destination");
        host.closeAudioSettings();
        engine.stop();
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (! host.audioSettingsOpen(), "audio settings did not finish closing"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsRescan { Scenario {
    "gui.settings_rescan_devices", { "gui", "settings", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsRescan (host, ctx); }
} };

std::optional<ScenarioResult> runSettingsMidiBindings (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without native settings UI");
   #else
    ctx.cleanup ([&host]
    {
        if (host.midiBindingsOpen()) host.closeTopModal();
        host.closeAudioSettings();
    });
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.openAudioSettings(), "native audio settings did not open"); } });
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickAudioSettingsControl ("midi-bindings"), "MIDI Bindings button was not drawn"); } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.midiBindingsOpen(), "MIDI Bindings button did not open its panel");
        ctx.expect (! host.audioSettingsOpen(), "settings did not step aside for MIDI Bindings");
        ctx.expect (host.clickModalButton ("Done"), "MIDI Bindings Done button was not visible");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (! host.midiBindingsOpen(), "Done did not close MIDI Bindings");
        ctx.expect (host.audioSettingsOpen(), "Done did not return to audio settings");
        host.closeAudioSettings();
    } });
    steps->push_back ({ 150, [&host, &ctx]
    { ctx.expect (! host.audioSettingsOpen(), "audio settings did not finish closing"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar settingsMidiBindings { Scenario {
    "gui.settings_midi_bindings", { "gui", "settings", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSettingsMidiBindings (host, ctx); }
} };

std::optional<ScenarioResult> runMidiSelectors (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_ALSA
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("virtual MIDI destination fixture requires ALSA sequencer");
   #else
    snd_seq_t* raw = nullptr;
    if (snd_seq_open (&raw, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
        return ScenarioResult::skip ("ALSA sequencer is unavailable");
    auto seq = std::shared_ptr<snd_seq_t> (raw, [] (snd_seq_t* value) { snd_seq_close (value); });
    const auto name = "dusk-selector-" + std::to_string (snd_seq_client_id (seq.get()));
    snd_seq_set_client_name (seq.get(), name.c_str());
    const int port = snd_seq_create_simple_port (seq.get(), "destination",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) return ScenarioResult::fail ("could not create the private MIDI destination");
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (0);
    const auto originalStage = engine.getStage();
    const auto originalDir = currentSessionDirectory (session);
    const auto restoreFile = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restoreFile))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, seq, port, originalDir, restoreFile, originalStage]
    {
        drainModals (host);
        snd_seq_delete_simple_port (seq.get(), port);
        engine.refreshMidiInputs();
        host.openSession (restoreFile);
        applySessionDirectory (session, originalDir);
        host.switchToStage (guiStage (originalStage));
    });
    ctx.keep (track.mode);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiOutputIndex);
    ctx.keep (track.midiChannel);
    ctx.keep (track.inputMonitor);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (-1);
    track.midiOutputIndex.store (-1);
    track.midiInputIdentifier.clear();
    track.midiOutputIdentifier.clear();
    track.inputMonitor.store (true);
    engine.refreshMidiInputs();
    const auto& outputs = engine.getMidiOutputDevices();
    int destination = -1;
    for (int i = 0; i < (int) outputs.size(); ++i)
        if (outputs[(size_t) i].name.find (name) != std::string::npos) destination = i;
    if (destination < 0) return ScenarioResult::fail ("private MIDI destination was not enumerated");
    const auto destinationId = outputs[(size_t) destination].identifier;
    const int keyboard = engine.getVirtualKeyboardInputIndex();
    if (keyboard < 0) return ScenarioResult::fail ("virtual keyboard was not enumerated");
    const auto keyboardId = engine.getMidiInputDevices()[(size_t) keyboard].identifier;
    host.switchToStage (GuiHost::Stage::Recording);
    const int maxRows = std::max ({ 17, (int) engine.getMidiInputDevices().size() + 1, (int) outputs.size() + 1 });
    const auto select = [&host, maxRows] (int row)
    {
        for (int i = 0; i < maxRows; ++i) host.pressPeerKey ("cursor up");
        for (int i = 0; i < row; ++i) host.pressPeerKey ("cursor down");
        host.pressPeerKey ("Return");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 150, [&host, &ctx] { ctx.expect (host.openMidiIo (0), "MIDI I/O popup did not open"); } });
    for (const auto choice : { std::pair<int, int> { 0, keyboard + 1 }, { 1, 3 }, { 2, destination + 1 } })
    {
        steps->push_back ({ 100, [&host, &ctx, choice]
        { ctx.expect (host.clickMidiSelector (0, choice.first), "MIDI selector was not visible"); } });
        steps->push_back ({ 100, [select, choice] { select (choice.second); } });
    }
    steps->push_back ({ 150, [&host, &track, &engine, &ctx, keyboard, keyboardId, destination, destinationId]
    {
        ctx.expect (track.midiInputIndex.load() == keyboard && track.midiInputIdentifier.toStdString() == keyboardId,
                    "input picker did not select and identify the virtual keyboard");
        ctx.expect (host.midiSelectorText (0, 0) == "Virtual Keyboard (Dusk Studio)", "input picker displayed the wrong port");
        ctx.expect (track.midiChannel.load() == 3 && host.midiSelectorText (0, 1) == "Ch 3", "channel picker did not select channel 3");
        ctx.expect (track.midiOutputIndex.load() == destination && track.midiOutputIdentifier.toStdString() == destinationId,
                    "output picker did not select and identify the destination");
        const std::uint8_t rejected[] = { 0xb1, 74, 100 };
        const std::uint8_t accepted[] = { 0xb2, 74, 101 };
        engine.postVirtualKeyboardMidi (rejected, 3);
        engine.postVirtualKeyboardMidi (accepted, 3);
    } });
    steps->push_back ({ 300, [seq, &ctx]
    {
        bool accepted = false;
        bool rejected = false;
        snd_seq_event_t* event = nullptr;
        while (snd_seq_event_input (seq.get(), &event) >= 0)
            if (event != nullptr && event->type == SND_SEQ_EVENT_CONTROLLER && event->data.control.param == 74)
            {
                accepted |= event->data.control.channel == 2 && event->data.control.value == 101;
                rejected |= event->data.control.channel == 1 && event->data.control.value == 100;
            }
        ctx.expect (accepted, "selected MIDI output did not receive the accepted channel");
        ctx.expect (! rejected, "channel filter passed the rejected channel to MIDI out");
    } });
    for (int kind : { 0, 1, 2 })
    {
        steps->push_back ({ 100, [&host, kind] { host.clickMidiSelector (0, kind); } });
        steps->push_back ({ 100, [select] { select (0); } });
    }
    steps->push_back ({ 100, [&host, &track, &ctx]
    {
        ctx.expect (track.midiInputIndex.load() == -1 && track.midiInputIdentifier.isEmpty()
                    && host.midiSelectorText (0, 0) == "None", "input None did not clear the route");
        ctx.expect (track.midiChannel.load() == 0 && host.midiSelectorText (0, 1) == "Omni", "Omni did not clear the channel filter");
        ctx.expect (track.midiOutputIndex.load() == -1 && track.midiOutputIdentifier.isEmpty()
                    && host.midiSelectorText (0, 2) == "None", "output None did not clear the route");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar midiSelectors { Scenario {
    "gui.midi_io_selectors", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMidiSelectors (host, ctx); }
} };

std::optional<ScenarioResult> runFaderEntry (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const auto bindings = session.midiBindings.current();
    const auto originalStage = engine.getStage();
    ctx.cleanup ([&host, &session, bindings, originalStage]
    {
        host.pressPeerKey ("Escape");
        drainModals (host);
        session.midiBindings.mutate ([&] (auto& value) { value = bindings; });
        host.switchToStage (guiStage (originalStage));
    });
    ctx.cleanup (host.preserveKeyboardFocus());
    ctx.keep (session.midiLearnPending);
    ctx.keep (session.midiLearnCapture);
    session.midiLearnPending.store (-1);
    session.midiLearnCapture.store (0);
    session.midiBindings.mutate ([] (auto& value) { value.clear(); });
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        ctx.keep (track.strip.faderGroupId);
        ctx.keep (track.strip.faderDb);
        ctx.keep (track.automationMode);
        track.strip.faderGroupId.store (t < 2 ? 1 : 0);
        track.automationMode.store ((int) AutomationMode::Off);
        track.strip.faderDb.store (-6.0f - (float) t * 3.0f);
    }
    host.switchToStage (GuiHost::Stage::Mixing);
    const auto type = [&host] (const std::string& value)
    {
       #if defined (__APPLE__)
        host.pressPeerKey ("command + A", 'a');
       #else
        host.pressPeerKey ("ctrl + A", 'a');
       #endif
        for (char ch : value) host.pressPeerKey (std::string (1, ch), ch);
        host.pressPeerKey ("Return", '\r');
    };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 250, [&host, &ctx] { ctx.expect (host.clickFader (0, true), "fader readout was not visible"); } });
    steps->push_back ({ 100, [&host, &ctx, type]
    {
        ctx.expect (host.faderEditing (0), "clicking the fader readout did not open an editor");
        if (host.faderEditing (0)) type ("-3.5");
    } });
    steps->push_back ({ 150, [&host, &ctx, &session]
    {
        ctx.expect (std::abs (session.track (0).strip.faderDb.load() + 3.5f) < 0.01f, "typed fader value was not applied");
        ctx.expect (std::abs (session.track (1).strip.faderDb.load() + 6.5f) < 0.01f, "typed fader value lost the group offset");
        ctx.expect (! session.track (0).strip.faderTouched.load(), "text entry left the fader touched");
        session.track (0).automationMode.store ((int) AutomationMode::Read);
    } });
    steps->push_back ({ 100, [&host, &ctx] { host.clickFader (0, true); ctx.expect (! host.faderEditing (0), "READ allowed fader text editing"); } });
    steps->push_back ({ 100, [&session]
    {
        session.track (0).automationMode.store ((int) AutomationMode::Off);
        session.track (0).strip.faderGroupId.store (0);
        session.track (1).strip.faderGroupId.store (0);
    } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickFader (0, false, true), "could not right-click the fader"); } });
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickModalAt (0.5f, 48.0f / 102.0f), "MIDI Learn menu did not open"); } });
    steps->push_back ({ 100, [&session, &engine, &ctx]
    {
        ctx.expect (session.midiLearnPending.load() == packLearnTarget (MidiBindingTarget::TrackFader, 0), "menu did not arm fader Learn");
        const std::uint8_t cc[] = { 0xb2, 74, 64 };
        engine.postVirtualKeyboardMidi (cc, 3);
    } });
    steps->push_back ({ 300, [&session, &engine, &ctx]
    {
        const auto& learned = session.midiBindings.current();
        ctx.expect (learned.size() == 1 && learned[0].channel == 3 && learned[0].dataNumber == 74
                    && learned[0].trigger == MidiBindingTrigger::CC && learned[0].target == MidiBindingTarget::TrackFader
                    && learned[0].targetIndex == 0 && session.midiLearnPending.load() == -1, "CC did not learn the selected fader");
        const std::uint8_t cc[] = { 0xb2, 74, 127 };
        engine.postVirtualKeyboardMidi (cc, 3);
    } });
    steps->push_back ({ 300, [&host, &session, &engine, &ctx]
    {
        ctx.expect (std::abs (session.track (0).strip.faderDb.load() - 12.0f) < 0.01f
                    && std::abs (host.faderValue (0) - 12.0) < 0.01, "learned CC did not raise the model and visible fader");
        const std::uint8_t cc[] = { 0xb2, 74, 0 };
        engine.postVirtualKeyboardMidi (cc, 3);
    } });
    steps->push_back ({ 300, [&host, &session, &ctx]
    {
        ctx.expect (std::abs (session.track (0).strip.faderDb.load() + 90.0f) < 0.01f
                    && std::abs (host.faderValue (0) + 90.0) < 0.01, "learned CC did not lower the model and visible fader");
        ctx.expect (std::abs (session.track (2).strip.faderDb.load() + 12.0f) < 0.01f, "fader Learn changed an unbound track");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar faderEntry { Scenario {
    "gui.fader_entry_and_learn", { "gui", "fader", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runFaderEntry (host, ctx); }
} };

std::optional<ScenarioResult> runGroupChips (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    const auto originalStage = ctx.engine().getStage();
    ctx.cleanup ([&host, originalStage]
    {
        host.switchToStage (guiStage (originalStage));
    });
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        ctx.keep (session.track (t).strip.faderGroupId);
        session.track (t).strip.faderGroupId.store (0);
    }
    host.switchToStage (GuiHost::Stage::Mixing);
    const auto check = [&host, &ctx] (int track, const std::string& expectedText, int expectedMaster, bool expectedFilled)
    {
        std::string text;
        int master = -1;
        bool filled = false;
        ctx.expect (host.groupChipView (track, text, master, filled), "the group chip did not render its fill");
        ctx.expect (text == expectedText, "the group chip label was wrong");
        ctx.expect (master == expectedMaster, "the lowest group member was not the master");
        ctx.expect (filled == expectedFilled, "the group chip used the wrong filled/outlined style");
    };
    auto steps = std::make_shared<std::vector<Step>>();
    for (int group = 1; group <= 8; ++group)
    {
        steps->push_back ({ 100, [&session, group]
        { session.track (1).strip.faderGroupId.store (group); } });
        steps->push_back ({ 100, [&session, check, group]
        {
            check (1, "G" + std::to_string (group), 1, true);
            session.track (0).strip.faderGroupId.store (group);
        } });
        steps->push_back ({ 100, [&session, check, group]
        {
            check (0, "G" + std::to_string (group), 0, true);
            check (1, "G" + std::to_string (group), 0, false);
            session.track (0).strip.faderGroupId.store (0);
        } });
        steps->push_back ({ 100, [check, group]
        {
            check (0, "", -1, false);
            check (1, "G" + std::to_string (group), 1, true);
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar groupChips { Scenario {
    "gui.fader_group_chips", { "gui", "groups" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runGroupChips (host, ctx); }
} };

std::optional<ScenarioResult> runMeterClip (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto originalStage = engine.getStage();
    const auto originalPosition = engine.getTransport().getPlayhead();
    const auto originalRegions = track.regions;
    ctx.cleanup ([&host, &engine, &track, originalStage, originalPosition, originalRegions]
    {
        engine.stop();
        track.regions = originalRegions;
        engine.getPlaybackEngine().preparePlayback();
        engine.getTransport().setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });
    ctx.keep (track.mode);
    ctx.keep (track.strip.faderDb);
    ctx.keep (track.strip.mute);
    ctx.keep (track.recordArmed);
    ctx.keep (track.inputMonitor);
    ctx.keep (track.automationMode);
    track.recordArmed.store (false);
    track.inputMonitor.store (false);
    track.automationMode.store ((int) AutomationMode::Off);
    ctx.keep (session.master().mute);
    session.master().mute.store (true);
    engine.stop();
    const auto rate = engine.getCurrentSampleRate();
    const auto frames = static_cast<std::int64_t> (rate * 5.0);
    std::vector<float> samples ((size_t) frames);
    for (std::int64_t i = 0; i < frames; ++i)
        samples[(size_t) i] = 0.8f * static_cast<float> (std::sin (6.283185307179586 * 440.0 * static_cast<double> (i) / rate));
    const auto path = ctx.tempDir() / "stage-tone.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = rate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    if (writer == nullptr || ! writer->write (channels, 1, frames) || ! writer->flush())
        return ScenarioResult::fail ("could not write the playback tone");
    writer.reset();
    AudioRegion region;
    using File = std::decay_t<decltype (region.file)>;
    region.file = File (path.u8string().c_str());
    region.lengthInSamples = frames;
    region.numChannels = 1;
    track.regions = { region };
    track.mode.store ((int) Track::Mode::Mono);
    track.strip.faderDb.store (12.0f);
    track.strip.mute.store (false);
    host.switchToStage (GuiHost::Stage::Recording);
    engine.getTransport().setPlayhead (0);
    engine.play();
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&host, &engine, &ctx]
    {
        ctx.expect (engine.getChannelStrip (0).getOutLDb() > 0.0f, "fixture did not cross 0 dBFS");
        ctx.expect (host.meterClip (0), "overload did not light the red clip bar");
        engine.stop();
    } });
    steps->push_back ({ 700, [&host, &ctx]
    { ctx.expect (host.meterClip (0), "clip bar cleared before one second"); } });
    steps->push_back ({ 600, [&host, &ctx]
    { ctx.expect (! host.meterClip (0), "clip bar did not clear after one second"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar meterClip { Scenario {
    "gui.meter_clip_hold", { "gui", "meter" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMeterClip (host, ctx); }
} };

std::optional<ScenarioResult> runStageAudioFlow (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (0);
    const auto originalStage = engine.getStage();
    const auto originalPosition = engine.getTransport().getPlayhead();
    const auto originalRegions = track.regions;
    ctx.cleanup ([&host, &engine, &track, originalStage, originalPosition, originalRegions]
    {
        engine.stop();
        track.regions = originalRegions;
        engine.getPlaybackEngine().preparePlayback();
        engine.getTransport().setPlayhead (originalPosition);
        host.switchToStage (guiStage (originalStage));
    });
    ctx.keep (track.mode);
    ctx.keep (track.strip.faderDb);
    ctx.keep (track.strip.mute);
    engine.stop();
    const auto rate = engine.getCurrentSampleRate();
    const auto frames = static_cast<std::int64_t> (rate * 5.0);
    std::vector<float> samples ((size_t) frames);
    for (std::int64_t i = 0; i < frames; ++i)
        samples[(size_t) i] = 0.125f * static_cast<float> (std::sin (6.283185307179586 * 440.0 * static_cast<double> (i) / rate));
    const auto path = ctx.tempDir() / "stage-tone.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = rate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    if (writer == nullptr || ! writer->write (channels, 1, frames) || ! writer->flush())
        return ScenarioResult::fail ("could not write the playback tone");
    writer.reset();
    AudioRegion region;
    using File = std::decay_t<decltype (region.file)>;
    region.file = File (path.u8string().c_str());
    region.lengthInSamples = frames;
    region.numChannels = 1;
    track.regions = { region };
    track.mode.store ((int) Track::Mode::Mono);
    track.strip.faderDb.store (0.0f);
    track.strip.mute.store (false);
    host.switchToStage (GuiHost::Stage::Recording);
    engine.getTransport().setPlayhead (0);
    engine.play();
    auto previousPosition = std::make_shared<std::int64_t> (0);
    auto overruns = std::make_shared<int> (0);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 300, [&engine, overruns] { *overruns = engine.getXRunCount(); } });
    for (auto stage : { GuiHost::Stage::Mixing, GuiHost::Stage::Aux, GuiHost::Stage::Recording,
                        GuiHost::Stage::Aux, GuiHost::Stage::Mixing, GuiHost::Stage::Recording })
    {
        steps->push_back ({ 100, [&host, &ctx, stage]
        { ctx.expect (host.clickStage (stage), "the stage button was not visible"); } });
        steps->push_back ({ 150, [&ctx, &engine, previousPosition, overruns, stage]
        {
            const auto expected = stage == GuiHost::Stage::Recording ? AudioEngine::Stage::Recording
                                : stage == GuiHost::Stage::Mixing ? AudioEngine::Stage::Mixing
                                : AudioEngine::Stage::Aux;
            ctx.expect (engine.getStage() == expected, "the stage button did not switch stages");
            ctx.expect (engine.getTransport().isPlaying(), "switching stages stopped playback");
            const auto at = engine.getTransport().getPlayhead();
            ctx.expect (at > *previousPosition, "playback did not advance across a stage switch");
            *previousPosition = at;
            ctx.expect (engine.getChannelStrip (0).getOutLDb() > -30.0f,
                        "the playback tone fell silent across a stage switch");
            ctx.expect (engine.getXRunCount() == *overruns, "a stage switch caused an engine overrun");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar stageAudioFlow { Scenario {
    "gui.stage_audio_flow", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStageAudioFlow (host, ctx); }
} };

std::optional<ScenarioResult> runSessionSwitch (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const auto sampleRate = engine.getCurrentSampleRate();
    if (sampleRate <= 0.0)
        return ScenarioResult::skip ("requires a positive engine sample rate");
    const auto originalDir = currentSessionDirectory (session);
    const auto restoreFile = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restoreFile))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, originalDir, restoreFile]
    {
        engine.stop();
        drainModals (host);
        host.openSession (restoreFile);
        applySessionDirectory (session, originalDir);
        session.setTrackArmed (0, false);
    });
    const auto outgoing = ctx.tempDir() / "outgoing" / "session.json";
    const auto incoming = ctx.tempDir() / "incoming" / "session.json";
    std::filesystem::create_directories (outgoing.parent_path());
    std::filesystem::create_directories (incoming.parent_path());
    session.track (0).mode.store ((int) Track::Mode::Midi);
    session.track (0).midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    session.track (0).strip.faderDb.store (0.0f);
    if (! SessionSerializer::save (session, outgoing))
        return ScenarioResult::fail ("could not save the outgoing session");
    session.track (0).strip.faderDb.store (-18.0f);
    if (! SessionSerializer::save (session, incoming))
        return ScenarioResult::fail ("could not save the incoming session");
    auto steps = std::make_shared<std::vector<Step>>();
    for (const std::string action : { "Cancel", "Don't Save", "Save" })
    {
        steps->push_back ({ 200, [&host, &ctx, &session, &engine, outgoing]
        {
            ctx.expect (host.openSession (outgoing), "could not open the outgoing session");
            session.setTrackArmed (0, true);
            engine.record();
            ctx.expect (engine.getTransport().isRecording(), "the take did not start");
        } });
        steps->push_back ({ 200, [&engine]
        {
            const std::uint8_t note[] { 0x90, 64, 100 };
            engine.postVirtualKeyboardMidi (note, 3);
        } });
        steps->push_back ({ 150, [&engine]
        {
            const std::uint8_t note[] { 0x80, 64, 0 };
            engine.postVirtualKeyboardMidi (note, 3);
        } });
        steps->push_back ({ 200, [&host, incoming] { host.requestSessionSwitch (incoming); } });
        steps->push_back ({ 200, [&host, &ctx, &engine, &session, outgoing, incoming]
        {
            ctx.expect (engine.getTransport().isStopped(), "the session switch did not stop recording");
            ctx.expect (currentSessionDirectory (session) == outgoing.parent_path(),
                        "the session changed before the prompt was answered");
            const auto& regions = session.track (0).midiRegions.current();
            ctx.expect (regions.size() == 1 && ! regions.front().notes.empty(),
                        "the session switch did not commit the MIDI take before prompting");
            host.requestSessionSwitch (incoming);
        } });
        steps->push_back ({ 100, [&host, &ctx, action]
        {
            ctx.expect (host.statusMessage() == "Session not switched: close the open prompt first",
                        "a second session switch did not report the open prompt");
            ctx.expect (host.clickModalButton (action), "the prompt did not offer " + action);
        } });
        steps->push_back ({ 500, [&host, &ctx, &session, action, outgoing, incoming]
        {
            ctx.expect (host.modalStackEmpty(), action + " left the prompt open");
            ctx.expect (currentSessionDirectory (session)
                            == (action == "Cancel" ? outgoing.parent_path() : incoming.parent_path()),
                        action + " opened the wrong session");
            Session saved;
            ctx.expect (SessionSerializer::load (saved, outgoing), "could not read the outgoing session");
            const auto& regions = saved.track (0).midiRegions.current();
            ctx.expect (action == "Save" ? regions.size() == 1 && ! regions.front().notes.empty()
                                         : regions.empty(),
                        action + " persisted the wrong take state");
            if (action == "Cancel")
                ctx.expect (session.track (0).midiRegions.current().size() == 1,
                            "Cancel discarded the committed take");
            else
                ctx.expect (nearly (session.track (0).strip.faderDb.load(), -18.0f),
                            "the incoming session contents did not load");
        } });
    }
    steps->push_back ({ 200, [&host, &ctx, &session, outgoing, sampleRate]
    {
        MidiRegion region;
        region.lengthInSamples = static_cast<std::int64_t> (sampleRate * 600.0);
        region.lengthInTicks = 576000;
        session.track (0).midiRegions.publish (
            std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
        host.startMixdown();
        ctx.expect (host.mixdownRunning(), "the mixdown did not start");
        host.requestSessionSwitch (outgoing);
    } });
    steps->push_back ({ 100, [&host, &ctx, &session, incoming]
    {
        ctx.expect (host.statusMessage() == "Session not switched: finish or cancel the bounce first",
                    "a session switch did not report the running bounce");
        ctx.expect (currentSessionDirectory (session) == incoming.parent_path(),
                    "the session switched during the bounce");
        ctx.expect (host.mixdownRunning(), "the refused session switch stopped the bounce");
        ctx.expect (host.clickModalButton ("Cancel"), "the mixdown did not offer Cancel");
    } });
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.mixdownRunning(); }, 5000, [&host, &ctx]
        {
            ctx.later (200, [&host, &ctx]
            {
                if (! host.modalStackEmpty())
                    ctx.expect (host.clickModalButton ("Close"), "the cancelled mixdown did not offer Close");
                ctx.later (200, [&host, &ctx]
                {
                    ctx.expect (host.modalStackEmpty(), "the cancelled mixdown left its modal open");
                    ctx.complete (ctx.verdict());
                });
            });
        }, "the mixdown did not cancel");
    });
    return std::nullopt;
}

const ScenarioRegistrar sessionSwitch { Scenario {
    "gui.session_switch_commits_take", { "gui", "session" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSessionSwitch (host, ctx); }
} };

// The heartbeat writes session.json.autosave only when the session changed
// since the last save or autosave, never touches session.json and leaves no
// temp file behind. Opening a session whose autosave says something else
// offers recovery, and each answer does what the prompt says.
std::optional<ScenarioResult> runAutosave (GuiHost& host, ScenarioContext& ctx)
{
    namespace fs = std::filesystem;
    static constexpr float kEdited = -12.0f;
    static constexpr float kLater  = -6.0f;

    auto& session = ctx.session();
    auto& fader = session.track (0).strip.faderDb;
    const float original = fader.load (std::memory_order_relaxed);
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &fader, original, originalDir, restore]
    {
        drainModals (host);
        host.openSession (restore);
        fader.store (original, std::memory_order_relaxed);
        applySessionDirectory (session, originalDir);
    });

    const auto dir = ctx.tempDir() / "autosaved";
    std::error_code error;
    fs::create_directories (dir, error);
    const auto sessionJson = dir / "session.json";
    const auto autosave = dir / "session.json.autosave";
    if (! SessionSerializer::save (session, sessionJson))
        return ScenarioResult::fail ("could not write the session to open");
    if (! host.openSession (sessionJson) || ! host.modalStackEmpty())
        return ScenarioResult::fail ("the session did not open without a prompt");

    host.autosaveTick();
    ctx.expect (! fs::exists (autosave, error), "the heartbeat wrote an autosave for an unchanged session");

    fader.store (kEdited, std::memory_order_relaxed);
    host.autosaveTick();
    ctx.expect (fs::exists (autosave, error), "the heartbeat did not write the change");
    ctx.expect (! fs::exists (dir / "session.json.autosave.tmp", error), "the autosave left its temp file behind");
    ctx.expect (nearly (savedFaderOf (autosave), kEdited), "the autosave does not hold the change");
    ctx.expect (nearly (savedFaderOf (sessionJson), original), "the heartbeat touched session.json");

    fs::remove (autosave, error);
    host.autosaveTick();
    ctx.expect (! fs::exists (autosave, error), "the heartbeat rewrote a state it had already written");

    // Recover: the autosave's state loads and becomes session.json.
    SessionSerializer::save (session, autosave);
    if (! ctx.expect (host.openSession (sessionJson) && ! host.modalStackEmpty(),
                      "a session with a newer autosave opened without offering recovery"))
        return ctx.verdict();
    ctx.expect (host.answerRecovery (GuiHost::Recovery::Recover), "the recovery prompt was not the modal up");
    ctx.expect (nearly (fader.load (std::memory_order_relaxed), kEdited), "Recover did not load the autosave");
    ctx.expect (nearly (savedFaderOf (sessionJson), kEdited), "Recover did not write the recovered state to session.json");
    ctx.expect (! fs::exists (autosave, error), "Recover left the autosave behind");

    // Load saved session: session.json wins and the autosave is discarded.
    fader.store (kLater, std::memory_order_relaxed);
    SessionSerializer::save (session, autosave);
    if (ctx.expect (host.openSession (sessionJson) && ! host.modalStackEmpty(),
                    "the second newer autosave offered no recovery"))
    {
        host.answerRecovery (GuiHost::Recovery::LoadSaved);
        ctx.expect (nearly (fader.load (std::memory_order_relaxed), kEdited), "Load saved session did not load session.json");
        ctx.expect (! fs::exists (autosave, error), "Load saved session kept the autosave");
    }

    // Cancel: nothing loads and the recovery point stays.
    fader.store (kLater, std::memory_order_relaxed);
    SessionSerializer::save (session, autosave);
    fader.store (kEdited, std::memory_order_relaxed);
    if (ctx.expect (host.openSession (sessionJson) && ! host.modalStackEmpty(),
                    "the third newer autosave offered no recovery"))
    {
        host.answerRecovery (GuiHost::Recovery::Cancel);
        ctx.expect (host.modalStackEmpty(), "Cancel left the prompt up");
        ctx.expect (fs::exists (autosave, error), "Cancel discarded the autosave");
    }
    return ctx.verdict();
}

std::optional<ScenarioResult> runStageViews (GuiHost& host, ScenarioContext& ctx)
{
    using Stage = GuiHost::Stage;
    auto original = Stage::Recording;
    switch (ctx.engine().getStage())
    {
        case AudioEngine::Stage::Recording: break;
        case AudioEngine::Stage::Mixing: original = Stage::Mixing; break;
        case AudioEngine::Stage::Aux: original = Stage::Aux; break;
        case AudioEngine::Stage::Mastering: original = Stage::Mastering; break;
    }
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&host, &transport, original, state = transport.getState(),
                  position = transport.getPlayhead()]
    {
        host.switchToStage (original);
        transport.setPlayhead (position);
        transport.setState (state);
    });
    transport.setState (Transport::State::Stopped);

    auto run = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakRun = run;
    *run = [&host, &ctx, weakRun] (std::size_t index)
    {
        constexpr std::array<Stage, 6> stages { Stage::Mixing, Stage::Recording, Stage::Aux,
                                               Stage::Mastering, Stage::Mixing, Stage::Recording };
        if (index == stages.size()) { ctx.complete (ctx.verdict()); return; }
        const auto stage = stages[index];
        if (! ctx.expect (host.clickStage (stage), "the stage button is not visible"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.waitUntil ([&host, stage] { return host.stageViewMatches (stage); }, 3000,
            [&host, &ctx, stage, index, next = weakRun.lock()]
            {
                if (stage == Stage::Recording || stage == Stage::Mixing)
                    for (int track = 0; track < Session::kNumTracks; ++track)
                        ctx.expect (host.stripStageControlsMatch (track, stage == Stage::Mixing),
                                    "tracking controls or aux sends are wrong on strip "
                                    + std::to_string (track + 1));
                (*next) (index + 1);
            }, "the stage click did not select its button and show only its view");
    };
    (*run) (0);
    return std::nullopt;
}

// ------------------------------------------------------------- registration

const ScenarioRegistrar stageViews { Scenario {
    "gui.stage_views_and_controls", { "gui", "stage" }, Needs::Engine | Needs::Gui,
    {}, {}, 25000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runStageViews (host, ctx); }
} };

std::optional<ScenarioResult> runClockFormats (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.keep (session.timeDisplayMode);
    ctx.keep (session.tempoBpm);
    ctx.keep (session.beatsPerBar);
    ctx.keep (session.midiLearnPending);
    ctx.cleanup ([&session, points = session.tempoMap.points()]
                 { session.tempoMap.setPoints (points); });
    ctx.cleanup ([&transport, position = transport.getPlayhead(), state = transport.getState()]
    {
        transport.setPlayhead (position);
        transport.setState (state);
    });
    transport.setState (Transport::State::Stopped);
    session.tempoMap.clear();
    session.tempoBpm.store (120.0f);
    session.beatsPerBar.store (4);
    session.midiLearnPending.store (-1);
    session.timeDisplayMode.store ((int) TimeDisplayMode::Bars);
    if (ctx.engine().getCurrentSampleRate() <= 0.0)
        ctx.engine().prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    transport.setPlayhead ((std::int64_t) std::llround (ctx.engine().getCurrentSampleRate() * 4.75));

    ctx.waitUntil ([&host] { return host.clockText() == "3.2.240"; }, 3000,
        [&host, &ctx, &session]
        {
            if (! ctx.expect (host.clickTimeFormat(), "the time-format button is not visible"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.waitUntil ([&host] { return host.clockText() == "00:04.750"; }, 3000,
                [&host, &ctx, &session]
                {
                    ctx.expect (session.timeDisplayMode.load() == (int) TimeDisplayMode::Time,
                                "the clock changed without publishing Time mode");
                    if (! ctx.expect (host.clickTimeFormat(), "the time-format button disappeared"))
                    { ctx.complete (ctx.verdict()); return; }
                    ctx.waitUntil ([&host] { return host.clockText() == "3.2.240"; }, 3000,
                        [&ctx, &session]
                        {
                            ctx.expect (session.timeDisplayMode.load() == (int) TimeDisplayMode::Bars,
                                        "the second click did not restore Bars mode");
                            ctx.complete (ctx.verdict());
                        }, "the clock did not return to Bars.Beats.Ticks");
                }, "the clock did not display minutes, seconds and milliseconds");
        }, "the clock did not display the initial Bars.Beats.Ticks position");
    return std::nullopt;
}

const ScenarioRegistrar clockFormats { Scenario {
    "gui.clock_format_roundtrip", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runClockFormats (host, ctx); }
} };

const ScenarioRegistrar automation { Scenario {
    "gui.automation_write_and_touch",
    { "gui", "automation" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAutomation (host, ctx); }
} };

const ScenarioRegistrar modeShown { Scenario {
    "gui.automation_mode_shown_on_every_strip",
    { "gui", "automation" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runModeShownOnEveryStrip (host, ctx); }
} };

const ScenarioRegistrar autosave { Scenario {
    "gui.autosave_writes_and_recovers",
    { "gui", "session", "autosave" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAutosave (host, ctx); }
} };

const ScenarioRegistrar editorLoop { Scenario {
    "gui.editor_open_close_loop",
    { "gui", "plugin", "editor" },
    Needs::Engine | Needs::Gui,
    {},
    {},
    180000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEditorOpenCloseLoop (host, ctx); }
} };

const ScenarioRegistrar auxAttach { Scenario {
    "gui.aux_attach_failure_no_modal",
    { "gui", "plugin", "editor" },
    Needs::Engine | Needs::Gui,
    { "multi_bus.clap" },
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxAttachFailure (host, ctx); }
} };

const ScenarioRegistrar clapNoWindow { Scenario {
    "gui.clap_no_window_message",
    { "gui", "plugin", "editor" },
    Needs::Engine | Needs::Gui,
    { "no_window.clap" },
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runClapNoWindowMessage (host, ctx); }
} };

const ScenarioRegistrar lv2Editor { Scenario {
    "gui.lv2_editor_reflects_state",
    { "gui", "plugin", "editor", "lv2" },
    Needs::Engine | Needs::Gui,
    { "file_state.lv2" },
    {},
    60000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runLv2EditorReflectsState (host, ctx); }
} };

const ScenarioRegistrar oopEditorChild { Scenario {
    "gui.oop_editor_closes_before_child",
    { "gui", "oop", "editor" },
    Needs::Engine | Needs::Gui | Needs::Oop,
    { "relayout.vst3" },
    {},
    120000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_OOP_PLUGINS
        (void) host; (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #elif defined (_WIN32)
        (void) host; (void) ctx;
        return ScenarioResult::skip ("Windows tracks the child by handle, not by pid");
       #else
        return runOopEditorClosesBeforeChild (host, ctx);
       #endif
    }
} };

const ScenarioRegistrar oopEditorFailure { Scenario {
    "gui.oop_editor_failure_no_strand",
    { "gui", "oop", "editor" },
    Needs::Engine | Needs::Gui | Needs::Oop,
    {},
    {},
    120000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_OOP_PLUGINS
        (void) host; (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #elif defined (_WIN32)
        (void) host; (void) ctx;
        return ScenarioResult::skip ("the sandbox stub children are driven on POSIX only");
       #else
        return runOopEditorFailureNoStrand (host, ctx);
       #endif
    }
} };
std::optional<ScenarioResult> runTrackShortcuts (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    const auto stage = ctx.engine().getStage();
    ctx.cleanup ([&host, &transport, stage, state = transport.getState(),
                  position = transport.getPlayhead()]
    {
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
        transport.setPlayhead (position);
        transport.setState (state);
    });
    transport.setState (Transport::State::Stopped);
    ctx.keep (session.activeBank);
    ctx.keep (session.mcu.bank);
    ctx.cleanup (host.preserveKeyboardFocus());
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        auto& track = session.track (index);
        ctx.keep (track.mode);
        ctx.keep (track.strip.mute);
        ctx.cleanup ([&session, index, armed = track.recordArmed.load(), solo = track.strip.solo.load()]
        {
            session.setTrackArmed (index, armed);
            session.setTrackSoloed (index, solo);
        });
        track.mode.store ((int) Track::Mode::Midi);
        session.setTrackArmed (index, false);
        session.setTrackSoloed (index, false);
        track.strip.mute.store (false);
    }
    auto steps = std::make_shared<std::vector<Step>>();
    for (const auto targetStage : { GuiHost::Stage::Recording, GuiHost::Stage::Mixing })
    {
        steps->push_back ({ 0, [&host, targetStage] { host.switchToStage (targetStage); } });
        steps->push_back ({ 50, [&host, &ctx, &session]
        {
            for (int i = 0; i < Session::kNumTracks; ++i)
                ctx.expect (host.pressKey ("cursor left"), "left arrow did not focus a strip");
            for (int target = 0; target < Session::kNumTracks; ++target)
            {
                for (const auto key : { "A", "S", "X" })
                    for (const bool enabled : { true, false })
                    {
                        ctx.expect (host.pressKey (key), std::string (key) + " was not handled");
                        for (int index = 0; index < Session::kNumTracks; ++index)
                        {
                            const auto& track = session.track (index);
                            const bool selected = enabled && index == target;
                            ctx.expect (track.recordArmed.load() == (selected && key[0] == 'A')
                                        && track.strip.solo.load() == (selected && key[0] == 'S')
                                        && track.strip.mute.load() == (selected && key[0] == 'X'),
                                        std::string (key) + " changed the wrong state on track "
                                        + std::to_string (index + 1));
                        }
                        ctx.expect (session.anyTrackArmed() == (enabled && key[0] == 'A'),
                                    "arm shortcut left the armed-track counter stale");
                    }
                ctx.expect (host.pressKey ("cursor right"), "right arrow did not move strip focus");
            }
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar trackShortcuts { Scenario {
    "gui.keyboard_track_shortcuts", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTrackShortcuts (host, ctx); }
} };
std::optional<ScenarioResult> runTapTempo (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    ctx.keep (session.tempoBpm);
    ctx.cleanup ([&session, points = session.tempoMap.points()]
                 { session.tempoMap.setPoints (points); });
    session.tempoMap.setPoints ({});
    auto stamps = std::make_shared<std::vector<double>>();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int delay : { 2200, 250, 500, 750, 1000, 300 })
        steps->push_back ({ delay, [&host, &ctx, &session, stamps]
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            stamps->push_back (std::chrono::duration<double, std::milli> (now).count());
            ctx.expect (host.pressKey ("B"), "tap tempo shortcut was not handled");
            if (stamps->size() < 2) return;
            const auto intervals = std::min<std::size_t> (4, stamps->size() - 1);
            const double duration = stamps->back() - (*stamps)[stamps->size() - 1 - intervals];
            const double expected = std::clamp (60000.0 * static_cast<double> (intervals) / duration,
                                               30.0, 300.0);
            ctx.expect (std::abs (session.tempoBpm.load() - expected) < 2.0,
                        "TAP did not average the most recent four intervals");
        } });
    steps->push_back ({ 2200, [&host, &ctx, &session]
    {
        const float before = session.tempoBpm.load();
        ctx.expect (host.pressKey ("B"), "first tap after timeout was not handled");
        ctx.expect (std::abs (session.tempoBpm.load() - before) < 0.001f,
                    "a first tap after timeout changed the tempo");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tapTempo { Scenario {
    "gui.tap_tempo_intervals", { "gui", "keyboard", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTapTempo (host, ctx); }
} };
std::optional<ScenarioResult> runArmInputRefusal (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (0);
    ctx.keep (track.frozen);
    ctx.cleanup ([&session, &track, channels = session.deviceCaptureChannels.load(),
                  mode = track.mode.load(), input = track.inputSource.load(), armed = track.recordArmed.load()]
    {
        session.deviceCaptureChannels.store (channels);
        track.mode.store (mode);
        track.inputSource.store (input);
        session.setTrackArmed (0, armed);
    });
    const auto stage = ctx.engine().getStage();
    ctx.cleanup ([&host, stage]
    {
        drainModals (host);
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    host.switchToStage (GuiHost::Stage::Recording);
    session.deviceCaptureChannels.store (1);
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    session.setTrackArmed (0, false);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the recording strip is missing")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int input : { 7, -1 })
    {
        steps->push_back ({ 100, [&ctx, &track, strip, input]
        {
            track.inputSource.store (input);
            ctx.expect (strip->clickArm(), "ARM is not visible");
        } });
        steps->push_back ({ 100, [&host, &ctx, &track, strip, input]
        {
            ctx.expect (! track.recordArmed.load() && ! strip->armLit(),
                        "a refused input left ARM enabled");
            const auto text = host.modalText();
            ctx.expect (text.find ("No input for " + track.name.toStdString()) != std::string::npos,
                        "the refusal did not name the track");
            const auto reason = input == -1 ? "has no input selected" : "records from In 8, and the audio device has 1 input(s)";
            ctx.expect (text.find (reason) != std::string::npos, "the refusal did not explain the missing input");
            ctx.expect (host.clickModalButton ("OK"), "the refusal has no usable OK button");
        } });
        steps->push_back ({ 100, [&host, &ctx, strip]
        {
            ctx.expect (strip->inputSettingsOpen(), "acknowledging the refusal did not open input settings");
            host.closeTopModal();
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar armInputRefusal { Scenario {
    "gui.arm_input_refusal", { "gui", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runArmInputRefusal (host, ctx); }
} };
std::optional<ScenarioResult> runAboutDetails (GuiHost& host, ScenarioContext& ctx)
{
    ctx.cleanup ([&host] { if (! host.modalStackEmpty()) host.closeTopModal(); });
    host.openAbout();
    ctx.waitUntil ([&host] { return ! host.modalStackEmpty(); }, 3000, [&host, &ctx]
    {
        const auto text = host.modalText();
        ctx.expect (text.find ("About Dusk Studio\nDusk Studio " JUCE_APPLICATION_VERSION_STRING) == 0,
                    "About does not show the application's version");
        ctx.expect (text.find ("Portastudio-style DAW.") != std::string::npos,
                    "About is missing the application description");
        ctx.expect (std::regex_search (text, std::regex (
                        "Built [A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}")),
                    "About does not show its build date and time");
        if (! ctx.expect (host.clickModalButton ("OK"), "About has no usable OK button"))
        { ctx.complete (ctx.verdict()); return; }
        ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); }, "OK did not dismiss About");
    }, "About did not open");
    return std::nullopt;
}

const ScenarioRegistrar aboutDetails { Scenario {
    "gui.about_details", { "gui", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAboutDetails (host, ctx); }
} };
std::optional<ScenarioResult> runTimelineDrawer (GuiHost& host, ScenarioContext& ctx)
{
    const bool expanded = host.timelineViewMatches (true);
    ctx.cleanup ([&host, expanded]
    {
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    ctx.expect (host.timelineViewMatches (false), "the timeline is not collapsed at launch");
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
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar timelineDrawer { Scenario {
    "gui.timeline_drawer", { "gui", "timeline" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTimelineDrawer (host, ctx); }
} };
std::optional<ScenarioResult> runSunsetTrackDefaults (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    auto& dsp = engine.getChannelStrip (0);
    if (! ctx.expect (! dsp.isBuiltinLoaded(), "the fixture strip already contains a built-in unit"))
        return ctx.verdict();
    const int keyboard = engine.getVirtualKeyboardInputIndex();
    if (! ctx.expect (keyboard >= 0, "the virtual keyboard input is missing")) return ctx.verdict();
    ctx.cleanup ([&host, mode = track.mode.load()]
    {
        if (auto* strip = host.strip (0)) strip->restoreTrackMode (mode);
    });
    ctx.keep (track.mode);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.inputMonitor);
    ctx.cleanup ([&engine, &dsp, &track, identifier = track.midiInputIdentifier,
                  id = track.builtinUnitId, state = track.builtinStateBase64]
    {
        engine.suspendProcessing();
        dsp.unloadBuiltin();
        engine.resumeProcessing();
        track.midiInputIdentifier = identifier;
        track.builtinUnitId = id;
        track.builtinStateBase64 = state;
    });
    const auto stage = engine.getStage();
    ctx.cleanup ([&host, stage]
    {
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    host.switchToStage (GuiHost::Stage::Recording);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the fixture strip is missing")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool bound : { false, true })
    {
        steps->push_back ({ 0, [&track, strip, keyboard, bound]
        {
            if (! bound) track.mode.store ((int) Track::Mode::Mono);
            track.midiInputIndex.store (bound ? keyboard : -1);
            if (bound) strip->clickMonitor();
            else track.inputMonitor.store (false);
        } });
        steps->push_back ({ 100, [strip] { strip->loadBuiltin ("dusk.builtin.synth"); } });
        steps->push_back ({ 100, [&ctx, &track, &dsp, strip, keyboard, bound]
        {
            ctx.expect (dsp.getBuiltinSlot().isLoadedInstrument()
                        && track.builtinUnitId == "dusk.builtin.synth", "Sunset did not load as an instrument");
            ctx.expect (track.mode.load() == (int) Track::Mode::Midi, "loading Sunset did not convert the audio track to MIDI");
            ctx.expect (track.midiInputIndex.load() == keyboard && track.inputMonitor.load() == ! bound,
                        "instrument input defaults did not respect the existing binding");
            ctx.expect (strip->instrumentControlsMatch (keyboard, ! bound),
                        "the displayed mode, input or IN button disagrees with the instrument defaults");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar sunsetTrackDefaults { Scenario {
    "gui.sunset_track_defaults", { "gui", "plugin" }, Needs::Engine | Needs::Gui,
    {}, {}, 20000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSunsetTrackDefaults (host, ctx); }
} };
std::optional<ScenarioResult> runMasteringTransport (GuiHost& host, ScenarioContext& ctx)
{
    auto& player = ctx.engine().getMasteringPlayer();
    auto& transport = ctx.engine().getTransport();
    if (! ctx.expect (! player.isLoaded(), "the fixture already has a mastering file")) return ctx.verdict();
    const auto path = ctx.tempDir() / "mastering.wav";
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 2, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the mastering fixture")) return ctx.verdict();
    std::vector<float> silence (480000, 0.0f);
    const float* channels[] { silence.data(), silence.data() };
    if (! ctx.expect (writer->write (channels, 2, 480000), "could not write the mastering fixture"))
        return ctx.verdict();
    writer.reset();
    const auto stage = ctx.engine().getStage();
    ctx.cleanup ([&host, &ctx, &player, &transport, stage, source = ctx.session().mastering().sourceFile,
                  state = transport.getState(), position = transport.getPlayhead()]
    {
        drainModals (host);
        player.stop();
        player.unloadFile();
        // unloadFile is only the player's half: the view wrote the path into
        // the session too, and this one points inside a temp directory that
        // dies with the case. Left behind it is saved into every later
        // session and reloads as a missing file.
        ctx.session().mastering().sourceFile = source;
        host.refreshMasteringSource();
        host.switchToStage (guiStage (stage));
        transport.setPlayhead (position);
        transport.setState (state);
    });
    ctx.cleanup (host.preserveKeyboardFocus());
    host.switchToStage (GuiHost::Stage::Recording);
    ctx.expect (host.pressKey ("spacebar", ' ') && transport.isPlaying(), "Space did not start multitrack playback");
    ctx.expect (host.pressKey ("command + 3") && host.stageViewMatches (GuiHost::Stage::Mastering),
                "Cmd+3 did not select Mastering");
    ctx.expect (transport.isStopped(), "entering Mastering did not stop multitrack playback");
    if (! ctx.expect (host.loadMasteringFile (path), "Load mix did not accept the fixture")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const float fraction : { 0.25f, 0.75f })
    {
        steps->push_back ({ 300, [&host, &ctx, fraction]
        { ctx.expect (host.clickMasteringWaveform (fraction), "the Mastering waveform is not available for seeking"); } });
        steps->push_back ({ 100, [&ctx, &player, fraction]
        {
            const auto position = static_cast<double> (player.getPlayhead()) / static_cast<double> (player.getLengthSamples());
            ctx.expect (std::abs (position - fraction) < 0.01, "the waveform click did not seek to its horizontal position");
            ctx.expect (! player.isPlaying(), "clicking the stopped waveform started playback");
        } });
    }
    steps->push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickMasteringButton ("Play"), "Play is unavailable"); } });
    steps->push_back ({ 150, [&host, &ctx, &player, &transport]
    {
        ctx.expect (player.isPlaying() && player.getPlayhead() > 0, "Play did not advance the mastering player");
        ctx.expect (transport.isStopped(), "mastering Play started multitrack playback");
        ctx.expect (host.clickMasteringButton ("Stop"), "Stop is unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, &player]
    {
        ctx.expect (! player.isPlaying(), "Stop did not stop the mastering player");
        player.setPlayhead (96000);
        ctx.expect (host.clickMasteringButton ("|<<"), "Rewind is unavailable");
    } });
    steps->push_back ({ 100, [&host, &ctx, &player]
    {
        ctx.expect (player.getPlayhead() == 0, "Rewind did not return to the start");
        ctx.expect (host.pressKey ("spacebar", ' '), "Space was not handled in Mastering");
    } });
    steps->push_back ({ 150, [&host, &ctx, &player, &transport]
    {
        ctx.expect (player.isPlaying() && player.getPlayhead() > 0, "Space did not play the loaded mix");
        ctx.expect (transport.isStopped(), "Space in Mastering started multitrack playback");
        ctx.expect (host.pressKey ("spacebar", ' '), "second Space was not handled");
        ctx.expect (! player.isPlaying(), "second Space did not stop the loaded mix");
    } });
    steps->push_back ({ 100, [&host, &ctx, &player]
    {
        ctx.expect (host.pressKey ("spacebar", ' ') && player.isPlaying(), "could not restart Mastering before leaving");
        ctx.expect (host.pressKey ("command + 1") && host.stageViewMatches (GuiHost::Stage::Recording),
                    "Cmd+1 did not select Recording");
        ctx.expect (! player.isPlaying(), "leaving Mastering did not stop its player");
    } });
    steps->push_back ({ 100, [&host, &ctx, &transport]
    {
        ctx.expect (transport.isStopped(), "leaving Mastering started multitrack playback");
        ctx.expect (host.pressKey ("command + 2") && host.stageViewMatches (GuiHost::Stage::Mixing),
                    "Cmd+2 did not select Mixing");
        for (int page = 0; page < host.consolePageCount(); ++page)
            ctx.expect (host.pressKey (std::to_string (page + 1), (char) ('1' + page)) && host.consolePageMatches (page),
                        "a plain digit did not select its visible console page");
        ctx.expect (host.pressKey ("command + 4") && host.stageViewMatches (GuiHost::Stage::Aux),
                    "Cmd+4 did not select Aux");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.pressKey ("shift + /", '?'), "the shortcuts key was not handled");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (host.shortcutsOpen(), "? did not open Keyboard Shortcuts");
        host.closeTopModal();
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (! host.shortcutsOpen(), "Keyboard Shortcuts cleanup left the panel open"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar masteringTransport { Scenario {
    "gui.mastering_transport", { "gui", "mastering" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasteringTransport (host, ctx); }
} };
std::optional<ScenarioResult> runUnarmedRecord (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& transport = ctx.engine().getTransport();
    ctx.cleanup ([&host, &transport, state = transport.getState(), position = transport.getPlayhead()]
    {
        if (! host.modalStackEmpty()) host.closeTopModal();
        transport.setPlayhead (position);
        transport.setState (state);
    });
    std::array<std::size_t, Session::kNumTracks> audioCounts {}, midiCounts {};
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        auto& track = session.track (index);
        ctx.cleanup ([&session, index, armed = track.recordArmed.load()] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
        audioCounts[static_cast<std::size_t> (index)] = track.regions.size();
        midiCounts[static_cast<std::size_t> (index)] = track.midiRegions.current().size();
    }
    transport.setState (Transport::State::Stopped);
    transport.setPlayhead (48000);
    ctx.expect (host.pressKey ("R", 'r'), "Record shortcut was not handled");
    ctx.waitUntil ([&host] { return ! host.modalStackEmpty(); }, 3000,
        [&host, &ctx, &session, &transport, audioCounts, midiCounts]
        {
            ctx.expect (transport.isStopped() && transport.getPlayhead() == 48000,
                        "unarmed Record changed transport state or position");
            ctx.expect (host.modalText().find ("Cannot record\nNo track is armed.") == 0,
                        "unarmed Record did not explain the refusal");
            for (int index = 0; index < Session::kNumTracks; ++index)
                ctx.expect (session.track (index).regions.size() == audioCounts[static_cast<std::size_t> (index)]
                            && session.track (index).midiRegions.current().size() == midiCounts[static_cast<std::size_t> (index)],
                            "unarmed Record changed a track's regions");
            if (! ctx.expect (host.clickModalButton ("OK"), "the refusal has no usable OK button"))
            { ctx.complete (ctx.verdict()); return; }
            ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                           [&ctx] { ctx.complete (ctx.verdict()); }, "the refusal did not close");
        }, "unarmed Record did not show its refusal");
    return std::nullopt;
}

const ScenarioRegistrar unarmedRecord { Scenario {
    "gui.record_requires_armed_track", { "gui", "recording" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runUnarmedRecord (host, ctx); }
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
    region.notes = { { 1, 60, 101, 240, 120 }, { 3, 67, 85, 600, 240 } };
    const auto original = region.notes;
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { region }));
    undo.clearUndoHistory();
    host.openPianoRoll (0, 0);
    ctx.later (300, [&host, &ctx, &track, original]
    {
        ctx.expect (host.pressPianoRollKey ("command + A"), "the piano roll did not select all notes");
        ctx.expect (host.pressPianoRollKey ("5"), "the piano roll did not select the sixteenth-note grid");
        for (const auto& action : std::array<std::pair<const char*, int>, 4> {
                 std::pair<const char*, int> { "cursor up", 1 }, { "cursor down", -1 },
                 { "cursor right", 120 }, { "cursor left", -120 } })
        {
            ctx.expect (host.pressPianoRollKey ("command + A"), "the piano roll did not reselect notes after undo");
            ctx.expect (host.pressPianoRollKey (action.first), std::string (action.first) + " was not handled");
            auto expected = original;
            for (auto& note : expected)
                if (std::abs (action.second) == 1) note.noteNumber += action.second;
                else note.startTick += action.second;
            const auto& edited = track.midiRegions.current();
            ctx.expect (edited.size() == 1 && edited[0].notes == expected,
                        std::string (action.first) + " changed the wrong note fields or amount");
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

std::optional<ScenarioResult> runMixdownHandoff (GuiHost& host, ScenarioContext& ctx)
{
    auto& player = ctx.engine().getMasteringPlayer();
    auto& track = ctx.session().track (0);
    if (! ctx.expect (! player.isLoaded(), "the fixture already has a mastering source")) return ctx.verdict();
    ctx.keep (track.mode);
    keepStage (host, ctx);
    ctx.cleanup ([&host, &ctx, &track, &player, regions = track.regions,
                  source = ctx.session().mastering().sourceFile]
    {
        drainModals (host);
        player.stop();
        player.unloadFile();
        ctx.session().mastering().sourceFile = source;
        track.regions = regions;
    });
    const auto input = ctx.tempDir() / "bounce-source.wav";
    auto writer = dusk::audio::FileWriter::create (input, { 48000.0, 1, 24 });
    if (! ctx.expect (writer != nullptr, "could not create the bounce fixture")) return ctx.verdict();
    std::vector<float> silence (4800, 0.0f);
    const float* channels[] { silence.data() };
    if (! ctx.expect (writer->write (channels, 1, 4800), "could not write the bounce fixture")) return ctx.verdict();
    writer.reset();
    AudioRegion region;
    region.file = decltype (region.file) (input.u8string().c_str());
    region.lengthInSamples = 4800;
    track.regions = { region };
    track.mode.store ((int) Track::Mode::Mono);
    host.switchToStage (GuiHost::Stage::Mixing);
    host.startMixdown();
    ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 30000,
        [&host, &ctx, &player]
        {
            ctx.waitUntil ([&host, &player]
            { return host.stageViewMatches (GuiHost::Stage::Mastering) && player.isLoaded(); }, 5000,
                [&host, &ctx, &player]
                {
                    const auto expected = ctx.session().getSessionDirectory().getChildFile ("mixdown.wav");
                    ctx.expect (player.getLoadedFile() == expected, "Mastering loaded a different file than mixdown.wav");
                    ctx.expect (ctx.session().mastering().sourceFile == expected, "the session did not retain the mixdown source");
                    auto reader = dusk::audio::FileReader::open (expected.getFullPathName().toStdString());
                    if (ctx.expect (reader != nullptr, "Mixdown did not write a readable WAV in the session folder"))
                    {
                        ctx.expect (reader->info().numChannels == 2 && reader->info().bitsPerSample == 24,
                                    "Mixdown did not write stereo 24-bit audio");
                        ctx.expect (reader->info().numFrames >= 4800, "Mixdown truncated the session");
                    }
                    const auto fallback = expected.getSiblingFile ("bounce.wav");
                    if (! ctx.expect (! fallback.existsAsFile(), "the fixture already has a fallback bounce"))
                    {
                        ctx.complete (ctx.verdict());
                        return;
                    }
                    ctx.cleanup ([expected, fallback]
                    {
                        if (! expected.existsAsFile()) fallback.moveFileTo (expected);
                        else fallback.deleteFile();
                    });
                    auto steps = std::make_shared<std::vector<Step>>();
                    steps->push_back ({ 100, [&host, &ctx, &player, expected, fallback]
                    {
                        player.unloadFile();
                        ctx.expect (expected.moveFileTo (fallback), "could not prepare the fallback-only fixture");
                        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "Load latest mixdown is unavailable");
                    } });
                    steps->push_back ({ 200, [&ctx, &player, fallback]
                    {
                        ctx.expect (player.isLoaded() && player.getLoadedFile() == fallback,
                                    "Load latest mixdown did not fall back to bounce.wav");
                        ctx.expect (ctx.session().mastering().sourceFile == fallback,
                                    "the session did not retain the fallback source");
                    } });
                    steps->push_back ({ 100, [&host, &ctx, &player, expected, fallback]
                    {
                        player.unloadFile();
                        ctx.expect (fallback.copyFileTo (expected), "could not prepare both mix candidates");
                        ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "Load latest mixdown is unavailable");
                    } });
                    steps->push_back ({ 200, [&ctx, &player, expected]
                    {
                        ctx.expect (player.isLoaded() && player.getLoadedFile() == expected,
                                    "Load latest mixdown did not prefer mixdown.wav over bounce.wav");
                        ctx.expect (ctx.session().mastering().sourceFile == expected,
                                    "the session did not retain the preferred source");
                    } });
                    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
                }, "closing the completed Mixdown did not load Mastering");
        }, "Mixdown did not expose its completion Close button");
    return std::nullopt;
}

const ScenarioRegistrar mixdownHandoff { Scenario {
    "gui.mixdown_handoff", { "gui", "bounce" }, Needs::Engine | Needs::Gui,
    {}, {}, 40000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMixdownHandoff (host, ctx); }
} };

std::optional<ScenarioResult> runAuxSelectors (GuiHost& host, ScenarioContext& ctx)
{
    const auto stage = ctx.engine().getStage();
    host.switchToStage (GuiHost::Stage::Aux);
    const int original = host.activeAuxLane();
    ctx.cleanup ([&host, original, stage]
    {
        host.switchToStage (GuiHost::Stage::Aux);
        host.clickAuxSelector (original);
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int lane : { 3, 1, 0, 2 })
    {
        steps->push_back ({ 100, [&host, &ctx, lane]
        { ctx.expect (host.clickAuxSelector (lane), "the AUX selector is unavailable"); } });
        steps->push_back ({ 100, [&host, &ctx, lane]
        {
            ctx.expect (host.auxLaneLayoutMatches (lane), "AUX did not show exactly the selected lane at full width");
            host.switchToStage (GuiHost::Stage::Mixing);
        } });
        steps->push_back ({ 100, [&host] { host.switchToStage (GuiHost::Stage::Aux); } });
        steps->push_back ({ 100, [&host, &ctx, lane]
        { ctx.expect (host.auxLaneLayoutMatches (lane), "changing stages lost the selected AUX lane"); } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar auxSelectors { Scenario {
    "gui.aux_selectors", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxSelectors (host, ctx); }
} };

std::optional<ScenarioResult> runAccessibleControls (GuiHost& host, ScenarioContext& ctx)
{
    auto& strip = ctx.session().track (0).strip;
    ctx.keep (strip.faderDb);
    ctx.keep (strip.pan);
    ctx.keep (strip.hpfFreq);
    ctx.keep (strip.compFetRatio);
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Recording);
    std::string value, help;
    for (int track = 1; track <= Session::kNumTracks; ++track)
        for (const auto* suffix : { "fader", "pan", "mute", "solo", "record arm", "input monitor",
                                   "high-pass filter frequency", "low-pass filter frequency", "insert slot" })
        {
            const auto title = "Track " + std::to_string (track) + " " + suffix;
            ctx.expect (host.accessibleControl (title, value, help), "missing accessible name: " + title);
        }
    for (const auto* title : { "Play", "Stop", "Record", "Rewind", "Fast forward" })
        ctx.expect (host.accessibleControl (title, value, help), std::string ("missing transport accessible name: ") + title);
    host.switchToStage (GuiHost::Stage::Mixing);
    for (int track = 1; track <= Session::kNumTracks; ++track)
        for (int aux = 1; aux <= Session::kNumAuxLanes; ++aux)
        {
            const auto title = "Track " + std::to_string (track) + " aux " + std::to_string (aux) + " send";
            ctx.expect (host.accessibleControl (title, value, help), "missing accessible name: " + title);
        }
    host.switchToStage (GuiHost::Stage::Aux);
    for (int aux = 1; aux <= Session::kNumAuxLanes; ++aux)
        for (const auto* suffix : { "return fader", "mute", "plugin slot 1" })
        {
            const auto title = "Aux " + std::to_string (aux) + " " + suffix;
            ctx.expect (host.accessibleControl (title, value, help), "missing accessible name: " + title);
        }
    host.switchToStage (GuiHost::Stage::Mixing);
    auto steps = std::make_shared<std::vector<Step>>();
    struct Value { const char* title; const char* input; const char* output; };
    const Value values[] {
        { "Track 1 fader", "-4.2", "-4.2 dB" },
        { "Track 1 pan", "-0.42", "L42" },
        { "Track 1 high-pass filter frequency", "OFF", "OFF" },
        { "FET ratio", "0", "4:1" },
        { "Track 1 fader", "-90", "-INF dB" },
        { "Track 1 fader", "-96", "-INF dB" },
        { "Track 1 fader", "-INF", "-INF dB" },
        { "Track 1 fader", "-4.2", "-4.2 dB" },
        { "Track 1 fader", "-INF dB", "-INF dB" }
    };
    for (const auto item : values)
    {
        steps->push_back ({ 100, [&host, &ctx, item]
        { ctx.expect (host.setAccessibleValue (item.title, item.input), "the accessible value action is unavailable"); } });
        steps->push_back ({ 100, [&host, &ctx, &strip, item]
        {
            std::string formatted, hint;
            ctx.expect (host.accessibleControl (item.title, formatted, hint) && formatted == item.output,
                        std::string (item.title) + " formatted value: expected " + item.output + ", got " + formatted);
            ctx.expect (! hint.empty(), std::string (item.title) + " has no accessible help");
            if (std::string (item.input).find ("-INF") == 0)
                ctx.expect (std::abs (strip.faderDb.load() - ChannelStripParams::kFaderMinDb) < 0.001f,
                            "the accessible -INF action did not mute the channel fader");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar accessibleControls { Scenario {
    "gui.accessible_controls", { "gui", "accessibility" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAccessibleControls (host, ctx); }
} };

std::optional<ScenarioResult> runWindowKeys (GuiHost& host, ScenarioContext& ctx)
{
    const bool fullscreen = host.fullScreen();
    const bool expanded = host.timelineViewMatches (true);
    ctx.cleanup ([&host, fullscreen, expanded]
    {
        host.closeTopModal();
        if (host.fullScreen() != fullscreen) host.pressKey ("F11");
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
    });
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool restore : { false, true })
    {
        steps->push_back ({ 100, [&host, &ctx]
        { ctx.expect (host.pressKey ("F11"), "F11 was not handled"); } });
        steps->push_back ({ 300, [&host, &ctx, fullscreen, restore]
        { ctx.expect (host.fullScreen() == (restore ? fullscreen : ! fullscreen), "F11 did not toggle the native window state"); } });
        steps->push_back ({ 100, [&host, &ctx]
        { ctx.expect (host.pressKey ("command + \\", '\\'), "the timeline window shortcut was not handled"); } });
        steps->push_back ({ 100, [&host, &ctx, expanded, restore]
        { ctx.expect (host.timelineViewMatches (restore ? expanded : ! expanded), "the timeline shortcut did not toggle visibility"); } });
    }
    steps->push_back ({ 100, [&host] { host.openAbout(); } });
    steps->push_back ({ 200, [&host, &ctx]
    {
        ctx.expect (! host.modalStackEmpty(), "About did not open for the Escape check");
        ctx.expect (host.pressPeerKey ("escape"), "the focused modal did not handle Escape");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.modalStackEmpty(), "Escape left the modal open"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar windowKeys { Scenario {
    "gui.window_keys", { "gui", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runWindowKeys (host, ctx); }
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
        ctx.expect (host.pressAudioEditorKey ("command + E"), "the editor did not handle Split");
    } });
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

std::optional<ScenarioResult> runRecordingUndoKey (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    ctx.keep (track.mode);
    ctx.keep (track.midiInputIndex);
    ctx.keep (session.countInEnabled);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
    }
    ctx.cleanup ([&engine, &transport, &track, regions = track.midiRegions.current(),
                  loop = transport.isLoopEnabled(), punch = transport.isPunchEnabled(),
                  position = transport.getPlayhead()]
    {
        const std::uint8_t off[] { 0x80, 64, 0 };
        engine.postVirtualKeyboardMidi (off, 3);
        engine.stop();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        transport.setLoopEnabled (loop);
        transport.setPunchEnabled (punch);
        transport.setPlayhead (position);
        engine.getUndoManager().clearUndoHistory();
    });
    engine.stop();
    transport.setPlayhead (0);
    transport.setLoopEnabled (false);
    transport.setPunchEnabled (false);
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (engine.getVirtualKeyboardInputIndex());
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    session.setTrackArmed (0, true);
    engine.getUndoManager().clearUndoHistory();
    host.switchToStage (GuiHost::Stage::Recording);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressKey ("R", 'r'), "the Record key was not handled"); } });
    steps->push_back ({ 100, [&engine, &transport, &ctx]
    {
        ctx.expect (transport.isRecording(), "R did not start the armed MIDI recording");
        const std::uint8_t on[] { 0x90, 64, 100 };
        engine.postVirtualKeyboardMidi (on, 3);
    } });
    steps->push_back ({ 150, [&engine]
    {
        const std::uint8_t off[] { 0x80, 64, 0 };
        engine.postVirtualKeyboardMidi (off, 3);
    } });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressKey ("spacebar", ' '), "Space did not stop recording"); } });
    steps->push_back ({ 100, [&host, &ctx, &track, &transport]
    {
        ctx.expect (transport.isStopped(), "recording did not stop");
        const auto& regions = track.midiRegions.current();
        if (ctx.expect (regions.size() == 1, "recording did not commit exactly one MIDI region"))
            ctx.expect (regions[0].notes.size() == 1 && regions[0].notes[0].noteNumber == 64,
                        "recording did not retain the virtual keyboard note");
        ctx.expect (host.pressKey ("command + Z"), "the recording Undo shortcut was not handled");
    } });
    steps->push_back ({ 100, [&ctx, &track]
    {
        ctx.expect (track.midiRegions.current().empty(), "Cmd+Z did not undo the recorded take");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar recordingUndoKey { Scenario {
    "gui.recording_undo_key", { "gui", "recording", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordingUndoKey (host, ctx); }
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

std::optional<ScenarioResult> runRecordingSetupAlert (GuiHost& host, ScenarioContext& ctx, bool keyboard = false)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    ctx.keep (session.countInEnabled);
    session.countInEnabled.store (false);
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const bool armed = session.track (index).recordArmed.load();
        ctx.cleanup ([&session, index, armed] { session.setTrackArmed (index, armed); });
        session.setTrackArmed (index, false);
    }
    const auto audioDir = std::filesystem::u8path (session.getAudioDirectory().getFullPathName().toStdString());
    std::error_code error;
    if (! ctx.expect (std::filesystem::remove (audioDir, error) && ! error,
                     "the temporary audio directory was not empty")) return ctx.verdict();
    ctx.cleanup ([&host, &engine, audioDir]
    {
        engine.stop();
        host.closeTopModal();
        std::error_code ignored;
        std::filesystem::remove (audioDir, ignored);
        std::filesystem::create_directories (audioDir, ignored);
    });
    {
        std::ofstream obstruction (audioDir);
        obstruction << "not a directory";
        if (! ctx.expect (obstruction.good(), "could not create the audio-directory obstruction")) return ctx.verdict();
    }
    for (const int index : { 0, 2 })
    {
        auto& track = session.track (index);
        ctx.keep (track.mode);
        ctx.keep (track.inputSource);
        track.mode.store ((int) Track::Mode::Mono);
        track.inputSource.store (0);
        session.setTrackArmed (index, true);
    }
    if (! ctx.expect (keyboard ? host.pressKey ("R", 'r') : host.clickRecord(),
                     "the recording input was not handled")) return ctx.verdict();
    ctx.waitUntil ([&host] { return ! host.modalStackEmpty(); }, 3000,
        [&host, &ctx, &engine]
        {
            const auto text = host.modalText();
            ctx.expect (text.find ("Recording setup failed\n") == 0
                        && text.find ("Tracks 1, 3") != std::string::npos,
                        "the setup failure did not name the two failed tracks");
            ctx.expect (engine.getRecordManager().getLastSetupFailures() == std::vector<int> { 0, 2 },
                        "the dialog did not correspond to the failed writers");
            ctx.expect (text.find ("NOT capturing audio") != std::string::npos,
                        "the setup failure did not explain the capture loss");
            engine.stop();
            ctx.expect (host.clickModalButton ("OK"), "the setup failure has no usable OK button");
            ctx.waitUntil ([&host] { return host.modalStackEmpty(); }, 3000,
                           [&ctx] { ctx.complete (ctx.verdict()); }, "OK did not dismiss the setup failure");
        }, "failed writers did not produce a recording setup alert");
    return std::nullopt;
}

const ScenarioRegistrar recordingSetupAlert { Scenario {
    "gui.recording_setup_alert", { "gui", "recording", "messages" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordingSetupAlert (host, ctx); }
} };

const ScenarioRegistrar recordingSetupAlertKey { Scenario {
    "gui.recording_setup_alert_key", { "gui", "recording", "messages", "keyboard" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runRecordingSetupAlert (host, ctx, true); }
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
        ctx.expect (host.pressPeerKey ("command + ]", ']'), "the piano roll did not handle next region");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (host.pianoRollRegion() == 1, "next region did not select the second MIDI region");
        ctx.expect (host.pressPeerKey ("command + [", '['), "the piano roll did not handle previous region");
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
        ctx.expect (host.wheelPianoVelocity (0.5f), "the velocity strip did not accept a wheel gesture");
    } });
    steps->push_back ({ 100, [&host, &ctx, height]
    {
        ctx.expect (host.pianoVelocityHeight() == *height + 16, "wheel-up did not grow the velocity strip");
        ctx.expect (host.wheelPianoVelocity (-0.5f), "the velocity strip did not accept wheel-down");
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

const ScenarioRegistrar firstLaunch { Scenario {
    "gui.first_launch", { "gui", "startup" }, Needs::Engine | Needs::Gui,
    {}, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        for (const auto& error : host.firstLaunchErrors())
            ctx.expect (false, error);
        return ctx.verdict();
    }
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

std::optional<ScenarioResult> runTempoEntry (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! ctx.expect (transport.isStopped(), "tempo entry requires a stopped fixture")) return ctx.verdict();
    ctx.cleanup ([&host, &engine, &session, &transport, points = session.tempoMap.points(),
                  bpm = session.tempoBpm.load(), position = transport.getPlayhead()]
    {
        host.closeTopModal();
        session.tempoBpm.store (bpm);
        engine.setTempoPoints (points);
        transport.setPlayhead (position);
        engine.getUndoManager().clearUndoHistory();
    });
    session.tempoBpm.store (120.0f);
    engine.setTempoPoints ({});
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool mapped : { false, true })
    {
        steps->push_back ({ 500, [&host, &ctx, &engine, &transport, mapped]
        {
            if (mapped) engine.setTempoPoints ({ { 0, 100.0f }, { 48000, 120.0f }, { 96000, 140.0f } });
            transport.setPlayhead (mapped ? 72000 : 0);
            ctx.expect (host.doubleClickTempo(), "the BPM readout was unavailable for double-click");
        } });
        steps->push_back ({ 200, [&host, &ctx, mapped]
        {
            if (! ctx.expect (host.focusModalTextInput(), "double-clicking BPM did not open a text input")) return;
            ctx.expect (host.pressPeerKey ("command + A"), "the tempo input did not accept Select All");
            const std::string text = mapped ? "127.6" : "133.5";
            for (const char character : text)
                ctx.expect (host.pressPeerKey (std::string (1, character), character), "the tempo input rejected a character");
            ctx.expect (host.pressPeerKey ("return"), "the tempo input did not accept Return");
        } });
        steps->push_back ({ 300, [&host, &ctx, &session, &transport, mapped]
        {
            ctx.expect (host.modalStackEmpty(), "accepting tempo left the prompt open");
            if (! mapped)
                ctx.expect (session.tempoMap.empty() && std::abs (session.tempoBpm.load() - 133.5f) < 0.001f,
                            "constant-tempo entry did not retain its fractional BPM");
            else
            {
                const auto points = session.tempoMap.points();
                if (! ctx.expect (points.size() == 3, "editing mapped tempo changed the point count")) return;
                ctx.expect (points[0].timelineSamples == 0 && points[1].timelineSamples == 48000
                            && points[2].timelineSamples == 96000, "editing BPM moved a tempo point");
                ctx.expect (std::abs (points[0].bpm - 100.0f) < 0.001f
                            && std::abs (points[1].bpm - 127.6f) < 0.001f
                            && std::abs (points[2].bpm - 140.0f) < 0.001f
                            && std::abs (session.tempoBpm.load() - 100.0f) < 0.001f,
                            "BPM entry did not edit only the point governing the playhead");
                ctx.expect (transport.getPlayhead() == 72000, "editing BPM moved the stopped playhead");
            }
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar tempoEntry { Scenario {
    "gui.tempo_entry", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTempoEntry (host, ctx); }
} };

std::optional<ScenarioResult> runAutomationMenu (GuiHost& host, ScenarioContext& ctx)
{
    auto& track = ctx.session().track (0);
    ctx.keep (track.automationMode);
    ctx.cleanup ([&host, stage = ctx.engine().getStage()]
    {
        host.closeTopModal();
        switch (stage)
        {
            case AudioEngine::Stage::Recording: host.switchToStage (GuiHost::Stage::Recording); break;
            case AudioEngine::Stage::Mixing: host.switchToStage (GuiHost::Stage::Mixing); break;
            case AudioEngine::Stage::Aux: host.switchToStage (GuiHost::Stage::Aux); break;
            case AudioEngine::Stage::Mastering: host.switchToStage (GuiHost::Stage::Mastering); break;
        }
    });
    host.switchToStage (GuiHost::Stage::Mixing);
    auto* strip = host.strip (0);
    if (! ctx.expect (strip != nullptr, "the channel strip is unavailable")) return ctx.verdict();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const int mode : { 1, 2, 3, 0 })
    {
        steps->push_back ({ 500, [strip] { strip->clickAutomationMode(); } });
        steps->push_back ({ 200, [&host, &ctx, mode]
        {
            ctx.expect (! host.modalStackEmpty(), "clicking the automation label did not open its menu");
            ctx.expect (host.clickModalAt (0.5f, (static_cast<float> (mode) + 0.5f) / 4.0f),
                        "the automation menu row was unavailable");
        } });
        steps->push_back ({ 200, [&host, &ctx, &track, mode]
        {
            static constexpr const char* labels[] { "OFF", "READ", "WRITE", "TOUCH" };
            ctx.expect (host.modalStackEmpty(), "choosing an automation mode left its menu open");
            ctx.expect (track.automationMode.load() == mode, "the menu selected the wrong automation mode");
            std::string label;
            bool faderEnabled = false;
            ctx.expect (host.automationView (GuiHost::StripKind::Channel, 0, label, faderEnabled)
                        && label == labels[mode] && faderEnabled == (mode != 1),
                        "the mode label or fader input state disagrees with the menu selection");
        } });
    }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar automationMenu { Scenario {
    "gui.automation_menu", { "gui", "automation" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAutomationMenu (host, ctx); }
} };

std::optional<ScenarioResult> runPunchMenu (GuiHost& host, ScenarioContext& ctx)
{
    auto& session = ctx.session();
    if (! host.modalStackEmpty()) return ScenarioResult::skip ("requires no modal");
    ctx.keep (session.preRollEnabled);
    ctx.keep (session.postRollEnabled);
    ctx.keep (session.preRollSeconds);
    ctx.keep (session.postRollSeconds);
    ctx.cleanup ([&host] { drainModals (host); });
    session.preRollEnabled.store (false);
    session.postRollEnabled.store (false);
    session.preRollSeconds.store (0.0f);
    session.postRollSeconds.store (0.0f);
    const bool punch = ctx.engine().getTransport().isPunchEnabled();
    auto steps = std::make_shared<std::vector<Step>>();
    for (const bool enabled : { true, false })
        for (const bool post : { false, true })
        {
            steps->push_back ({ 500, [&host, &ctx]
            { ctx.expect (host.rightClickPunch(), "the Punch button was unavailable for right-click"); } });
            steps->push_back ({ 150, [&host, &ctx, post]
            { ctx.expect (host.clickModalAt (0.5f, (post ? 112.0f : 48.0f) / 166.0f), "the roll enable row was unavailable"); } });
            steps->push_back ({ 150, [&host, &ctx, &session, post, enabled, punch]
            {
                ctx.expect ((post ? session.postRollEnabled.load() : session.preRollEnabled.load()) == enabled,
                            "the Punch menu did not toggle the selected roll mode");
                ctx.expect (ctx.engine().getTransport().isPunchEnabled() == punch,
                            "a Punch context-menu gesture toggled punch recording");
                ctx.expect (host.modalStackEmpty(), "choosing a roll enable row left its menu open");
            } });
        }
    for (const bool off : { false, true })
        for (const bool post : { false, true })
        {
            steps->push_back ({ 500, [&host, &ctx]
            { ctx.expect (host.rightClickPunch(), "the Punch button was unavailable for its preset menu"); } });
            steps->push_back ({ 150, [&host, &ctx, post]
            { ctx.expect (host.clickModalAt (0.5f, (post ? 144.0f : 80.0f) / 166.0f), "the roll-duration submenu was unavailable"); } });
            steps->push_back ({ 150, [&host, &ctx, post, off]
            {
                const int row = off ? 0 : post ? 4 : 2;
                ctx.expect (host.clickModalAt (0.5f, (static_cast<float> (row) + 0.5f) / 6.0f),
                            "the roll-duration preset was unavailable");
            } });
            steps->push_back ({ 150, [&host, &ctx, &session, post, off]
            {
                const float expected = off ? 0.0f : post ? 5.0f : 2.0f;
                ctx.expect (std::abs ((post ? session.postRollSeconds.load() : session.preRollSeconds.load()) - expected) < 0.001f,
                            "the Punch submenu did not set the selected roll duration");
                ctx.expect (host.modalStackEmpty(), "choosing a preset left its parent or submenu open");
            } });
        }
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar punchMenu { Scenario {
    "gui.punch_menu", { "gui", "transport" }, Needs::Engine | Needs::Gui,
    {}, {}, 15000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPunchMenu (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario
