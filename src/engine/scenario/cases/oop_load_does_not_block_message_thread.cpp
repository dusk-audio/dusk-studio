#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "OopStubHarness.h"

#include <chrono>
#include <memory>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS
constexpr int kTickMs = 50;
constexpr int kTicks  = 10;
// The ticks always arrive eventually; what a load running on the message thread
// would do is hold them back for its whole deadline, tens of seconds. So the
// check is when the last one lands, with room for a loop shared with the rest
// of the app.
constexpr long long kSlackMs = 1500;

struct ProbeState
{
    std::unique_ptr<PluginManager> manager;
    std::unique_ptr<PluginSlot> slot;
    int ticks = 0;
    bool loadCompleted = false;
    std::chrono::steady_clock::time_point startedAt {};
};

void finish (ScenarioContext& ctx, const std::shared_ptr<ProbeState>& state)
{
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                               std::chrono::steady_clock::now() - state->startedAt).count();
    ctx.note (std::to_string (state->ticks) + " message-thread ticks in "
              + std::to_string (elapsedMs) + " ms while the load was outstanding");

    ctx.expect (! state->loadCompleted,
                "the stalling child answered, so the load was never outstanding");
    ctx.expect (elapsedMs <= (long long) kTicks * kTickMs + kSlackMs,
                "the message thread took " + std::to_string (elapsedMs)
                    + " ms to run " + std::to_string (kTicks)
                    + " ticks while a load was outstanding");
    ctx.complete (ctx.verdict());
}

void tick (ScenarioContext& ctx, std::shared_ptr<ProbeState> state)
{
    ++state->ticks;
    if (state->ticks >= kTicks)
    {
        finish (ctx, state);
        return;
    }
    ctx.later (kTickMs, [&ctx, state] { tick (ctx, state); });
}

std::optional<ScenarioResult> runProbe (ScenarioContext& ctx)
{
    const auto host = oopstub::hostBinary();
    if (! host)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto state = std::make_shared<ProbeState>();
    ctx.cleanup ([state] { state->slot.reset(); state->manager.reset(); });
    state->manager = std::make_unique<PluginManager>();
    // A child that completes the handshake and then never answers the load RPC:
    // the load's own deadline is tens of seconds, so a load that ran on the
    // message thread would freeze the UI for that long.
    oopstub::useStub (*state->manager, *host, "--ipc-stub-timeout");

    state->slot = std::make_unique<PluginSlot>();
    state->slot->setManager (*state->manager);
    state->slot->prepareToPlay (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    state->slot->loadFromDescriptorAsync (oopstub::stubDescriptor(),
                                          [state] (bool, auto) { state->loadCompleted = true; });

    if (state->loadCompleted)
        return ScenarioResult::fail ("the load reported completion from inside the call");
    if (state->slot->isRemote())
        return ScenarioResult::fail ("the slot went remote from inside the load call");

    state->startedAt = std::chrono::steady_clock::now();
    ctx.later (kTickMs, [&ctx, state] { tick (ctx, state); });
    return std::nullopt;
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "oop.load_does_not_block_message_thread",
    { "oop", "plugin" },
    Needs::Engine | Needs::Oop,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return runProbe (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
