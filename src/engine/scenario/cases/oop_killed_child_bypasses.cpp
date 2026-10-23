#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "OopStubHarness.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/Json.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>

#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (_WIN32)
 #include <csignal>
#endif

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (_WIN32) && ! defined (__APPLE__)
constexpr int kCrashNoticeTimeoutMs = 10000;

struct KillState
{
    std::unique_ptr<PluginManager> manager;
    std::unique_ptr<PluginSlot> slot;
    oopstub::SlotMidiBuffer midi;
    std::array<float, ScenarioContext::kBlockSize> left {};
    std::array<float, ScenarioContext::kBlockSize> right {};
    bool loadDone = false;
    bool loadOk = false;
    std::string loadError;
    std::chrono::steady_clock::time_point killedAt {};
};

void finish (ScenarioContext& ctx, const std::shared_ptr<KillState>& state)
{
    const auto noticeMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                              std::chrono::steady_clock::now() - state->killedAt).count();
    ctx.note ("the killed child was noticed after " + std::to_string (noticeMs) + " ms");

    state->left.fill (0.25f);
    state->right.fill (-0.5f);
    for (int b = 0; b < 4; ++b)
        state->slot->processStereoBlock (state->left.data(), state->right.data(),
                                         ScenarioContext::kBlockSize, state->midi);

    ctx.expect (state->left.front() > 0.2f && state->right.front() < -0.4f,
                "the bypassed slot did not pass the dry signal through");
    ctx.expect (! state->slot->isRemote(),
                "the slot still reports a live child after the kill");
    ctx.complete (ctx.verdict());
}

void afterLoad (ScenarioContext& ctx, const std::shared_ptr<KillState>& state)
{
    if (! state->loadOk || ! state->slot->isRemote())
    {
        ctx.complete (ScenarioResult::fail (
            "the slot never went out of process: " + state->loadError));
        return;
    }

    const int pid = state->slot->getRemoteChildPid();
    ctx.note ("child pid: " + std::to_string (pid));
    if (pid <= 0)
    {
        ctx.complete (ScenarioResult::fail ("a remote slot reported no child pid"));
        return;
    }

    state->killedAt = std::chrono::steady_clock::now();
    if (::kill (pid, SIGKILL) != 0)
    {
        ctx.complete (ScenarioResult::fail ("could not kill the child process"));
        return;
    }

    // wasCrashed is raised by the message-thread reaper, not by the audio path,
    // so the wait has to let the loop run rather than push blocks through.
    ctx.waitUntil ([state] { return state->slot->wasCrashed(); }, kCrashNoticeTimeoutMs,
                   [&ctx, state] { finish (ctx, state); },
                   "the slot never noticed its child had died");
}

std::optional<ScenarioResult> runKilledChild (ScenarioContext& ctx)
{
    const auto host = oopstub::hostBinary();
    if (! host)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto state = std::make_shared<KillState>();
    // The slot owns the load callback and that captures state, so nothing frees
    // the slot, the manager or the child unless the context does it on the way
    // out - including when a wait below times out.
    ctx.cleanup ([state] { state->slot.reset(); state->manager.reset(); });
    state->manager = std::make_unique<PluginManager>();
    // The only stub mode that answers the load RPC, so the only one that leaves
    // a slot genuinely out of process with a child to kill.
    oopstub::useStub (*state->manager, *host, "--ipc-load-reply-stub");

    state->slot = std::make_unique<PluginSlot>();
    state->slot->setManager (*state->manager);
    state->slot->prepareToPlay (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    state->slot->loadFromDescriptorAsync (
        oopstub::stubDescriptor(),
        [state] (bool ok, auto error)
        {
            state->loadOk = ok;
            state->loadError = error.toStdString();
            state->loadDone = true;
        });

    ctx.waitUntil ([state] { return state->loadDone; }, 30000,
                   [&ctx, state] { afterLoad (ctx, state); },
                   "the sandboxed load never completed");
    return std::nullopt;
}
#endif

#if DUSKSTUDIO_HAS_OOP_PLUGINS
struct ReopenState
{
    PluginDescriptor descriptor;
    oopstub::SlotMidiBuffer midi;
    std::array<float, ScenarioContext::kBlockSize> left {}, right {};
    std::string cachedState;
    std::string error;
    int gainIndex = -1;
    int terminateIndex = -1;
    bool loaded = false;
    bool loadDone = false;
};

PluginSlot& reopenSlot (ScenarioContext& ctx)
{
    return ctx.engine().getChannelStrip (0).getPluginSlot();
}

bool carriesGain (ScenarioContext& ctx, ReopenState& state, float gain)
{
    auto& slot = reopenSlot (ctx);
    state.left.fill (0.8f);
    state.right.fill (-0.4f);
    slot.processStereoBlock (state.left.data(), state.right.data(),
                             ScenarioContext::kBlockSize, state.midi);
    const auto matches = [gain] (float value, float input)
    { return std::isfinite (value) && std::abs (value - input * gain) <= 1.0e-6f; };
    return std::all_of (state.left.begin(), state.left.end(),
                         [&] (float value) { return matches (value, 0.8f); })
        && std::all_of (state.right.begin(), state.right.end(),
                        [&] (float value) { return matches (value, -0.4f); });
}

void reopenAfterCrash (ScenarioContext& ctx, const std::shared_ptr<ReopenState>& state)
{
    auto& engine = ctx.engine();
    auto& slot = reopenSlot (ctx);
    ctx.expect (slot.wasAutoBypassed() && ! slot.isRemote(),
                "the crashed child did not leave the slot auto-bypassed and disconnected");
    ctx.expect (carriesGain (ctx, *state, 1.0f), "the crashed slot changed the dry audio");
    slot.clearAutoBypass();
    ctx.expect (! slot.wasCrashed() && ! slot.wasAutoBypassed()
                    && ! slot.isRemote() && ! slot.isLoaded(),
                "Re-enable did not clear the crash without starting another child");
    const auto descriptor = slot.getDescriptorForSave (0);
    ctx.expect (descriptor.has_value() && *descriptor == state->descriptor,
                "Re-enable lost the plugin identity");
    ctx.expect (slot.getStateBase64ForSave (0).toStdString() == state->cachedState,
                "Re-enable lost the nondefault cached state");
    engine.publishPluginStateForSave (true);
    const auto path = ctx.sessionDir() / "crashed-session.json";
    if (! ctx.expect (SessionSerializer::save (ctx.session(), path), "the crashed session did not save"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    std::ifstream input (path);
    const auto saved = dusk::json::Json::parse (input, nullptr, false);
    const auto& tracks = dusk::json::array (saved, "tracks");
    if (! ctx.expect (tracks.size() == Session::kNumTracks, "the saved track inventory is incomplete"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    const dusk::json::Json expectedDescriptor = state->descriptor.toJsonObject();
    ctx.expect (dusk::json::child (tracks[0], "plugin_descriptor") == expectedDescriptor,
                "the saved JSON changed the plugin identity");
    ctx.expect (dusk::json::getString (tracks[0], "plugin_state") == state->cachedState,
                "the saved JSON lost the exact nondefault plugin state");
    ctx.expect (! dusk::json::getBool (tracks[0], "insert_bypassed", false),
                "the crash bypass was saved as a user bypass");
    slot.unload();
    ctx.expect (slot.getStateBase64ForSave (0).isEmpty(), "explicit unload retained the cached state");
    if (! ctx.expect (SessionSerializer::load (ctx.session(), path), "the crashed session did not reopen"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    engine.consumePluginStateAfterLoad();
    ctx.expect (engine.getLastPluginLoadFailures().empty(), "reopening reported a plugin load failure");
    ctx.expect (slot.isRemote() && slot.isLoaded() && ! slot.isAsyncLoadPending()
                    && ! slot.wasCrashed() && ! slot.wasAutoBypassed(),
                "reopening did not launch a healthy sandboxed instance");
    ctx.expect (carriesGain (ctx, *state, 0.375f),
                "the reopened child did not restore gain: expected L=0.3, R=-0.15 across all 256 frames");
    ctx.complete (ctx.verdict());
}

void cacheThenCrash (ScenarioContext& ctx, const std::shared_ptr<ReopenState>& state)
{
    auto& slot = reopenSlot (ctx);
    ctx.engine().publishPluginStateForSave (true);
    state->cachedState = ctx.session().track (0).pluginStateBase64.toStdString();
    if (! ctx.expect (! state->cachedState.empty(), "the nondefault state was not cached")
        || ! ctx.expect (slot.isRemote(), "the fixture must be remote before its termination is armed"))
    {
        ctx.complete (ctx.verdict());
        return;
    }
    slot.setParamNormalised (state->terminateIndex, 1.0f);
    ctx.waitUntil ([&ctx, state]
                   {
                       carriesGain (ctx, *state, 0.375f);
                       return reopenSlot (ctx).wasCrashed();
                   }, 10000, [&ctx, state] { reopenAfterCrash (ctx, state); },
                   "the real fixture child never terminated and reached the crash reaper");
}

std::optional<ScenarioResult> runCrashReopen (ScenarioContext& ctx)
{
    if (! oopstub::hostBinary())
        return ScenarioResult::fail ("the sandbox host binary is not beside the app");
    auto& engine = ctx.engine();
    auto& manager = engine.getPluginManager();
    auto& slot = reopenSlot (ctx);
    using SessionFile = std::decay_t<decltype (ctx.session().getSessionDirectory())>;
    using SessionString = std::decay_t<decltype (ctx.session().getSessionDirectory().getFullPathName())>;
    SessionString error;
    auto instance = manager.createPluginInstance (
        SessionFile (ctx.fixture ("crash_state.vst3")->u8string().c_str()),
        ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error);
    if (! instance) return ScenarioResult::fail ("the real fixture did not load: " + error.toStdString());
    auto state = std::make_shared<ReopenState>();
    state->descriptor = manager.descriptorForInstance (*instance);
    if (std::filesystem::u8path (state->descriptor.location).extension() != ".vst3")
        state->descriptor.location = ctx.fixture ("crash_state.vst3")->u8string();
    const auto& parameters = instance->getParameters();
    for (int index = 0; index < parameters.size(); ++index)
    {
        const auto name = parameters[index]->getName (100).toStdString();
        if (name == "State Gain") state->gainIndex = index;
        if (name == "Terminate Child") state->terminateIndex = index;
    }
    instance.reset();
    if (state->descriptor.backend != PluginBackend::JuceLegacy
        || state->gainIndex < 0 || state->terminateIndex < 0)
        return ScenarioResult::fail ("the legacy fixture lacks State Gain or Terminate Child");
    const bool wasOop = manager.isOopEnabled();
    ctx.cleanup ([&manager, &slot, wasOop] { slot.unload(); manager.setOopEnabled (wasOop); });
    ctx.keep (ctx.session().track (0).strip.insertBypassed);
    ctx.session().track (0).strip.insertBypassed.store (false);
    manager.setOopEnabled (true);
    slot.loadFromDescriptorAsync (state->descriptor, [state] (bool ok, auto loadError)
    {
        state->loaded = ok;
        state->error = loadError.toStdString();
        state->loadDone = true;
    });
    ctx.waitUntil ([state] { return state->loadDone; }, 30000, [&ctx, state]
    {
        auto& loadedSlot = reopenSlot (ctx);
        if (! ctx.expect (state->loaded && loadedSlot.isRemote(), "the real fixture was not sandboxed: " + state->error)
            || ! ctx.expect (carriesGain (ctx, *state, 1.0f), "the fresh child did not pass unity audio"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.engine().publishPluginStateForSave (true);
        const auto unityState = ctx.session().track (0).pluginStateBase64.toStdString();
        loadedSlot.setParamNormalised (state->gainIndex, 0.375f);
        ctx.waitUntil ([&ctx, state] { return carriesGain (ctx, *state, 0.375f); }, 5000,
                       [&ctx, state, unityState]
                       {
                           ctx.engine().publishPluginStateForSave (true);
                           ctx.expect (ctx.session().track (0).pluginStateBase64.toStdString() != unityState,
                                       "the saved blob did not distinguish nondefault gain from unity");
                           cacheThenCrash (ctx, state);
                       }, "the gain parameter never reached the real sandboxed audio path");
    }, "the real fixture load never completed");
    return std::nullopt;
}
#endif

const ScenarioRegistrar reopenRegistrar { Scenario {
    "oop.crashed_session_reopens_state", { "oop", "plugin", "session" }, Needs::Engine | Needs::Oop,
   #if DUSKSTUDIO_HAS_OOP_PLUGINS
    { "crash_state.vst3" },
   #else
    {},
   #endif
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return runCrashReopen (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #endif
    }, 60000
} };

const ScenarioRegistrar registrar { Scenario {
    "oop.killed_child_bypasses",
    { "oop", "plugin" },
    Needs::Engine | Needs::Oop,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_OOP_PLUGINS
        (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #elif defined (_WIN32)
        (void) ctx;
        return ScenarioResult::skip ("Windows tracks the child by handle, not by pid");
       #elif defined (__APPLE__)
        (void) ctx;
        return ScenarioResult::skip ("the sandbox stub children are not exercised on macOS");
       #else
        return runKilledChild (ctx);
       #endif
    },
    60000
} };
} // namespace
} // namespace duskstudio::scenario
