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
#if DUSKSTUDIO_HAS_NATIVE_AU
constexpr long long kReturnBudgetMs = 1500;

ScenarioResult runCancel (ScenarioContext& ctx)
{
    auto& manager = ctx.engine().getPluginManager();
    const auto before = manager.getAuEffectDescriptions().size()
                      + manager.getAuInstrumentDescriptions().size();

    // See scan.cancel_lv2: the scan runs on the calling thread, so the flag has
    // to be up before entering for the abort check to be observable.
    std::atomic<bool> abort { true };
    const auto start = std::chrono::steady_clock::now();
    manager.scanAuPlugins (&abort);
    const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                            std::chrono::steady_clock::now() - start).count();

    const auto after = manager.getAuEffectDescriptions().size()
                     + manager.getAuInstrumentDescriptions().size();

    ctx.note ("cancelled scan returned in " + std::to_string (tookMs) + " ms");
    ctx.note ("descriptions before " + std::to_string (before)
              + ", after " + std::to_string (after));

    if (tookMs >= kReturnBudgetMs)
        return ScenarioResult::fail ("a cancelled AU scan took " + std::to_string (tookMs) + " ms");
    if (after != before)
        return ScenarioResult::fail ("a cancelled AU scan still replaced the plugin list");
    return ScenarioResult::pass();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "scan.cancel_au",
    { "scan", "au", "plugin" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_AU
        return runCancel (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("AU scanning is macOS only");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
