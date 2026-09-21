#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "OopStubHarness.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (_WIN32) && ! defined (__APPLE__)
// The child is asked to exit, then taken down with SIGTERM and a half-second
// grace, so the wait only has to cover a loaded machine - not a hung child.
constexpr int kChildExitTimeoutMs = 5000;
constexpr int kLoadTimeoutMs      = 30000;

struct UnloadState
{
    std::unique_ptr<PluginManager> manager;
    std::unique_ptr<PluginSlot> slot;
    bool loadDone = false;
    bool loadOk = false;
    std::string loadError;
    // The child that is about to be dropped, and the one that replaces it.
    int deposedPid = -1;
    int replacementPid = -1;
};

void load (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state,
           std::function<void()> next, std::string whatFailed)
{
    state->loadDone = false;
    state->slot->loadFromDescriptorAsync (
        oopstub::stubDescriptor(),
        [state] (bool ok, auto error)
        {
            state->loadOk = ok;
            state->loadError = error.toStdString();
            state->loadDone = true;
        });
    ctx.waitUntil ([state] { return state->loadDone; }, kLoadTimeoutMs,
                   std::move (next), std::move (whatFailed));
}

// A loaded sandboxed slot reports the pid of the child it is driving, so the
// pid that is about to be orphaned can be captured before the slot lets go.
bool captureChildPid (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state,
                      int& pidOut)
{
    if (! state->loadOk || ! state->slot->isRemote())
    {
        ctx.complete (ScenarioResult::fail (
            "the slot never went out of process: " + state->loadError));
        return false;
    }
    pidOut = state->slot->getRemoteChildPid();
    ctx.note ("child pid: " + std::to_string (pidOut));
    if (pidOut <= 0)
    {
        ctx.complete (ScenarioResult::fail ("a remote slot reported no child pid"));
        return false;
    }
    return true;
}

void finish (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state)
{
    ctx.expect (state->replacementPid != state->deposedPid,
                "the replacement load reused the pid of the child it replaced");
    ctx.expect (state->slot->isRemote(),
                "the slot did not come back out of process after the replacement");
    ctx.complete (ctx.verdict());
}

// Second leg: loading over a loaded slot has to end the child it deposes, not
// leave it behind while the replacement runs alongside it.
void afterReplacementLoad (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state)
{
    if (! captureChildPid (ctx, state, state->replacementPid)) return;

    ctx.waitUntil ([state] { return oopstub::processGone (state->deposedPid); },
                   kChildExitTimeoutMs, [&ctx, state] { finish (ctx, state); },
                   "the replaced sandbox child outlived the load that deposed it");
}

void afterSecondLoad (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state)
{
    if (! captureChildPid (ctx, state, state->deposedPid)) return;

    load (ctx, state, [&ctx, state] { afterReplacementLoad (ctx, state); },
          "the replacement load never completed");
}

// First leg: unload() alone has to end the child. Retiring the connection
// into the slot's deferred-destruction ring is not enough, since nothing else
// would end the process until two more sandboxed loads pushed it out.
void afterUnload (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state)
{
    ctx.expect (! state->slot->isRemote(), "the slot still reports a live child");

    load (ctx, state, [&ctx, state] { afterSecondLoad (ctx, state); },
          "the load after the unload never completed");
}

void afterFirstLoad (ScenarioContext& ctx, const std::shared_ptr<UnloadState>& state)
{
    if (! captureChildPid (ctx, state, state->deposedPid)) return;

    state->slot->unload();
    ctx.waitUntil ([state] { return oopstub::processGone (state->deposedPid); },
                   kChildExitTimeoutMs, [&ctx, state] { afterUnload (ctx, state); },
                   "the sandbox child outlived the unload that dropped it");
}

std::optional<ScenarioResult> runUnloadReapsChild (ScenarioContext& ctx)
{
    const auto host = oopstub::hostBinary();
    if (! host)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto state = std::make_shared<UnloadState>();
    // The slot owns the load callback and that captures state, so nothing frees
    // the slot, the manager or the child unless the context does it on the way
    // out - including when a wait below times out.
    ctx.cleanup ([state] { state->slot.reset(); state->manager.reset(); });
    state->manager = std::make_unique<PluginManager>();
    // The only stub mode that answers the load RPC, so the only one that leaves
    // a slot genuinely out of process with a child to account for.
    oopstub::useStub (*state->manager, *host, "--ipc-load-reply-stub");

    state->slot = std::make_unique<PluginSlot>();
    state->slot->setManager (*state->manager);
    state->slot->prepareToPlay (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);

    load (ctx, state, [&ctx, state] { afterFirstLoad (ctx, state); },
          "the sandboxed load never completed");
    return std::nullopt;
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "oop.unload_reaps_child",
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
        return runUnloadReapsChild (ctx);
       #endif
    },
    120000
} };
} // namespace
} // namespace duskstudio::scenario
