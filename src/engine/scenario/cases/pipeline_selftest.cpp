#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../AudioPipelineSelfTest.h"

#include <cstdio>
#include <string>

namespace duskstudio::scenario
{
namespace
{
template <typename Fn>
void forEachLine (const std::string& text, Fn&& fn)
{
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto end = text.find ('\n', start);
        fn (text.substr (start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

// Adapter over the pre-existing 15-case pipeline self-test: the suite owns the
// verdict, the self-test keeps printing its own detail.
const ScenarioRegistrar registrar { Scenario {
    "engine.pipeline_selftest",
    { "engine", "selftest" },
    Needs::Engine,
    {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        AudioPipelineSelfTest test (ctx.engine(), ctx.engine().getDeviceManager(), ctx.session());
        const auto report = test.runAll();

        std::string failures;
        forEachLine (report, [&failures] (const std::string& line)
        {
            // Indented so the suite's own verdict lines stay the only ones at
            // column zero.
            std::fprintf (stdout, "  %s\n", line.c_str());
            if (line.find ("[FAIL]") == std::string::npos) return;
            if (! failures.empty()) failures += " | ";
            failures += line;
        });
        std::fflush (stdout);

        if (! failures.empty())
            return ScenarioResult::fail (failures);
        return ScenarioResult::pass();
    }
} };
} // namespace
} // namespace duskstudio::scenario
