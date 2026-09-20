#include "GuiHost.h"

#include "../engine/AudioEngine.h"
#include "../engine/audiofile/FileWriter.h"
#include "../engine/PluginSlot.h"
#include "../engine/scenario/Scenario.h"
#include "../engine/scenario/ScenarioContext.h"
#include "../engine/scenario/cases/OopStubHarness.h"
#include "../dsp/ChannelStrip.h"
#include "../session/Session.h"
#include "../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

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

StripHandle* readyStrip (GuiHost& host)
{
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

    auto* strip = readyStrip (host);
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

    auto* strip = readyStrip (host);
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

    auto* strip = readyStrip (host);
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

    auto* strip = readyStrip (host);
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

    auto* strip = readyStrip (host);
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
        host.switchToStage (originalStage == AudioEngine::Stage::Recording ? GuiHost::Stage::Recording
                            : originalStage == AudioEngine::Stage::Mixing ? GuiHost::Stage::Mixing
                            : originalStage == AudioEngine::Stage::Aux ? GuiHost::Stage::Aux
                            : GuiHost::Stage::Mastering);
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
    const auto originalDir = currentSessionDirectory (session);
    const auto restoreFile = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restoreFile))
        return ScenarioResult::fail ("could not save the initial session");
    ctx.cleanup ([&host, &session, &engine, originalDir, restoreFile]
    {
        engine.stop();
        if (! host.modalStackEmpty()) host.closeTopModal();
        host.openSession (restoreFile);
        applySessionDirectory (session, originalDir);
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
    steps->push_back ({ 200, [&host, &ctx, &session, outgoing]
    {
        MidiRegion region;
        region.lengthInSamples = static_cast<std::int64_t> (ctx.engine().getCurrentSampleRate() * 600.0);
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
    ctx.cleanup ([&session, &fader, original, originalDir]
    {
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

// ------------------------------------------------------------- registration

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
} // namespace
} // namespace duskstudio::scenario
