#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../PluginManager.h"

#include <atomic>
#include <chrono>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
constexpr long long kReturnBudgetMs = 1500;

ScenarioResult runCancel (ScenarioContext& ctx)
{
    auto& manager = ctx.engine().getPluginManager();
    const auto before = manager.getLv2EffectDescriptions().size()
                      + manager.getLv2InstrumentDescriptions().size();

    // The scan runs on the calling thread, so the only way to observe the abort
    // check is to raise the flag before entering: a cancel that is ignored walks
    // the whole LV2 path and replaces the cached results.
    std::atomic<bool> abort { true };
    const auto start = std::chrono::steady_clock::now();
    manager.scanLv2Plugins (&abort);
    const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                            std::chrono::steady_clock::now() - start).count();

    const auto after = manager.getLv2EffectDescriptions().size()
                     + manager.getLv2InstrumentDescriptions().size();

    ctx.note ("cancelled scan returned in " + std::to_string (tookMs) + " ms");
    ctx.note ("descriptions before " + std::to_string (before)
              + ", after " + std::to_string (after));

    if (tookMs >= kReturnBudgetMs)
        return ScenarioResult::fail ("a cancelled LV2 scan took " + std::to_string (tookMs) + " ms");
    if (after != before)
        return ScenarioResult::fail ("a cancelled LV2 scan still replaced the plugin list");
    return ScenarioResult::pass();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "scan.cancel_lv2",
    { "scan", "lv2", "plugin" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return runCancel (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native LV2 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
