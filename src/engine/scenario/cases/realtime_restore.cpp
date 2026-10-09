#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../RealtimeKit.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#if defined(__linux__)
 #include <csignal>
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

namespace duskstudio::scenario
{
namespace
{
// A Multicore DSP lane the realtime CPU-time warning moved to normal priority
// stays there while the transport rolls, and the engine's own once-a-second
// diagnostics put it back on realtime after the stop. The warning is the
// kernel's SIGXCPU, sent here to the lane's thread while it waits for a block,
// so the guard's handler runs on that thread as it would after an overrun.
std::optional<ScenarioResult> laneBackOnRealtimeAfterStop (ScenarioContext& ctx)
{
   #if defined(__linux__)
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! transport.isStopped()) return ScenarioResult::skip ("requires a stopped transport");

    // The guard catches SIGXCPU only under a finite limit. A soft one can be
    // put back up to the hard one afterwards, which leaves the hard one alone.
    rlimit was {};
    if (getrlimit (RLIMIT_RTTIME, &was) != 0) return ScenarioResult::skip ("no RLIMIT_RTTIME");
    if (was.rlim_cur == RLIM_INFINITY)
    {
        rlimit finite = was;
        finite.rlim_cur = 60'000'000;
        if (finite.rlim_max != RLIM_INFINITY) finite.rlim_cur = std::min (finite.rlim_cur, finite.rlim_max);
        if (setrlimit (RLIMIT_RTTIME, &finite) != 0)
            return ScenarioResult::skip ("RLIMIT_RTTIME would not take a soft limit");
    }
    ctx.cleanup ([was] { setrlimit (RLIMIT_RTTIME, &was); });
    if (! rt::guardRealtimeCpuTime())
        return ScenarioResult::skip ("SIGXCPU has a handler that is not Dusk Studio's");

    engine.setWorkerCountForTest (1);
    ctx.cleanup ([&engine] { engine.applyDesiredWorkers(); });
    const auto workers = engine.dspWorkerThreadIdsForTest();
    if (workers.empty()) return ScenarioResult::skip ("too few cores here for a Multicore DSP worker");
    const auto lane = workers.front();
    const auto granted = rt::threadScheduling (lane);
    if (! granted.isRealtime())
        return ScenarioResult::skip ("the DSP worker got no realtime priority here (RLIMIT_RTPRIO, RTKit)");
    // A failed run must not leave the lane at normal priority for the cases after it.
    ctx.cleanup ([lane, granted]
    {
        if (rt::threadScheduling (lane).isRealtime() || ! rt::threadIsBlocked (lane)) return;
        rt::RealtimeRestorer restorer;
        restorer.restore (lane, granted);
    });
    ctx.cleanup ([&engine, &transport, at = transport.getPlayhead()]
    {
        engine.stop();
        transport.setPlayhead (at);
        engine.resetXRunCounts();
    });

    transport.setPlayhead (0);
    engine.play();
    ctx.pump (4);
    if (! ctx.expect (! transport.isStopped(), "the transport did not start")) return ctx.verdict();

    const int before = rt::realtimeDemotions().count;
    if (! ctx.expect (syscall (SYS_tgkill, (pid_t) getpid(), (pid_t) lane, SIGXCPU) == 0,
                      "SIGXCPU could not be sent to the DSP worker"))
        return ctx.verdict();

    ctx.waitUntil ([before] { return rt::realtimeDemotions().count > before; }, 2000,
                   [&ctx, &engine, &transport, lane, before]
    {
        const auto demotions = rt::realtimeDemotions();
        ctx.expect (demotions.count == before + 1 && demotions.lastThreadId == lane,
                    "the warning moved a thread other than the DSP worker");
        if (! ctx.expect (! rt::threadScheduling (lane).isRealtime(),
                          "the warning left the DSP worker at realtime priority"))
        {
            ctx.complete (ctx.verdict());
            return;
        }

        // Two diagnostics ticks with the transport rolling.
        const auto stillPlaying = [&ctx, &engine, &transport, lane]
        {
            ctx.pump (4);
            ctx.expect (! transport.isStopped(), "the transport stopped by itself");
            ctx.expect (! rt::threadScheduling (lane).isRealtime(),
                        "the DSP worker was put back on realtime while the transport rolled");
            ctx.expect (! engine.isProcessingSuspended(), "the audio was held while the transport rolled");
        };
        ctx.later (1200, [&ctx, &engine, lane, stillPlaying]
        {
            stillPlaying();
            ctx.later (1200, [&ctx, &engine, lane, stillPlaying]
            {
                stillPlaying();
                engine.stop();
                ctx.waitUntil ([lane] { return rt::threadScheduling (lane).isRealtime(); }, 3000,
                               [&ctx, &engine]
                {
                    ctx.expect (! engine.isProcessingSuspended(), "the restore left the audio held");
                    ctx.pump (4);
                    ctx.complete (ctx.verdict());
                }, "the DSP worker was not put back on realtime after the transport stopped");
            });
        });
    }, "the SIGXCPU sent to the DSP worker moved nothing");
    return std::nullopt;
   #else
    (void) ctx;
    return ScenarioResult::skip ("the realtime CPU-time guard is Linux-only");
   #endif
}

const ScenarioRegistrar laneBackRegistrar { Scenario {
    "engine.dsp_lane_back_on_realtime_after_stop", { "engine", "transport" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return laneBackOnRealtimeAfterStop (ctx); }, 15000 } };
} // namespace
} // namespace duskstudio::scenario
