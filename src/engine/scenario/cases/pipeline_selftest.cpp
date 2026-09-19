#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../ScenarioWorld.h"
#include "../../AudioEngine.h"
#include "../../AudioPipelineSelfTest.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace duskstudio::scenario
{
namespace
{
const ScenarioRegistrar resetRegistrar { Scenario {
    "engine.scenario_world_resets_mix", { "engine", "selftest" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
        ScenarioWorld world;
        auto& session = world.session();
        const Session defaults;
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            auto& track = session.track (t);
            track.name = "changed";
            auto& strip = track.strip;
            strip.faderDb.store (-12.0f); strip.liveFaderDb.store (-9.0f);
            strip.pan.store (0.5f); strip.livePan.store (-0.5f);
            for (int a = 0; a < ChannelStripParams::kNumAuxSends; ++a)
            {
                strip.auxSendDb[(size_t) a].store (-3.0f);
                strip.liveAuxSendDb[(size_t) a].store (-6.0f);
            }
        }
        session.master().faderDb.store (-18.0f);
        session.master().liveFaderDb.store (-15.0f);
        session.master().mute.store (true);
        auto& aux = session.auxLane (0).params;
        aux.returnLevelDb.store (-12.0f);
        aux.liveReturnLevelDb.store (-12.0f);
        aux.mute.store (true);
        aux.liveMute.store (true);
        aux.outputPair.store (3);
        aux.faderTouched.store (true);
        aux.automationMode.store ((int) AutomationMode::Read);
        aux.automationLanes[0].publishPoints ({ { 0, 0.5f, 120.0f } });
        session.track (0).automationMode.store ((int) AutomationMode::Read);
        session.track (0).automationLanes[0].publishPoints ({ { 0, 0.5f, 120.0f } });
        world.reset();

        const auto matches = [&ctx] (float actual, float expected)
        {
            ctx.expect (std::abs (actual - expected) < 1.0e-6f,
                        "a continuous mix value survived the world reset");
        };
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto& strip = session.track (t).strip;
            const auto& expected = defaults.track (t).strip;
            ctx.expect (session.track (t).name == defaults.track (t).name,
                        "a track name survived the world reset");
            matches (strip.faderDb.load(), expected.faderDb.load());
            matches (strip.liveFaderDb.load(), expected.liveFaderDb.load());
            matches (strip.pan.load(), expected.pan.load());
            matches (strip.livePan.load(), expected.livePan.load());
            for (int a = 0; a < ChannelStripParams::kNumAuxSends; ++a)
            {
                matches (strip.auxSendDb[(size_t) a].load(), expected.auxSendDb[(size_t) a].load());
                matches (strip.liveAuxSendDb[(size_t) a].load(), expected.liveAuxSendDb[(size_t) a].load());
            }
        }
        matches (session.master().faderDb.load(), defaults.master().faderDb.load());
        matches (session.master().liveFaderDb.load(), defaults.master().liveFaderDb.load());
        ctx.expect (session.master().mute.load() == defaults.master().mute.load(),
                    "master mute survived the world reset");
        const auto& auxDefaults = defaults.auxLane (0).params;
        matches (aux.returnLevelDb.load(), auxDefaults.returnLevelDb.load());
        matches (aux.liveReturnLevelDb.load(), auxDefaults.liveReturnLevelDb.load());
        ctx.expect (aux.mute.load() == auxDefaults.mute.load() && aux.liveMute.load() == auxDefaults.liveMute.load()
                        && aux.outputPair.load() == auxDefaults.outputPair.load()
                        && aux.faderTouched.load() == auxDefaults.faderTouched.load(),
                    "an aux return's mute, output or touch survived the world reset");
        ctx.expect (aux.automationMode.load() == (int) AutomationMode::Off && aux.automationLanes[0].pointsConst().empty()
                        && session.track (0).automationMode.load() == (int) AutomationMode::Off
                        && session.track (0).automationLanes[0].pointsConst().empty(),
                    "automation survived the world reset");
        return ctx.verdict();
    }
} };

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
