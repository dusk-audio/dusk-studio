#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "OopStubHarness.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_OOP_PLUGINS && ! defined (__APPLE__)
constexpr int kCycles = 50;
constexpr int kBlocksPerCycle = 2;
// Short enough that the load is still in flight when the unload lands, so the
// odd cycles are the cancel-mid-load race.
constexpr int kCycleGapMs = 2;
constexpr int kCycleLoadTimeoutMs = 15000;

struct SwitchingState
{
    std::unique_ptr<PluginManager> manager;
    std::unique_ptr<PluginSlot> slot;
    oopstub::SlotMidiBuffer midi;
    std::array<float, ScenarioContext::kBlockSize> left {};
    std::array<float, ScenarioContext::kBlockSize> right {};
    int cycle = 0;
    int cyclesRemoteAtUnload = 0;
    int doneForCycle = 0;
    bool finalLoadDone = false;
    bool finalLoadOk = false;
    std::string finalLoadError;
};

void driveBlocks (SwitchingState& state, int blocks)
{
    for (int b = 0; b < blocks; ++b)
    {
        state.left.fill (0.25f);
        state.right.fill (-0.5f);
        state.slot->processStereoBlock (state.left.data(), state.right.data(),
                                        ScenarioContext::kBlockSize, state.midi);
    }
}

void finish (ScenarioContext& ctx, const std::shared_ptr<SwitchingState>& state)
{
    ctx.expect (state->finalLoadOk, "the load after the switching burst failed: " + state->finalLoadError);
    ctx.expect (state->slot->isLoaded(), "the slot did not come back loaded");
    ctx.expect (state->slot->isRemote(), "the slot did not come back out of process");
    ctx.expect (! state->slot->wasCrashed(), "a child was reported crashed after the burst");

    driveBlocks (*state, 2);
    ctx.expect (state->left.front() > 0.2f && state->right.front() < -0.4f,
            "audio through the recovered slot was not the dry signal");

    ctx.note ("cycles run: " + std::to_string (state->cycle)
              + ", out of process when unloaded: "
              + std::to_string (state->cyclesRemoteAtUnload));
    ctx.expect (state->cyclesRemoteAtUnload >= kCycles / 4,
            "too few cycles in the burst reached the sandboxed path");

    state->slot.reset();
    state->manager.reset();

    ctx.complete (ctx.verdict());
}

void step (ScenarioContext& ctx, std::shared_ptr<SwitchingState> state)
{
    if (state->cycle >= kCycles)
    {
        state->slot->loadFromDescriptorAsync (
            oopstub::stubDescriptor(),
            [state] (bool ok, auto error)
            {
                state->finalLoadOk = ok;
                state->finalLoadError = error.toStdString();
                state->finalLoadDone = true;
            });
        ctx.waitUntil ([state] { return state->finalLoadDone; }, 30000,
                       [&ctx, state] { finish (ctx, state); },
                       "the load after the switching burst never completed");
        return;
    }

    const int cycle = ++state->cycle;
    // A cancelled cycle's completion can land after a later cycle's, so the
    // marker only ever moves forward.
    state->slot->loadFromDescriptorAsync (
        oopstub::stubDescriptor(),
        [state, cycle] (bool, auto)
        { state->doneForCycle = std::max (state->doneForCycle, cycle); });

    auto closeCycle = [&ctx, state]
    {
        driveBlocks (*state, kBlocksPerCycle);
        if (state->slot->isRemote()) ++state->cyclesRemoteAtUnload;
        state->slot->unload();
        step (ctx, state);
    };

    // Alternate the two races the user can drive: replacing a plugin that has
    // finished loading, and replacing one whose child is still being built.
    if (cycle % 2 == 0)
        ctx.waitUntil ([state, cycle] { return state->doneForCycle == cycle; },
                       kCycleLoadTimeoutMs, closeCycle,
                       "a load in the switching burst never completed");
    else
        ctx.later (kCycleGapMs, closeCycle);
}

std::optional<ScenarioResult> runRapidSwitching (ScenarioContext& ctx)
{
    const auto host = oopstub::hostBinary();
    if (! host)
        return ScenarioResult::skip ("the sandbox host binary is not beside the app");

    auto state = std::make_shared<SwitchingState>();
    state->manager = std::make_unique<PluginManager>();
    // --ipc-stub answers audio blocks but never the load RPC, so the only mode
    // that puts a slot out of process is the one that answers control messages.
    // It does not service audio blocks, so a block through the recovered slot
    // exercises the timeout -> dry-passthrough fallback rather than a round trip.
    oopstub::useStub (*state->manager, *host, "--ipc-load-reply-stub");

    state->slot = std::make_unique<PluginSlot>();
    state->slot->setManager (*state->manager);
    state->slot->prepareToPlay (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);

    step (ctx, state);
    return std::nullopt;
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "oop.rapid_switching_safe",
    { "oop", "plugin", "slow" },
    Needs::Engine | Needs::Oop,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if ! DUSKSTUDIO_HAS_OOP_PLUGINS
        (void) ctx;
        return ScenarioResult::skip ("built without sandboxed plugin hosting");
       #elif defined (__APPLE__)
        (void) ctx;
        return ScenarioResult::skip ("the sandbox stub children are not exercised on macOS");
       #else
        return runRapidSwitching (ctx);
       #endif
    },
    180000
} };
} // namespace
} // namespace duskstudio::scenario
