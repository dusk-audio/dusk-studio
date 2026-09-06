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
// Long enough for the fork/exec and the ready handshake, so the worker really is
// parked in the reply wait the destructor has to unwind.
constexpr int kSettleMs = 300;
constexpr long long kDestroyBudgetMs = 2000;

struct QuitState
{
    std::unique_ptr<PluginManager> manager;
    std::unique_ptr<PluginSlot> slot;
    bool loadCompleted = false;
};

std::optional<ScenarioResult> runQuitDuringLoad (ScenarioContext& ctx)
{
    const auto host = oopstub::hostBinary();
    if (! host)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto state = std::make_shared<QuitState>();
    ctx.cleanup ([state] { state->slot.reset(); state->manager.reset(); });
    state->manager = std::make_unique<PluginManager>();
    // --ipc-stub completes the handshake and then never reads the control
    // channel, so the load RPC sits in its reply wait for its full deadline.
    oopstub::useStub (*state->manager, *host, "--ipc-stub");

    state->slot = std::make_unique<PluginSlot>();
    state->slot->setManager (*state->manager);
    state->slot->prepareToPlay (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    state->slot->loadFromDescriptorAsync (oopstub::stubDescriptor(),
                                          [state] (bool, auto) { state->loadCompleted = true; });

    ctx.later (kSettleMs, [&ctx, state]
    {
        if (state->loadCompleted)
        {
            ctx.complete (ScenarioResult::fail (
                "the stalling child answered the load, so nothing was interrupted"));
            return;
        }

        const auto start = std::chrono::steady_clock::now();
        state->slot.reset();
        const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                                std::chrono::steady_clock::now() - start).count();

        ctx.note ("destroying the slot mid-load took " + std::to_string (tookMs) + " ms");
        if (tookMs >= kDestroyBudgetMs)
        {
            ctx.complete (ScenarioResult::fail (
                "destroying a slot mid-load took " + std::to_string (tookMs) + " ms"));
            return;
        }
        ctx.complete (ScenarioResult::pass());
    });

    return std::nullopt;
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "oop.quit_during_load_bounded",
    { "oop", "plugin" },
    Needs::Engine | Needs::Oop,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_OOP_PLUGINS
        return runQuitDuringLoad (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
