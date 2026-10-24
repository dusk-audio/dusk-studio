#include "../GuiScenarioSupport.h"
#include "../../../engine/builtin/BuiltinRegistry.h"
#include "../../../engine/PluginSlot.h"
#include "../../../engine/scenario/cases/OopStubHarness.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/ParamEditAction.h"
#include "../../../session/SessionSerializer.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if DUSKSTUDIO_HAS_NATIVE_LV2
 #include <unistd.h>
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (_WIN32)
 #include <csignal>
#endif

namespace duskstudio::scenario::gui
{
namespace
{
// ------------------------------------------------------- editor open / close

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
    // multi_bus.clap ships no CLAP_EXT_GUI, the LV2 control-state plugin no UI
    // and the VST3 one only a lifecycle-probe view with no platform to attach to, so they
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
   #elif ! defined (__linux__)
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("only the Linux host looks for a plug-in window in the container");
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
    // A wait that times out ends the case without finish below.
    ctx.cleanup ([&host, strip]
    {
        strip->closeEditor();
        dismissAlert (host);
        strip->unloadNativePlugins();
        strip->refreshInsertButton();
    });

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

#if DUSKSTUDIO_HAS_NATIVE_LV2
// The file the fixture UI (tests/fixtures/file_state_ui.cpp) rewrites with each
// Gain value the host sends it, once this process has created it.
std::filesystem::path lv2UiProbePath()
{
    std::error_code error;
    auto dir = std::filesystem::temp_directory_path (error);
    if (error) dir = "/tmp";
    return dir / ("dusk-lv2-ui-probe-" + std::to_string ((long) ::getpid()));
}

std::optional<double> readLv2UiProbe (const std::filesystem::path& path)
{
    std::ifstream in (path);
    double value = 0.0;
    if (in >> value) return value;
    return std::nullopt;
}
#endif

std::optional<ScenarioResult> runLv2EditorReflectsState (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_LV2
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("built without native LV2 hosting");
   #else
    static constexpr double kOpenGain = 0.75;
    static constexpr double kLiveGain = 0.5;
    static constexpr double kReopenGain = 0.375;
    static constexpr double kTolerance = 1.0e-4;
    static constexpr int kSeedTimeoutMs = 3000;

    const auto fixture = ctx.fixture ("file_state.lv2");
    if (! fixture)
        return ScenarioResult::skip ("missing fixture: file_state.lv2");

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    const auto probe = lv2UiProbePath();
    ctx.cleanup ([strip, probe]
    {
        strip->closeEditor();
        strip->unloadNativePlugins();
        strip->refreshInsertButton();
        std::error_code ignored;
        std::filesystem::remove (probe, ignored);
    });

    // The file-state plug-in is the one the fixture bundle gives a UI.
    std::string error;
    if (! strip->loadNativeLv2 (*fixture, "urn:duskstudio:test:file-state", error))
        return ScenarioResult::fail ("the fixture did not load: " + error);
    strip->refreshInsertButton();

    auto& slot = ctx.engine().getChannelStrip (kStripIndex).getNativeLv2Slot();
    std::optional<std::uint32_t> gainPort;
    for (int i = 0; i < slot.paramCount(); ++i)
        if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == "Gain")
            gainPort = info->id;
    if (! gainPort)
        return ScenarioResult::fail ("the fixture exposes no Gain control");
    const auto gain = *gainPort;
    slot.setParamValue (gain, kOpenGain);

    {
        std::ofstream create (probe, std::ios::trunc);
        if (! create)
            return ScenarioResult::fail ("could not create the editor probe file");
    }

    const auto seeded = [probe] (double expected)
    {
        return [probe, expected]
        {
            const auto shown = readLv2UiProbe (probe);
            return shown && std::abs (*shown - expected) < kTolerance;
        };
    };

    ctx.later (200, [&ctx, &host, &slot, strip, gain, seeded]
    {
        if (! strip->openEditor())
        {
            dismissAlert (host);
            if (host.canEmbedPluginEditors())
            {
               #if defined (__linux__)
                ctx.complete (ScenarioResult::fail (
                    "the fixture's X11 UI did not open on a display that embeds editors"));
               #else
                ctx.complete (ScenarioResult::skip (
                    "the fixture's only UI is an X11 widget, built on Linux alone"));
               #endif
            }
            else
            {
                ctx.complete (ScenarioResult::skip (
                    "this display cannot embed a plug-in editor"));
            }
            return;
        }

        ctx.waitUntil (seeded (kOpenGain), kSeedTimeoutMs,
            [&ctx, &slot, strip, gain, seeded]
            {
                double held = 0.0;
                if (ctx.expect (strip->readEditorControl ("Gain", held),
                                "the open editor reported no Gain control"))
                    ctx.expect (std::abs (held - kOpenGain) < kTolerance,
                                "opening the editor moved the plug-in's Gain to "
                                    + std::to_string (held));

                slot.setParamValue (gain, kLiveGain);
                ctx.waitUntil (seeded (kLiveGain), kSeedTimeoutMs,
                    [&ctx, &slot, strip, gain, seeded]
                    {
                        strip->closeEditor();
                        ctx.expect (! strip->hasOpenEditor(), "the editor did not close");
                        slot.setParamValue (gain, kReopenGain);
                        ctx.later (200, [&ctx, strip, seeded]
                        {
                            if (! ctx.expect (strip->openEditor(), "the editor did not reopen"))
                            { ctx.complete (ctx.verdict()); return; }
                            ctx.waitUntil (seeded (kReopenGain), kSeedTimeoutMs,
                                [&ctx] { ctx.complete (ctx.verdict()); },
                                "the reopened editor was not seeded with the Gain set while it was closed");
                        });
                    }, "a Gain change made while the editor was open did not reach it");
            }, "the editor did not open showing the plug-in's live Gain");
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
};

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

    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("this display cannot embed plug-in editors");

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
        return ScenarioResult::fail ("the fixture did not load through the JUCE host: "
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
            ctx.complete (ScenarioResult::fail (
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
        steps->push_back ({ 200, [&ctx, strip]
        {
            ctx.expect (strip->openEditor(), "the sandboxed editor did not open");
        } });
        steps->push_back ({ 600, [&host, strip] { strip->closeEditor(); dismissAlert (host); },
                            [strip] { return strip->hasOpenEditor(); },
                            "the sandboxed editor never stayed open" });
        steps->push_back ({ 300, [&ctx, &slot, state]
        {
            ctx.expect (! slot.wasCrashed(), "the sandbox child crashed before unloading");
            ctx.expect (slot.isRemote(), "the slot stopped hosting its sandbox child before unloading");
            ctx.expect (slot.getRemoteChildPid() == state->childPid, "the sandbox child changed before unloading");
            ctx.expect (! oopstub::processGone (state->childPid), "the sandbox child exited before its editor closed");
            ctx.expect (slot.hideRemoteEditor(), "the sandbox child did not answer after its editor closed");
            slot.unload();
        }, [strip] { return ! strip->hasOpenEditor(); }, "the sandboxed editor never closed" });

        runSteps (ctx, steps, [&ctx, &host, strip, &slot, state]
        {
            // The child must be gone once the slot is, and the editor must have
            // let go before it: a surviving child would keep an editor window
            // alive over a slot that no longer exists.
            ctx.waitUntil ([state] { return oopstub::processGone (state->childPid); },
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

// The two inline slot labels MANUAL.md quotes, on a channel insert and on an
// aux lane slot. The sandbox stub answers the load RPC and no block command, so
// a block through the slot expires its deadline (stalled) and killing the child
// leaves it crashed - both through the code a real failure runs. The engine's
// own callback never reaches an insert on a silent, unarmed track, so the block
// is pushed straight at the slot, the way the headless kill case drives one.
constexpr const char* kStubHealthy = "\xe2\x96\xbe sandbox stub";
constexpr const char* kStubStalled = "! sandbox stub (stalled)";
constexpr const char* kStubCrashed = "! sandbox stub (crashed)";
constexpr int kHealthCrashTimeoutMs = 15000;

// True once the slot's watchdog has given up on the mute child.
bool stallSlot (PluginSlot& slot)
{
    oopstub::SlotMidiBuffer midi;
    std::array<float, ScenarioContext::kBlockSize> left {};
    std::array<float, ScenarioContext::kBlockSize> right {};
    slot.processStereoBlock (left.data(), right.data(), ScenarioContext::kBlockSize, midi);
    return slot.wasAutoBypassed();
}

void runAuxHealthLabelLeg (GuiHost& host, ScenarioContext& ctx)
{
    host.switchToStage (GuiHost::Stage::Aux);
    auto* lane = host.auxLane (kAuxLane);
    if (lane == nullptr)
    {
        ctx.complete (ScenarioResult::fail ("the Aux stage built no lane to drive"));
        return;
    }

    auto& auxSlot = ctx.engine().getAuxLaneStrip (kAuxLane).getPluginSlot (kAuxSlot);
    auto state = std::make_shared<SandboxState>();
    auxSlot.loadFromDescriptorAsync (oopstub::stubDescriptor(), [state] (bool ok, auto error)
    {
        state->loadOk = ok;
        state->loadError = error.toStdString();
        state->loadDone = true;
    });

    ctx.waitUntil ([state] { return state->loadDone; }, 30000,
    [&ctx, lane, &auxSlot, state]
    {
        if (! state->loadOk || ! auxSlot.isRemote())
        {
            ctx.complete (ScenarioResult::skip (
                "the aux slot never went out of process: " + state->loadError));
            return;
        }

        ctx.expect (stallSlot (auxSlot), "a block through the mute child did not stall the aux slot");
        lane->refreshSlot (kAuxSlot);
        ctx.expect (lane->slotLabel (kAuxSlot) == kStubStalled,
                    "the stalled aux slot reads \"" + lane->slotLabel (kAuxSlot) + "\"");

        auxSlot.clearAutoBypass();
        lane->refreshSlot (kAuxSlot);
        ctx.expect (lane->slotLabel (kAuxSlot) == "sandbox stub",
                    "the re-enabled aux slot reads \"" + lane->slotLabel (kAuxSlot) + "\"");

        auxSlot.unload();
        lane->refreshSlot (kAuxSlot);
        ctx.complete (ctx.verdict());
    },
    "the aux lane's sandboxed load never completed");
}

void runCrashedHealthLabelLeg (GuiHost& host, ScenarioContext& ctx, StripHandle* strip,
                               PluginSlot& slot)
{
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&ctx, strip]
    {
        strip->refreshInsertButton();
        ctx.expect (strip->insertLabel() == kStubCrashed,
                    "the crashed insert reads \"" + strip->insertLabel() + "\"");
    } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickInsert (kStripIndex, true), "the insert did not receive a right-click"); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickContextMenuItem ("Re-enable plugin (crashed)"),
                  "the crashed insert's menu offered no Re-enable entry"); } });
    steps->push_back ({ 300, [&ctx, strip, &slot]
    {
        ctx.expect (! slot.wasCrashed(), "Re-enable left the slot crashed");
        strip->refreshInsertButton();
        ctx.expect (strip->insertLabel() == kStubHealthy,
                    "the re-enabled insert reads \"" + strip->insertLabel() + "\"");
        slot.unload();
        strip->refreshInsertButton();
    } });
    runSteps (ctx, steps, [&host, &ctx] { runAuxHealthLabelLeg (host, ctx); });
}

std::optional<ScenarioResult> runSlotHealthLabels (GuiHost& host, ScenarioContext& ctx)
{
    const auto childBinary = oopstub::hostBinary();
    if (! childBinary)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped())
        return ScenarioResult::skip ("requires stopped transport");

    auto& manager = engine.getPluginManager();
    auto& slot    = engine.getChannelStrip (kStripIndex).getPluginSlot();
    auto& auxSlot = engine.getAuxLaneStrip (kAuxLane).getPluginSlot (kAuxSlot);
    if (slot.isLoaded() || auxSlot.isLoaded())
        return ScenarioResult::skip ("requires an empty channel insert and aux slot");

    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr)
        return ScenarioResult::skip ("the console has no strip to drive");

    const bool oopWasEnabled = manager.isOopEnabled();
    oopstub::useStub (manager, *childBinary, "--ipc-load-reply-stub");
    ctx.cleanup ([&ctx, &host, &slot, &auxSlot, oopWasEnabled]
    {
        auxSlot.clearAutoBypass();
        auxSlot.unload();
        if (auto* lane = host.auxLane (kAuxLane)) lane->refreshSlot (kAuxSlot);
        slot.clearAutoBypass();
        restoreInProcessHosting (ctx, slot, oopWasEnabled);
        drainModals (host);
        if (auto* channel = host.strip (kStripIndex)) channel->refreshInsertButton();
    });

    auto state = std::make_shared<SandboxState>();
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

        state->childPid = slot.getRemoteChildPid();
        ctx.note ("child pid: " + std::to_string (state->childPid));
        if (state->childPid <= 0)
        {
            ctx.complete (ScenarioResult::fail ("a remote slot reported no child pid"));
            return;
        }

        ctx.expect (stallSlot (slot), "a block through the mute child did not stall the slot");
        strip->refreshInsertButton();
        ctx.expect (strip->insertLabel() == kStubStalled,
                    "the stalled insert reads \"" + strip->insertLabel() + "\"");

        auto steps = std::make_shared<std::vector<Step>>();
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickInsert (kStripIndex, true), "the insert did not receive a right-click"); } });
        steps->push_back ({ 200, [&host, &ctx]
        { ctx.expect (host.clickContextMenuItem ("Re-enable plugin (auto-bypassed)"),
                      "the stalled insert's menu offered no Re-enable entry"); } });
        steps->push_back ({ 300, [&ctx, strip, &slot]
        {
            ctx.expect (! slot.wasAutoBypassed(), "Re-enable left the slot stalled");
            strip->refreshInsertButton();
            ctx.expect (strip->insertLabel() == kStubHealthy,
                        "the re-enabled insert reads \"" + strip->insertLabel() + "\"");
        } });
        runSteps (ctx, steps, [&ctx, &host, strip, &slot, state]
        {
            if (::kill (state->childPid, SIGKILL) != 0)
            {
                ctx.complete (ScenarioResult::fail ("could not kill the child process"));
                return;
            }
            ctx.waitUntil ([&slot] { return slot.wasCrashed(); }, kHealthCrashTimeoutMs,
                           [&ctx, &host, strip, &slot] { runCrashedHealthLabelLeg (host, ctx, strip, slot); },
                           "the slot never noticed its child had died");
        });
    },
    "the sandboxed load never completed");
    return std::nullopt;
}
#endif

// Sunset's editor follows a MIDI program change by replaying the preset into
// the unit. A new editor is built on every open, and one that replays the
// change it found already applied wipes every edit made since.
std::optional<ScenarioResult> runSunsetReopenKeepsEdit (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    return ScenarioResult::skip ("requires native UI");
   #endif
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the unit's editor can embed into");
    static constexpr std::uint8_t kProgram = 5;
    static constexpr float kPresetCutoffHz = 2000.0f;
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    auto& strip = engine.getChannelStrip (0);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (engine.getMidiInputDevices().empty()) return ScenarioResult::skip ("requires an enumerated MIDI input for injection");
    if (strip.getPluginSlot().isLoaded() || strip.isBuiltinLoaded() || strip.isNativeClapLoaded()
        || strip.isNativeLv2Loaded() || strip.isNativeVst3Loaded() || strip.isNativeAuLoaded()
        || strip.isNativeMultisampleLoaded() || strip.builtinReloadFailed())
        return ScenarioResult::skip ("requires an empty first insert");
    ctx.keep (track.mode);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiChannel);
    const auto mode = strip.insertMode.load();
    const auto stage = engine.getStage();
    ctx.cleanup ([&host, &engine, &strip, mode, stage]
    {
        drainModals (host);
        host.closeBuiltin (0);
        engine.suspendProcessing();
        strip.unloadBuiltin();
        strip.insertMode.store (mode);
        engine.resumeProcessing();
        if (auto* handle = host.strip (0)) handle->refreshInsertButton();
        host.switchToStage (guiStage (stage));
    });
    host.switchToStage (GuiHost::Stage::Mixing);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (0);
    track.midiChannel.store (0);
    std::string error;
    engine.suspendProcessing();
    const bool loaded = strip.loadBuiltin ("dusk.builtin.synth", error);
    if (loaded) strip.insertMode.store (ChannelStrip::kInsertPlugin);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load Sunset: " + error);
    if (! strip.getBuiltinSlot().hasPluginEditor()) return ScenarioResult::fail ("Sunset brought no editor of its own");
    int cutoff = -1;
    for (int i = 0; i < strip.getBuiltinSlot().paramCount(); ++i)
        if (const auto* info = strip.getBuiltinSlot().paramInfo (i);
            info != nullptr && info->id != nullptr && std::string ("filterCutoff") == info->id) cutoff = i;
    if (cutoff < 0) return ScenarioResult::fail ("Sunset has no filterCutoff parameter");
    if (auto* handle = host.strip (0)) handle->refreshInsertButton();

    const auto cutoffHz = [&strip, cutoff] { return strip.getBuiltinSlot().getParamValue (cutoff); };
    const auto editorUp = [&host] { auto* s = host.strip (0); return s != nullptr && s->hasOpenBuiltinEditor(); };
    auto edited = std::make_shared<float> (0.0f);
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&engine]
    {
        dusk::MidiBuffer midi;
        const std::uint8_t program[] = { 0xc0, kProgram };
        midi.addEvent (program, 2, 0);
        engine.stageTestMidiInjection (0, std::move (midi));
    } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickInsert (0, false), "insert button unavailable"); },
                       [cutoffHz] { return std::abs (cutoffHz() - kPresetCutoffHz) < 1.0f; },
                       "the program change did not load preset 5" });
    steps->push_back ({ 500, [&host, &ctx, cutoffHz, edited]
    {
        ctx.expect (host.builtinPointer (0, "filterCutoff", 0.25f, true)
                    && host.builtinPointer (0, "filterCutoff", 0.25f, false), "the editor took no filterCutoff edit");
        *edited = cutoffHz();
        ctx.expect (std::abs (*edited - kPresetCutoffHz) > 100.0f, "the edit did not move filterCutoff off the preset");
    }, editorUp, "the editor did not open" });
    steps->push_back ({ 500, [&host, &ctx, cutoffHz, edited]
    {
        ctx.expect (std::abs (cutoffHz() - *edited) < 1.0f, "the open editor did not keep the edit");
        host.closeBuiltin (0);
    } });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.clickInsert (0, false), "insert button unavailable for the reopen"); },
                       [editorUp] { return ! editorUp(); }, "the editor did not close" });
    steps->push_back ({ 0, [] {}, editorUp, "the editor did not reopen" });
    steps->push_back ({ 1000, [&ctx, cutoffHz, edited]
    {
        ctx.expect (std::abs (cutoffHz() - *edited) < 1.0f,
                    "reopening the editor put filterCutoff back to the preset ("
                        + std::to_string (cutoffHz()) + " Hz, edited to " + std::to_string (*edited) + " Hz)");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar sunsetReopenKeepsEdit { Scenario {
    "gui.sunset_reopen_keeps_edit", { "gui", "midi" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runSunsetReopenKeepsEdit (host, ctx); }
} };

// The manual dismisses a built-in unit's editor with Escape or a click outside
// it. On Linux the Escape goes through the display server, which the message
// loop reads only when it is otherwise idle, so the case fails if an editor that
// draws slower than its pump interval (Sunset's on a software renderer) is
// pumped without pacing. Elsewhere the key and the click go to the window's peer
// and prove only the dismiss paths themselves.
std::optional<ScenarioResult> runBuiltinEditorDismiss (GuiHost& host, ScenarioContext& ctx, const std::string& unitId)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    return ScenarioResult::skip ("requires native UI");
   #endif
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the unit's editor can embed into");
    auto& engine = ctx.engine();
    auto& strip = engine.getChannelStrip (0);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    if (strip.getPluginSlot().isLoaded() || strip.isBuiltinLoaded() || strip.isNativeClapLoaded()
        || strip.isNativeLv2Loaded() || strip.isNativeVst3Loaded() || strip.isNativeAuLoaded()
        || strip.isNativeMultisampleLoaded() || strip.builtinReloadFailed())
        return ScenarioResult::skip ("requires an empty first insert");
    const auto mode = strip.insertMode.load();
    const auto stage = engine.getStage();
    ctx.cleanup ([&host, &engine, &strip, mode, stage]
    {
        drainModals (host);
        host.closeBuiltin (0);
        engine.suspendProcessing();
        strip.unloadBuiltin();
        strip.insertMode.store (mode);
        engine.resumeProcessing();
        if (auto* handle = host.strip (0)) handle->refreshInsertButton();
        host.switchToStage (guiStage (stage));
    });
    host.switchToStage (GuiHost::Stage::Mixing);
    std::string error;
    engine.suspendProcessing();
    const bool loaded = strip.loadBuiltin (unitId, error);
    if (loaded) strip.insertMode.store (ChannelStrip::kInsertPlugin);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load " + unitId + ": " + error);
    if (! strip.getBuiltinSlot().hasPluginEditor()) return ScenarioResult::fail (unitId + " brought no editor of its own");
    if (auto* handle = host.strip (0)) handle->refreshInsertButton();

    const auto editorUp = [&host] { auto* s = host.strip (0); return s != nullptr && s->hasOpenBuiltinEditor(); };
    const auto editorDown = [editorUp] { return ! editorUp(); };
    // Long enough for the editor's pump to settle into its own pace, which is
    // when a pump that overruns has the message loop to itself.
    constexpr int kOpenForMs = 1500;
    constexpr int kDismissBoundMs = 5000;
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.clickInsert (0, false), "insert button unavailable"); } });
    steps->push_back ({ 0, [] {}, editorUp, "the editor did not open" });
    // Not a dialog: the keyboard stays with the window, so a shortcut no modal
    // would pass on still acts.
    steps->push_back ({ kOpenForMs, [&host, &ctx, editorUp]
    {
        auto& click = ctx.session().metronomeEnabled;
        const bool before = click.load();
        host.pressPeerKey (keyCodeDescription ('c'), 'c');
        ctx.expect (editorUp() && click.load() != before, "C at the window did not toggle the click with the editor open");
        click.store (before);
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
       #if defined (__linux__)
        ctx.expect (host.pressEscapeThroughDisplayServer(), "the X server took no Escape for the window");
       #else
        ctx.note ("no display server route for the key here; Escape went to the window's peer");
        ctx.expect (host.pressPeerKey ("escape"), "the window took no Escape");
       #endif
    } });
    steps->push_back ({ 0, [] {}, editorDown, "Escape at the window did not close " + unitId + "'s editor", {}, kDismissBoundMs });
    steps->push_back ({ 0, [&host, &ctx] { ctx.expect (host.clickInsert (0, false), "insert button unavailable for the reopen"); } });
    steps->push_back ({ 0, [] {}, editorUp, "the editor did not reopen" });
    steps->push_back ({ kOpenForMs, [&host, &ctx]
    { ctx.expect (host.clickBuiltinEditorDim (0), "no editor to click outside of"); } });
    steps->push_back ({ 0, [] {}, editorDown, "a click on the dim did not close " + unitId + "'s editor", {}, kDismissBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar sunsetEditorDismiss { Scenario {
    "gui.sunset_editor_dismiss", { "gui" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBuiltinEditorDismiss (host, ctx, "dusk.builtin.synth"); }
} };

const ScenarioRegistrar duskverbEditorDismiss { Scenario {
    "gui.duskverb_editor_dismiss", { "gui" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runBuiltinEditorDismiss (host, ctx, "dusk.builtin.reverb"); }
} };

// ------------------------------------------- aux built-in editor lifetime

// A unit that brings its own editor hands that editor the DSP instance, so the
// editor has to be gone before the instance is. This drives the three ways an
// aux slot loses the instance under an open editor - removed, replaced by the
// same unit, replaced by a different one - and ends with the lane empty and
// still drawing.
std::optional<ScenarioResult> runAuxBuiltinEditorLifetime (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr const char* kFirstUnit  = "dusk.builtin.delay";
    static constexpr const char* kSecondUnit = "dusk.builtin.reverb";
    static constexpr const char* kEmptySlot  = "Click to add an insert";

    if (builtin::findUnit (kFirstUnit) == nullptr || builtin::findUnit (kSecondUnit) == nullptr)
        return ScenarioResult::skip ("built without the units that bring their own editor");

    auto& engine = ctx.engine();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");

    auto& strip = engine.getAuxLaneStrip (kAuxLane);
    if (strip.isBuiltinLoaded (kAuxSlot) || strip.getPluginSlot (kAuxSlot).isLoaded()
        || strip.nativeInsertRestoreFailed (kAuxSlot))
        return ScenarioResult::skip ("requires an empty first aux insert");

    keepStage (host, ctx);
    ctx.keep (strip.insertMode[(std::size_t) kAuxSlot]);
    auto& laneParams = ctx.session().auxLane (kAuxLane);
    ctx.cleanup ([&host, &engine, &strip, &laneParams]
    {
        drainModals (host);
        // Whatever the run left, taken off the slot itself rather than through
        // the lane, so a case that ended early still leaves it empty.
        engine.suspendProcessing();
        strip.unloadBuiltin (kAuxSlot);
        engine.resumeProcessing();
        laneParams.builtinUnitId[(std::size_t) kAuxSlot].clear();
        laneParams.builtinStateBase64[(std::size_t) kAuxSlot].clear();
        if (auto* lane = host.auxLane (kAuxLane))
        {
            lane->refreshSlot (kAuxSlot);
            lane->rebuildSlots();
        }
    });

    host.switchToStage (GuiHost::Stage::Aux);
    auto* lane = host.auxLane (kAuxLane);
    if (lane == nullptr)
        return ScenarioResult::skip ("the aux stage realised no lane to drive");

    // Whether this display can carry the embedded editor at all. Decided by the
    // first leg; the later legs only assert the editor is up when it is.
    auto embeds = std::make_shared<bool> (false);
    const auto editorFor = [lane] (const char* unitId)
    { return lane->builtinEditorUnit (kAuxSlot) == unitId; };

    auto steps = std::make_shared<std::vector<Step>>();

    steps->push_back ({ 200, [&ctx, lane]
    {
        ctx.expect (lane->loadBuiltin (kAuxSlot, kFirstUnit),
                    "Tape Echo 2 did not load on the aux lane");
    } });

    // Replaced by the same unit: the successor can be handed the address the
    // old instance was freed from, so the editor cannot be left bound to it.
    steps->push_back ({ 1500, [&ctx, lane, embeds, editorFor]
    {
        *embeds = editorFor (kFirstUnit);
        if (! *embeds)
            ctx.note ("this display carried no embedded unit editor; the legs below "
                      "still prove the editor goes before the unit does");
        ctx.expect (lane->slotLabel (kAuxSlot) == "Tape Echo 2",
                    "the loaded slot reads \"" + lane->slotLabel (kAuxSlot) + "\"");

        // Read on the load's own stack: the old instance is freed inside this
        // call, so the editor that dereferences it has to be gone by the time
        // the call returns, not merely asked to close on a later pump.
        ctx.expect (lane->loadBuiltin (kAuxSlot, kFirstUnit),
                    "Tape Echo 2 did not replace itself on the aux lane");
        ctx.expect (lane->builtinEditorUnit (kAuxSlot).empty(),
                    "the editor was still up when its unit's DSP was replaced");
    } });

    // Replaced by a different unit, through the same seam a session restore and
    // a clone replay reach the slot by.
    steps->push_back ({ 1500, [&ctx, &strip, lane, embeds, editorFor]
    {
        ctx.expect (strip.isBuiltinLoaded (kAuxSlot),
                    "the same-unit replacement left the slot empty");
        ctx.expect (! *embeds || editorFor (kFirstUnit),
                    "the replacement's editor never came back");

        ctx.expect (lane->loadBuiltin (kAuxSlot, kSecondUnit),
                    "DuskVerb 2 did not replace Tape Echo 2 on the aux lane");
        ctx.expect (lane->builtinEditorUnit (kAuxSlot).empty(),
                    "the editor was still up when a different unit took the slot");
    } });

    // Removed: the lane's own remove path, one message-loop tick later.
    steps->push_back ({ 1500, [&ctx, lane, embeds, editorFor]
    {
        ctx.expect (! *embeds || editorFor (kSecondUnit),
                    "the replacing unit's editor never came up");
        ctx.expect (lane->slotLabel (kAuxSlot) == "DuskVerb 2",
                    "the replaced slot reads \"" + lane->slotLabel (kAuxSlot) + "\"");
        lane->unloadSlot (kAuxSlot);
    } });

    runSteps (ctx, steps, [&ctx, &strip, lane]
    {
        ctx.waitUntil ([&strip, lane]
        {
            return ! strip.isBuiltinLoaded (kAuxSlot)
                && lane->builtinEditorUnit (kAuxSlot).empty();
        }, 5000,
        [&ctx, lane]
        {
            // The lane is still a live component after all of that: it lays its
            // slot row out again and reports what the empty row now says.
            lane->rebuildSlots();
            lane->refreshSlot (kAuxSlot);
            ctx.expect (lane->slotLabel (kAuxSlot) == kEmptySlot,
                        "the lane's emptied slot reads \"" + lane->slotLabel (kAuxSlot) + "\"");
            ctx.complete (ctx.verdict());
        },
        "the aux lane never let go of the unit or its editor");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar auxBuiltinEditorLifetime { Scenario {
    "gui.aux_builtin_editor_lifetime", { "gui", "aux" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runAuxBuiltinEditorLifetime (host, ctx); }
} };

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

std::optional<ScenarioResult> runEditorTitleNamesStrip (GuiHost& host, ScenarioContext& ctx)
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
    const std::string opened = "Sunburst Gtr";
    const std::string renamed = "Ribbon Room";
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&session, opened] { session.track (0).name = opened.c_str(); } });
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickStripModule (0, 0, true, false), "EQ module label is unavailable"); } });
    steps->push_back ({ 500, [&host, &ctx, opened]
    {
        ctx.expect (host.stripModuleEditorOpen (0, 0), "EQ label did not open its editor");
        ctx.expect (host.stripModuleEditorTitle (0, 0) == opened,
                    "EQ editor is not titled with the strip name: " + host.stripModuleEditorTitle (0, 0));
    } });
    steps->push_back ({ 200, [&session, renamed] { session.track (0).name = renamed.c_str(); } });
    steps->push_back ({ 500, [&host, &ctx, renamed]
    {
        ctx.expect (host.stripModuleEditorOpen (0, 0), "the rename closed the EQ editor");
        ctx.expect (host.stripModuleEditorTitle (0, 0) == renamed,
                    "EQ editor title did not follow the rename: " + host.stripModuleEditorTitle (0, 0));
        host.closeStripModuleEditors (0);
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (! host.stripModuleEditorOpen (0, 0), "the EQ editor stayed open"); } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
}

const ScenarioRegistrar editorTitleNamesStrip { Scenario {
    "gui.editor_title_names_strip", { "gui", "strip" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runEditorTitleNamesStrip (host, ctx); }
} };

// The master tape is Tape Machine 2, and TAPE opens the plug-in's own editor over
// a dimmed window. Opening it leaves the stage as it was; Escape or a second
// click on TAPE dismisses it; the status light still engages the stage on its own.
std::optional<ScenarioResult> runMasterTapeEditor (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped()) return ScenarioResult::skip ("requires stopped transport");
    if (host.masterTapeEditorOpen()) return ScenarioResult::skip ("requires a closed tape editor");
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the native tape editor can embed into");
    const auto originalStage = engine.getStage();
    const bool originalTape = session.master().tapeEnabled.load();
    if (readyStrip (host, ctx) == nullptr) return ScenarioResult::fail ("channel strip is unavailable");
    ctx.keep (session.master().tape.bias);
    ctx.keep (session.master().tape.autoComp);
    ctx.cleanup ([&host, &session, originalStage, originalTape]
    {
        host.closeMasterTape();
        drainModals (host);
        session.master().tapeEnabled.store (originalTape);
        host.switchToStage (guiStage (originalStage));
    });
    session.master().tapeEnabled.store (false);
    const auto stripStage = engine.getStage();

    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickMasterTape (true), "the master TAPE label is unavailable"); } });
    steps->push_back ({ 1500, [&host, &ctx, &session, &engine, stripStage]
    {
        ctx.expect (host.masterTapeEditorOpen(), "TAPE did not open the tape editor");
        ctx.expect (engine.getStage() == stripStage, "opening the tape editor changed the stage");
        ctx.expect (host.masterTapeEditorDrawn(), "the tape editor never drew a frame");
        ctx.expect (! session.master().tapeEnabled.load(), "opening the editor engaged the tape");
        ctx.expect (host.pressPeerKey ("escape"), "the window did not handle Escape");
    } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (! host.masterTapeEditorOpen(), "Escape left the tape editor open");
        ctx.expect (host.clickMasterTape (true), "the master TAPE label is unavailable");
    } });
    steps->push_back ({ 1500, [&host, &ctx, &engine, stripStage]
    {
        ctx.expect (host.masterTapeEditorOpen(), "TAPE did not reopen the tape editor");
        ctx.expect (engine.getStage() == stripStage, "reopening the tape editor changed the stage");
        ctx.expect (host.clickMasterTape (true), "the master TAPE label is unavailable over its editor");
    } });
    steps->push_back ({ 600, [&host, &ctx]
    {
        ctx.expect (! host.masterTapeEditorOpen(), "a second click on TAPE left the editor open");
        ctx.expect (host.clickMasterTape (false), "the master TAPE status light is unavailable");
    } });
    steps->push_back ({ 300, [&host, &ctx, &session]
    {
        ctx.expect (session.master().tapeEnabled.load(), "the status light did not engage the tape");
        ctx.expect (! host.masterTapeEditorOpen(), "the status light opened the editor");
        session.master().tapeEnabled.store (false);
        ctx.expect (host.clickMasterTape (true), "the master TAPE label is unavailable");
    } });
    // Working a control in the editor engages the stage; its BYPASS switch is
    // the status light's switch, and turning it on leaves the stage out.
    steps->push_back ({ 1500, [&host, &ctx, &session]
    {
        auto& tape = session.master().tape;
        ctx.expect (host.masterTapeEditorOpen(), "TAPE did not open the tape editor again");
        ctx.expect (host.masterTapeEdit ("bias", 30.0f), "the tape editor took no Bias edit");
        ctx.expect (std::abs (tape.bias.load() - 30.0f) < 0.01f,
                    "the Bias edit left the session's bias at " + std::to_string (tape.bias.load()));
        ctx.expect (session.master().tapeEnabled.load(), "working Bias did not engage the tape");
        ctx.expect (host.masterTapeEdit ("daf_bypass", 1.0f), "the tape editor took no BYPASS edit");
        ctx.expect (! session.master().tapeEnabled.load(), "BYPASS in the editor left the tape engaged");
        ctx.expect (host.masterTapeEdit ("autoComp", 0.0f), "the tape editor took no Auto comp edit");
        ctx.expect (! tape.autoComp.load(), "the Auto comp edit left the session's Auto comp on");
        ctx.expect (session.master().tapeEnabled.load(), "working Auto comp did not engage the tape");
        ctx.expect (host.masterTapeEdit ("daf_bypass", 0.0f), "the tape editor took no second BYPASS edit");
        ctx.expect (session.master().tapeEnabled.load(), "BYPASS off in the editor did not engage the tape");
        host.closeMasterTape();
    } });
    // The editor goes over the next two ticks, not on the close call.
    runSteps (ctx, steps, [&host, &ctx]
    {
        ctx.waitUntil ([&host] { return ! host.masterTapeEditorOpen(); }, 3000,
                       [&ctx] { ctx.complete (ctx.verdict()); },
                       "closing left the tape editor open");
    });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar masterTapeEditor { Scenario {
    "gui.master_tape_editor", { "gui", "master" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runMasterTapeEditor (host, ctx); }
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
    steps->push_back ({ 0, [&ctx, &slot, component]
    {
        ctx.expect (slot.isLoadedStandardVst3(), "Add insert did not load the VST3 fixture");
        component->closeEditor();
    },
    [&slot, component] { return slot.isLoadedStandardVst3() && component->hasOpenEditor(); },
    "Add insert did not load the VST3 fixture and open its editor" });
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

const ScenarioRegistrar slotHealthLabels { Scenario {
    "gui.slot_health_labels",
    { "gui", "oop", "plugins" },
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
        return runSlotHealthLabels (host, ctx);
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

// The compressor editor, Utility's knob panel and Audio settings are native
// windows over the main one, and they keep keys the way any dialog does. A key
// that reaches the window, as every key does on Windows: M, C, T and ? do
// nothing behind the panel, L toggles the loop, Cmd+Z and Cmd+Shift+Z reach the
// history, and Escape closes the panel. A key the panel itself holds, as on
// Linux and macOS: Cmd+Z, Cmd+Shift+Z, Cmd+Y and Cmd+S reach the history and
// save the session, and Cmd+Shift+S opens Save As with the panel out of its
// way. Utility's panel steps aside for Save As the same way. The startup
// dialog lets nothing through: M, Space and Cmd+Z do nothing behind it. The
// virtual keyboard keeps Cmd+Z, whose letter is in its layout.
std::optional<ScenarioResult> runNativeDialogKeys (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& um = engine.getUndoManager();
    auto& dsp = engine.getChannelStrip (0);
    if (! transport.isStopped() || ! host.modalStackEmpty() || host.startupDialogOpen() || host.audioSettingsOpen())
        return ScenarioResult::skip ("requires a stopped transport and no dialog");
    if (dsp.getPluginSlot().isLoaded() || dsp.isBuiltinLoaded() || dsp.isNativeClapLoaded()
        || dsp.isNativeLv2Loaded() || dsp.isNativeVst3Loaded() || dsp.isNativeAuLoaded()
        || dsp.isNativeMultisampleLoaded() || dsp.builtinReloadFailed())
        return ScenarioResult::skip ("requires an empty first insert");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    const bool loop = transport.isLoopEnabled();
    const bool shown = host.setTimelineShown (true);
    ctx.keep (session.metronomeEnabled);
    keepStage (host, ctx);
    ctx.cleanup ([&host, &engine, &session, &transport, &dsp, originalDir, restore, loop, shown, mode = dsp.insertMode.load()]
    {
        host.closeVirtualKeyboard();
        host.closeStartupDialog();
        host.closeAudioSettings();
        host.closeBuiltin (0);
        if (auto* strip = host.strip (0)) strip->closeEditor();
        host.closeStripModuleEditors (0);
        drainModals (host);
        engine.suspendProcessing();
        dsp.unloadBuiltin();
        dsp.insertMode.store (mode);
        engine.resumeProcessing();
        if (auto* strip = host.strip (0)) strip->refreshInsertButton();
        reopenSavedSession (host, restore);
        applySessionDirectory (session, originalDir);
        transport.setLoopEnabled (loop);
        host.setTimelineShown (shown);
        engine.getUndoManager().clearUndoHistory();
    });
    const auto sessionJson = ctx.tempDir() / "Keys" / "session.json";
    std::error_code madeDir;
    std::filesystem::create_directories (sessionJson.parent_path(), madeDir);
    if (! SessionSerializer::save (session, sessionJson) || ! host.openSession (sessionJson))
        return ScenarioResult::fail ("could not open a saved session");
    host.switchToStage (GuiHost::Stage::Mixing);

    auto edits = std::make_shared<int> (0);
    const auto newEdit = [&um, edits]
    {
        um.beginNewTransaction ("Panel key probe");
        um.perform (new ParamEditAction ([edits] { ++*edits; }, [edits] { --*edits; }));
        return *edits;
    };
    auto steps = std::make_shared<std::vector<Step>>();
    // Keys at the window while `up` holds; `close` is what Escape there should do.
    const auto atWindow = [&host, &ctx, &session, &transport, steps, newEdit, edits]
                          (const std::string& where, std::function<bool()> up)
    {
        auto markers = std::make_shared<std::size_t>();
        auto before = std::make_shared<std::array<bool, 3>>();
        steps->push_back ({ 300, [&host, &ctx, &session, where, up, markers, before]
        {
            if (! ctx.expect (up(), where + " did not open")) return;
            *markers = session.getMarkers().size();
            *before = { session.metronomeEnabled.load(), host.timelineViewMatches (true), false };
            host.pressPeerKey (keyCodeDescription ('m'), 'm');
            host.pressPeerKey (keyCodeDescription ('c'), 'c');
            host.pressPeerKey ("T", 't');
            host.pressPeerKey ("shift + /", '?');
        } });
        steps->push_back ({ 300, [&host, &ctx, &session, &transport, where, up, markers, before, newEdit, edits]
        {
            ctx.expect (session.getMarkers().size() == *markers, "M at the window behind " + where + " dropped a marker");
            ctx.expect (session.metronomeEnabled.load() == (*before)[0], "C at the window behind " + where + " toggled the click");
            ctx.expect (host.timelineViewMatches (true) == (*before)[1], "T at the window behind " + where + " toggled the timeline");
            ctx.expect (! host.shortcutsOpen() && host.modalStackEmpty(),
                        "a key at the window behind " + where + " opened '" + host.modalText() + "'");
            if (! ctx.expect (up(), "the keys at the window closed " + where)) return;
            const bool loopWas = transport.isLoopEnabled();
            host.pressPeerKey (keyCodeDescription ('l'), 'l');
            ctx.expect (transport.isLoopEnabled() != loopWas, "L at the window behind " + where + " did not reach the loop");
            transport.setLoopEnabled (loopWas);
            const int start = newEdit();
            host.pressPeerKey ("command + Z");
            ctx.expect (*edits == start - 1, "Cmd+Z at the window behind " + where + " did not undo");
            host.pressPeerKey ("command + shift + Z");
            ctx.expect (*edits == start, "Cmd+Shift+Z at the window behind " + where + " did not redo");
        } });
    };

    // The compressor editor, with every key at the window.
    steps->push_back ({ 200, [&host, &ctx]
    { ctx.expect (host.clickStripModule (0, 1, true, false), "the first strip's compressor label is not on screen"); } });
    atWindow ("the compressor editor", [&host] { return host.stripModuleEditorOpen (0, 1); });
    steps->push_back ({ 100, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("escape"), "Escape at the window was not handled over the compressor editor"); } });
    steps->push_back ({ 300, [&host, &ctx, &engine, &dsp]
    {
        ctx.expect (! host.stripModuleEditorOpen (0, 1), "Escape at the window left the compressor editor open");
        std::string error;
        engine.suspendProcessing();
        const bool loaded = dsp.loadBuiltin ("dusk.builtin.utility", error);
        if (loaded) dsp.insertMode.store (ChannelStrip::kInsertPlugin);
        engine.resumeProcessing();
        if (! ctx.expect (loaded, "could not load Utility: " + error)) return;
        if (auto* strip = host.strip (0)) strip->refreshInsertButton();
        ctx.expect (host.clickInsert (0, false), "the first strip's insert is not on screen");
    } });

    // Utility's knob panel: every key at the window, then Save As.
    const auto utilityUp = [&host]
    {
        auto* strip = host.strip (0);
        return strip != nullptr && strip->hasOpenBuiltinEditor();
    };
    atWindow ("Utility's knob panel", utilityUp);
   #if defined (_WIN32)
    auto utilityHidden = std::make_shared<std::function<bool()>>();
    steps->push_back ({ 100, [&host, &ctx, utilityHidden]
    {
        if (auto* strip = host.strip (0)) *utilityHidden = strip->captureBuiltinEditorHiddenProbe();
        if (! ctx.expect ((bool) *utilityHidden, "Utility had no confirmed visible native child before Save As"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        host.pressPeerKey ("command + shift + S");
    } });
    steps->push_back ({ 400, [&host, &ctx, utilityHidden]
    {
        ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                    "Cmd+Shift+S over Utility's knob panel showed '" + host.modalText() + "' rather than Save As");
        ctx.expect (*utilityHidden && (*utilityHidden)(),
                    "Utility's confirmed native child was not hidden or destroyed over the Save As browser");
    } });
    steps->push_back ({ 0, [&host, &ctx]
    {
        drainModals (host);
        ctx.expect (host.openAudioSettings(), "Audio settings did not open");
    }, [&host]
    {
        const auto* strip = host.strip (0);
        return strip != nullptr && strip->builtinEditorDisposed();
    }, "Utility's editor or dimmer was not disposed after Save As", {}, 3000 });
   #else
    steps->push_back ({ 100, [&host] { host.pressPeerKey ("command + shift + S"); } });
    steps->push_back ({ 400, [&host, &ctx, utilityUp]
    {
        ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                    "Cmd+Shift+S over Utility's knob panel showed '" + host.modalText() + "' rather than Save As");
        ctx.expect (! utilityUp(), "Utility's knob panel stayed up over the Save As browser");
        drainModals (host);
        ctx.expect (host.openAudioSettings(), "Audio settings did not open");
    } });
   #endif

    // Audio settings: every key at the window, then the panel's own.
    atWindow ("Audio settings", [&host] { return host.audioSettingsOpen(); });
    auto start = std::make_shared<int> (0);
    steps->push_back ({ 100, [start, newEdit] { *start = newEdit(); } });
    struct Chord { const char* description; const char* name; int delta; };
    for (const auto chord : { Chord { "command + Z", "Cmd+Z", -1 }, Chord { "command + shift + Z", "Cmd+Shift+Z", 0 },
                              Chord { "command + Z", "Cmd+Z", -1 }, Chord { "command + Y", "Cmd+Y", 0 } })
    {
        steps->push_back ({ 100, [&host, &ctx, chord]
        { ctx.expect (host.inputAudioSettings (chord.description), "Audio settings did not take " + std::string (chord.name)); } });
        steps->push_back ({ 100, [&ctx, start, chord, edits]
        {
            ctx.expect (*edits == *start + chord.delta,
                        std::string (chord.name) + " in Audio settings did not reach the history");
        } });
    }
    auto saved = std::make_shared<std::filesystem::file_time_type>();
    steps->push_back ({ 100, [&host, &ctx, sessionJson, saved]
    {
        std::error_code error;
        *saved = std::filesystem::file_time_type::clock::now() - std::chrono::hours (1);
        std::filesystem::last_write_time (sessionJson, *saved, error);
        if (! ctx.expect (! error, "could not backdate the saved session")) return;
        ctx.expect (host.inputAudioSettings ("command + S"), "Audio settings did not take Cmd+S");
    } });
    steps->push_back ({ 100, [&host, &ctx, sessionJson, saved]
    {
        std::error_code error;
        ctx.expect (std::filesystem::last_write_time (sessionJson, error) > *saved && ! error,
                    "Cmd+S in Audio settings did not save the session in place");
        ctx.expect (host.inputAudioSettings ("command + shift + S"), "Audio settings did not take Cmd+Shift+S");
    } });
    steps->push_back ({ 400, [&host, &ctx]
    {
        ctx.expect (host.modalText().rfind ("Save session as...", 0) == 0,
                    "Cmd+Shift+S in Audio settings showed '" + host.modalText() + "' rather than Save As");
        ctx.expect (! host.audioSettingsOpen(), "Audio settings stayed up over the Save As browser");
        drainModals (host);
        ctx.expect (host.openAudioSettings(), "Audio settings did not open again");
    } });
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("escape"), "Escape at the window was not handled over Audio settings"); } });

    // The startup dialog: nothing.
    steps->push_back ({ 300, [&host, &ctx]
    {
        ctx.expect (! host.audioSettingsOpen(), "Escape at the window left Audio settings open");
        ctx.expect (host.openStartupDialog ({}, false), "the startup dialog did not open");
    } });
    steps->push_back ({ 400, [&host, &ctx, &session, &transport, newEdit, edits]
    {
        if (! ctx.expect (host.startupDialogOpen(), "the startup dialog is not up")) return;
        const auto markers = session.getMarkers().size();
        const int latest = newEdit();
        host.pressPeerKey (keyCodeDescription ('m'), 'm');
        host.pressPeerKey ("spacebar", ' ');
        host.pressPeerKey ("command + Z");
        ctx.expect (session.getMarkers().size() == markers && host.modalStackEmpty(),
                    "M behind the startup dialog dropped a marker or opened '" + host.modalText() + "'");
        ctx.expect (transport.isStopped(), "Space behind the startup dialog started the transport");
        ctx.expect (*edits == latest, "Cmd+Z behind the startup dialog undid an edit");
        ctx.expect (host.startupDialogOpen(), "the keys closed the startup dialog");
        host.closeStartupDialog();
    } });

    // The virtual keyboard plays its layout's letters with the command key held too.
    steps->push_back ({ 300, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("K", 'k'), "K was not handled"); } });
    steps->push_back ({ 600, [&host, &ctx, start, newEdit]
    {
        if (! ctx.expect (host.virtualKeyboardOpen(), "K did not open the virtual keyboard")) return;
        *start = newEdit();
        ctx.expect (host.inputVirtualKeyboard ("command + Z"), "the virtual keyboard did not take Cmd+Z");
    } });
    // Its layout keys at the window too, as Windows delivers every key and macOS
    // every command chord; L, outside the layout, still reaches the loop.
    steps->push_back ({ 300, [&host, &ctx, &session, &transport, start, edits]
    {
        ctx.expect (*edits == *start, "Cmd+Z on the virtual keyboard reached the history");
        const auto markers = session.getMarkers().size();
        host.pressPeerKey ("command + Z");
        host.pressPeerKey ("command + B");
        host.pressPeerKey (keyCodeDescription ('m'), 'm');
        ctx.expect (*edits == *start, "Cmd+Z at the window behind the virtual keyboard undid an edit");
        ctx.expect (host.modalStackEmpty(), "Cmd+B at the window behind the virtual keyboard opened '" + host.modalText() + "'");
        ctx.expect (session.getMarkers().size() == markers, "M at the window behind the virtual keyboard dropped a marker");
        ctx.expect (host.virtualKeyboardOpen(), "the layout keys at the window closed the virtual keyboard");
        const bool loopWas = transport.isLoopEnabled();
        host.pressPeerKey (keyCodeDescription ('l'), 'l');
        ctx.expect (transport.isLoopEnabled() != loopWas, "L at the window behind the virtual keyboard did not reach the loop");
        transport.setLoopEnabled (loopWas);
        host.closeVirtualKeyboard();
    } });
    steps->push_back ({ 0, [] {}, [&host] { return ! host.virtualKeyboardOpen(); }, "the virtual keyboard did not close" });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar nativeDialogKeys { Scenario {
    "gui.native_dialog_keys", { "gui", "keyboard", "plugin" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runNativeDialogKeys (host, ctx); }
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

// A unit's editor is a native child that paints over the tuner, so the tuner
// opening takes it down first: DuskVerb 2 drawn in the aux lane steps aside and
// comes back after, and Sunset's editor closes. The tuner is then the one taking
// the keys, a dialog's set, and Escape closes it rather than the editor.
std::optional<ScenarioResult> runTunerOverEditors (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_UI
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("requires the native UI");
   #else
    static constexpr const char* kStripUnit = "dusk.builtin.synth";
    static constexpr const char* kAuxUnit = "dusk.builtin.reverb";
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("requires a window the unit's editor can embed into");
    if (builtin::findUnit (kStripUnit) == nullptr || builtin::findUnit (kAuxUnit) == nullptr)
        return ScenarioResult::skip ("built without Sunset or DuskVerb 2");
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& strip = engine.getChannelStrip (0);
    auto& aux = engine.getAuxLaneStrip (kAuxLane);
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty() || host.tunerOpen())
        return ScenarioResult::skip ("requires a stopped transport, no modal and no tuner");
    if (strip.getPluginSlot().isLoaded() || strip.isBuiltinLoaded() || strip.isNativeClapLoaded()
        || strip.isNativeLv2Loaded() || strip.isNativeVst3Loaded() || strip.isNativeAuLoaded()
        || strip.isNativeMultisampleLoaded() || strip.builtinReloadFailed())
        return ScenarioResult::skip ("requires an empty first insert");
    if (aux.isBuiltinLoaded (kAuxSlot) || aux.getPluginSlot (kAuxSlot).isLoaded()
        || aux.nativeInsertRestoreFailed (kAuxSlot))
        return ScenarioResult::skip ("requires an empty first aux insert");

    keepStage (host, ctx);
    ctx.keep (session.metronomeEnabled);
    ctx.keep (aux.insertMode[(std::size_t) kAuxSlot]);
    auto& laneParams = session.auxLane (kAuxLane);
    ctx.cleanup ([&host, &engine, &session, &strip, &aux, &laneParams,
                  mode = strip.insertMode.load(), markers = session.getMarkers()]
    {
        if (host.tunerOpen()) host.pressPeerKey ("escape");
        drainModals (host);
        host.closeBuiltin (0);
        engine.suspendProcessing();
        strip.unloadBuiltin();
        strip.insertMode.store (mode);
        aux.unloadBuiltin (kAuxSlot);
        engine.resumeProcessing();
        if (auto* handle = host.strip (0)) handle->refreshInsertButton();
        laneParams.builtinUnitId[(std::size_t) kAuxSlot].clear();
        laneParams.builtinStateBase64[(std::size_t) kAuxSlot].clear();
        if (auto* lane = host.auxLane (kAuxLane))
        {
            lane->refreshSlot (kAuxSlot);
            lane->rebuildSlots();
        }
        session.getMarkers() = markers;
    });

    host.switchToStage (GuiHost::Stage::Aux);
    auto* lane = host.auxLane (kAuxLane);
    if (lane == nullptr)
        return ScenarioResult::skip ("the aux stage realised no lane to drive");
    std::string error;
    engine.suspendProcessing();
    const bool loaded = strip.loadBuiltin (kStripUnit, error);
    if (loaded) strip.insertMode.store (ChannelStrip::kInsertPlugin);
    engine.resumeProcessing();
    if (! loaded) return ScenarioResult::fail ("could not load Sunset: " + error);
    if (auto* handle = host.strip (0)) handle->refreshInsertButton();

    const auto plain = [&host] (char letter) { return host.pressPeerKey (keyCodeDescription (letter), letter); };
    const auto auxEditorUp = [lane] { return lane->builtinEditorUnit (kAuxSlot) == kAuxUnit; };
    const auto stripEditorUp = [&host] { auto* s = host.strip (0); return s != nullptr && s->hasOpenBuiltinEditor(); };
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&ctx, lane]
    { ctx.expect (lane->loadBuiltin (kAuxSlot, kAuxUnit), "DuskVerb 2 did not load on the aux lane"); } });
    steps->push_back ({ 0, [&host, &ctx, plain]
    {
        plain ('u');
        ctx.expect (host.tunerOpen(), "U did not open the tuner over the aux lane");
    }, auxEditorUp, "DuskVerb 2's editor never came up in the aux lane", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx]
    { ctx.expect (host.pressPeerKey ("escape"), "Escape over the tuner was not handled"); },
                       [auxEditorUp] { return ! auxEditorUp(); }, "DuskVerb 2's editor stayed up over the tuner",
                       {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx]
    {
        ctx.expect (! host.tunerOpen(), "Escape did not close the tuner over the aux lane");
        host.switchToStage (GuiHost::Stage::Mixing);
    }, auxEditorUp, "DuskVerb 2's editor did not come back after the tuner closed", {}, kSettleBoundMs });
    steps->push_back ({ 200, [&host, &ctx] { ctx.expect (host.clickInsert (0, false), "insert button unavailable"); } });
    steps->push_back ({ 800, [&host, &ctx, plain]
    {
        plain ('u');
        ctx.expect (host.tunerOpen(), "U did not open the tuner over Sunset's editor");
    }, stripEditorUp, "Sunset's editor did not open", {}, kSettleBoundMs });
    steps->push_back ({ 0, [&host, &ctx, &session, plain]
    {
        const bool clickWas = session.metronomeEnabled.load();
        const auto markers = session.getMarkers().size();
        plain ('c');
        plain ('m');
        ctx.expect (session.metronomeEnabled.load() == clickWas, "C behind the tuner toggled the click");
        ctx.expect (session.getMarkers().size() == markers && host.modalStackEmpty(), "M behind the tuner dropped a marker");
        ctx.expect (host.tunerOpen(), "the keys behind the tuner closed it");
        ctx.expect (host.pressPeerKey ("escape"), "Escape over the tuner was not handled");
        ctx.expect (! host.tunerOpen(), "Escape did not close the tuner over Sunset's editor");
    }, [stripEditorUp] { return ! stripEditorUp(); }, "U left Sunset's editor up over the tuner", {}, kSettleBoundMs });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar tunerOverEditors { Scenario {
    "gui.tuner_over_editors", { "gui", "keyboard", "plugin" }, Needs::Engine | Needs::Gui,
    {}, {}, 30000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runTunerOverEditors (host, ctx); }
} };

// With a plug-in's editor up and nothing in the window holding the keyboard, as
// macOS leaves it while the plug-in's own view is first responder, a Tab that
// reaches the window is left alone: the editor's body does not take the
// keyboard from the plug-in. With the body holding it, Tab stays in the editor.
std::optional<ScenarioResult> runPluginEditorTabLeftAlone (GuiHost& host, ScenarioContext& ctx)
{
   #if ! DUSKSTUDIO_HAS_NATIVE_CLAP || ! defined (__linux__)
    (void) host;
    (void) ctx;
    return ScenarioResult::skip ("needs the Linux CLAP fixture");
   #else
    if (! host.canEmbedPluginEditors())
        return ScenarioResult::skip ("this display cannot embed plug-in editors");
    const auto fixture = ctx.fixture ("no_window.clap");
    if (! fixture) return ScenarioResult::fail ("missing fixture: no_window.clap");
    auto* strip = readyStrip (host, ctx);
    if (strip == nullptr) return ScenarioResult::skip ("the console has no strip to drive");
    std::string error;
    if (! strip->loadNativeClap (*fixture, "studio.dusk.test.no-window", error))
        return ScenarioResult::fail ("the fixture did not load: " + error);
    strip->refreshInsertButton();
    auto giveBack = std::make_shared<std::function<void()>>();
    ctx.cleanup ([&host, strip, giveBack]
    {
        if (*giveBack) (*giveBack)();
        strip->closeEditor();
        dismissAlert (host);
        strip->unloadNativePlugins();
        strip->refreshInsertButton();
    });
    auto steps = std::make_shared<std::vector<Step>>();
    steps->push_back ({ 200, [&host, &ctx, strip, giveBack]
    {
        if (! ctx.expect (strip->openEditor(), "the CLAP fixture's editor did not open")) return;
        ctx.expect (host.modalHasKeyboardFocus(), "the plug-in editor's body did not take the keyboard");
        host.pressPeerKey ("tab");
        ctx.expect (host.modalHasKeyboardFocus() && strip->hasOpenEditor(), "Tab took the keyboard out of the plug-in editor");
        *giveBack = host.unfocusWindow();
        ctx.expect (! host.modalHasKeyboardFocus(), "the plug-in editor's body kept the keyboard");
        host.pressPeerKey ("tab");
        host.pressPeerKey ("shift + tab");
        ctx.expect (! host.modalHasKeyboardFocus(), "Tab with nothing focused took the keyboard for the plug-in editor's body");
        ctx.expect (strip->hasOpenEditor(), "Tab with nothing focused closed the plug-in editor");
    } });
    runSteps (ctx, steps, [&ctx] { ctx.complete (ctx.verdict()); });
    return std::nullopt;
   #endif
}

const ScenarioRegistrar pluginEditorTabLeftAlone { Scenario {
    "gui.plugin_editor_tab_left_alone", { "gui", "plugin", "keyboard" }, Needs::Engine | Needs::Gui,
    { "no_window.clap" }, {}, 10000,
    [] (GuiHost& host, ScenarioContext& ctx) { return runPluginEditorTabLeftAlone (host, ctx); }
} };
} // namespace
} // namespace duskstudio::scenario::gui
