#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "OopStubHarness.h"

#include <array>
#include <chrono>
#include <memory>
#include <string>

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

    std::string failure;
    state->left.fill (0.25f);
    state->right.fill (-0.5f);
    for (int b = 0; b < 4; ++b)
        state->slot->processStereoBlock (state->left.data(), state->right.data(),
                                         ScenarioContext::kBlockSize, state->midi);

    if (! (state->left.front() > 0.2f && state->right.front() < -0.4f))
        failure = "the bypassed slot did not pass the dry signal through";
    else if (state->slot->isRemote())
        failure = "the slot still reports a live child after the kill";

    state->slot.reset();
    state->manager.reset();
    ctx.complete (failure.empty() ? ScenarioResult::pass() : ScenarioResult::fail (failure));
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
