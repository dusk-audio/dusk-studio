#include "GuiHost.h"

#include "../engine/AudioEngine.h"
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
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
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
        while (! host.modalStackEmpty()) host.closeTopModal();
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
        while (! host.modalStackEmpty()) host.closeTopModal();
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
        while (! host.modalStackEmpty()) host.closeTopModal();
        engine.setRenderOversamplingOverride (0);
        engine.reattachAudioCallback();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        if (originalFile.existsAsFile()) player.loadFile (originalFile);
        else player.unloadFile();
        player.setPlayhead (originalPosition);
        host.refreshMasteringSource();
        host.switchToStage (originalStage == AudioEngine::Stage::Recording ? GuiHost::Stage::Recording
                            : originalStage == AudioEngine::Stage::Mixing ? GuiHost::Stage::Mixing
                            : originalStage == AudioEngine::Stage::Aux ? GuiHost::Stage::Aux : GuiHost::Stage::Mastering);
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
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 400, [&host, &ctx]
    { ctx.expect (host.clickMasteringButton ("Load latest mixdown"), "latest mixdown button unavailable"); } });
    steps->push_back ({ 600, [&host, &ctx, &player, source]
    {
        ctx.expect (player.isLoaded() && player.getLoadedFile().getFullPathName().toStdString() == source.string(),
                    "latest mixdown did not load the session mix");
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
    runSteps (ctx, steps, [&host, &ctx, &engine, output]
    {
        ctx.waitUntil ([&host] { return host.clickModalButton ("Close"); }, 20000,
            [&ctx, &engine, output]
            {
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
        while (! host.modalStackEmpty()) host.closeTopModal();
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
        while (! host.modalStackEmpty()) host.closeTopModal();
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
        while (! host.modalStackEmpty()) host.closeTopModal();
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
        while (! host.modalStackEmpty()) host.closeTopModal();
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
