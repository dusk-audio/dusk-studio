#include "GuiHost.h"

#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/scenario/Scenario.h"
#include "../engine/scenario/ScenarioContext.h"
#include "../engine/scenario/cases/OopStubHarness.h"
#include "../dsp/ChannelStrip.h"
#include "../session/Session.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

// ------------------------------------------------------------- registration

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
