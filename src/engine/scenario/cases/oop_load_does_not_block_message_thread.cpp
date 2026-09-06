#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "OopStubHarness.h"

#include <memory>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS
constexpr int kTickMs = 50;
constexpr int kTicks  = 10;
// The loop is shared with everything else the app is doing, so allow a couple of
// late ticks before calling it stalled.
constexpr int kMinTicks = 8;

struct ProbeState
{
    std::unique_ptr<PluginManager> manager;
    std::unique_ptr<PluginSlot> slot;
    int ticks = 0;
    bool loadCompleted = false;
};

void finish (ScenarioContext& ctx, const std::shared_ptr<ProbeState>& state)
{
    std::string failure;
    ctx.note ("message-thread ticks during the stalled load: " + std::to_string (state->ticks)
              + " of " + std::to_string (kTicks));

    if (state->loadCompleted)
        failure = "the stalling child answered, so the load was never outstanding";
    else if (state->ticks < kMinTicks)
        failure = "the message thread only ran " + std::to_string (state->ticks)
                + " times while a load was outstanding";

    state->slot.reset();
    state->manager.reset();
    ctx.complete (failure.empty() ? ScenarioResult::pass() : ScenarioResult::fail (failure));
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
