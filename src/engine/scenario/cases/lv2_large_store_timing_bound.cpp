#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_LV2
constexpr const char* kPluginUri = "urn:duskstudio:test:file-state";
// The fixture stores exactly one file per save, so the file count is driven by
// how many slots carry it and how many generations the session goes through.
constexpr int kSlots       = 20;
constexpr int kGenerations = 10;
constexpr long long kBudgetMs = 3000;

long long elapsedMsSince (std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (
               std::chrono::steady_clock::now() - start).count();
}

ScenarioResult runTimingBound (ScenarioContext& ctx)
{
    const auto fixture = *ctx.fixture ("file_state.lv2");
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    const auto& sessionDir = ctx.sessionDir();

    const int slots = std::min (kSlots, Session::kNumTracks);
    for (int t = 0; t < slots; ++t)
    {
        std::string error;
        if (! engine.getChannelStrip (t).getNativeLv2Slot().load (
                fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize,
                error, kPluginUri))
            return ScenarioResult::fail ("could not load the fixture on track "
                                         + std::to_string (t + 1) + ": " + error);
    }

    const auto sessionFile = sessionDir / "session.json";

    // First generation primes the state directory and the shared copy store; the
    // measured ones are the steady-state saves a user actually waits on.
    engine.publishPluginStateForSave (true);
    if (! SessionSerializer::save (session, sessionFile))
        return ScenarioResult::fail ("the priming session save failed");

    long long slowestMs = 0;
    long long totalMs = 0;
    for (int generation = 0; generation < kGenerations; ++generation)
    {
        const auto start = std::chrono::steady_clock::now();
        engine.publishPluginStateForSave (true);
        const bool saved = SessionSerializer::save (session, sessionFile);
        const auto took = elapsedMsSince (start);
        totalMs += took;
        slowestMs = std::max (slowestMs, took);
        if (! saved)
            return ScenarioResult::fail ("session save failed on generation "
                                         + std::to_string (generation + 1));
    }

    int storedFiles = 0;
    std::error_code fsError;
    for (std::filesystem::recursive_directory_iterator it (sessionDir / "state", fsError), end;
         ! fsError && it != end; it.increment (fsError))
        if (it->is_regular_file()) ++storedFiles;

    ctx.note ("slots " + std::to_string (slots)
              + ", generations " + std::to_string (kGenerations)
              + ", files stored per generation " + std::to_string (slots)
              + ", files under the session state directory " + std::to_string (storedFiles));
    ctx.note ("slowest save " + std::to_string (slowestMs) + " ms, total "
              + std::to_string (totalMs) + " ms");

    for (int t = 0; t < slots; ++t)
        engine.getChannelStrip (t).unloadNativeLv2();

    if (fsError)
        return ScenarioResult::fail ("could not walk the session state directory: " + fsError.message());
    if (storedFiles == 0)
        return ScenarioResult::fail ("the saves stored no state files, so there was nothing to time");
    if (slowestMs >= kBudgetMs)
        return ScenarioResult::fail ("a session save took " + std::to_string (slowestMs)
                                     + " ms, over the " + std::to_string (kBudgetMs)
                                     + " ms budget");
    return ScenarioResult::pass();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "lv2.large_store_timing_bound",
    { "lv2", "state", "session", "slow" },
    Needs::Engine,
    { "file_state.lv2" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_LV2
        return runTimingBound (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native LV2 host");
       #endif
    },
    120000
} };
} // namespace
} // namespace duskstudio::scenario
