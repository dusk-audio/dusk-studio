#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"

#include <string>
#include <utility>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
constexpr int kTrackIndex = 0;
constexpr const char* kPluginUri = "urn:duskstudio:test:many-patches";

ScenarioResult runNumbering (ScenarioContext& ctx)
{
    const auto fixture = *ctx.fixture ("many_patch.lv2");
    auto& slot = ctx.engine().getChannelStrip (kTrackIndex).getNativeLv2Slot();

    std::string firstFailure;
    auto expect = [&ctx, &firstFailure] (bool condition, std::string message)
    {
        if (! condition)
        {
            if (firstFailure.empty()) firstFailure = message;
            ctx.note (std::move (message));
        }
        return condition;
    };

    auto capture = [&] (std::vector<std::string>& into)
    {
        std::string error;
        if (! slot.load (fixture, ScenarioContext::kSampleRate,
                         ScenarioContext::kBlockSize, error, kPluginUri))
        {
            ctx.note ("load error: " + error);
            return false;
        }
        into.clear();
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* info = slot.paramInfo (i);
            if (info == nullptr) return false;
            into.push_back (info->name);
        }
        return true;
    };

    std::vector<std::string> first;
    if (! expect (capture (first), "the first load failed"))
        return ScenarioResult::fail (firstFailure);
    ctx.note ("parameters advertised: " + std::to_string (first.size()));
    expect (! first.empty(), "the fixture advertised no parameters");

    slot.unload();

    std::vector<std::string> second;
    if (! expect (capture (second), "the reload failed"))
        return ScenarioResult::fail (firstFailure);

    if (expect (first.size() == second.size(), "the reload advertised a different parameter count"))
        for (std::size_t i = 0; i < first.size(); ++i)
            if (! expect (first[i] == second[i],
                          "parameter " + std::to_string (i) + " changed identity across a reload: "
                              + first[i] + " -> " + second[i]))
                break;

    // Lilv gives no iteration order for a plugin's patch properties, so the host
    // appends them sorted by property URI to keep the index a MIDI binding
    // persists meaningful. These labels are the URI's own suffix, so the sorted
    // order is visible in the names.
    for (std::size_t i = 1; i < first.size(); ++i)
        if (! expect (first[i - 1] < first[i],
                      "parameters are not in ascending property order at index "
                          + std::to_string (i) + ": " + first[i - 1] + " then " + first[i]))
            break;

    slot.unload();

    return firstFailure.empty() ? ScenarioResult::pass()
                                : ScenarioResult::fail (firstFailure);
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "lv2.params_numbering_stable",
    { "lv2", "params", "plugin" },
    Needs::Engine,
    { "many_patch.lv2" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return runNumbering (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native LV2 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
