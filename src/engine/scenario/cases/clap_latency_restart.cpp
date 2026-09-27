#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../hosting/RestartPacer.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"

#include <functional>
#include <string>
#include <utility>

// A CLAP plug-in changes its latency only inside activate(), so a running one
// asks the host for a restart. These drive the engine's own message-thread
// drain, not a stand-in for it: the fixture asks from process() as the engine
// pumps blocks, and the drain's ticks on the message loop do the rest.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrack = 0;
constexpr int kOtherTrack = 1;
constexpr const char* kLatencyClapId = "studio.dusk.test.latency";

// Enough drain ticks at 30 Hz for a restart that is going to happen to have
// happened, twice over.
constexpr int kDrainWaitMs = 2000;

clap::NativeClapSlot& slotOf (ScenarioContext& ctx)
{
    return ctx.engine().getChannelStrip (kTrack).getNativeClapSlot();
}

const clap::ClapInstance::ParamInfo* paramNamed (clap::NativeClapSlot& slot, const char* name)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == name)
            return info;
    return nullptr;
}

bool setParam (ScenarioContext& ctx, const char* name, double value)
{
    auto& slot = slotOf (ctx);
    const auto* info = paramNamed (slot, name);
    if (! ctx.expect (info != nullptr, std::string ("the fixture has no \"") + name + "\" parameter"))
        return false;
    slot.setParamValue (info->id, value);
    return true;
}

int activations (ScenarioContext& ctx)
{
    auto& slot = slotOf (ctx);
    double value = -1.0;
    if (const auto* info = paramNamed (slot, "Activations"))
        slot.getParamValue (info->id, value);
    return (int) value;
}

// Loads the fixture on a mono track whose input is monitored (and, for a take,
// armed), so the strip runs its insert on every pumped block and the plug-in
// sees its parameter events there. An armed track that does not monitor skips
// its insert chain.
bool loadOnTrack (ScenarioContext& ctx, bool armForTake)
{
    auto& session = ctx.session();
    auto& track = session.track (kTrack);
    ctx.keep (track.mode);
    ctx.keep (track.inputMonitor);
    ctx.keep (track.inputSource);
    ctx.keep (track.recordArmed);
    ctx.keep (session.deviceCaptureChannels);
    track.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    track.inputSource.store (-2, std::memory_order_relaxed);
    track.inputMonitor.store (true, std::memory_order_relaxed);
    if (armForTake)
    {
        // The arm is stored directly, past Session's input check, so pin a
        // capture width that offers the track's own input.
        session.deviceCaptureChannels.store (kTrack + 1);
        track.recordArmed.store (true, std::memory_order_relaxed);
    }
    session.recomputeRtCounters();

    const auto fixture = ctx.fixture ("latency.clap");
    std::string error;
    auto& engine = ctx.engine();
    ctx.cleanup ([&engine] { engine.getChannelStrip (kTrack).getNativeClapSlot().unload(); });
    if (! ctx.expect (fixture.has_value()
                          && slotOf (ctx).load (*fixture, ScenarioContext::kSampleRate,
                                                ScenarioContext::kBlockSize, error, kLatencyClapId),
                      "the latency fixture did not load"))
    {
        ctx.note ("load error: " + error);
        return false;
    }
    ctx.pump (4);
    return ctx.expect (engine.getAggregatePdcLatencySamples() == 0,
                       "compensation was not zero before the look-ahead moved");
}

// The drain consumes the request, fences the engine, reactivates the slot and
// recomputes PDC. Nothing pumps a block while it waits, so the per-block PDC
// pass cannot be what moves the compensation.
std::optional<ScenarioResult> latencyFollowsRestart (ScenarioContext& ctx)
{
    static constexpr int kLookAhead = 256;
    if (! loadOnTrack (ctx, /*armForTake*/ false)) return ctx.verdict();
    if (! setParam (ctx, "Look-ahead", kLookAhead)) return ctx.verdict();
    ctx.pump (1);
    ctx.expect (slotOf (ctx).getLatencySamples() == 0,
                "the latency moved before the restart the plug-in asked for");

    auto& engine = ctx.engine();
    ctx.waitUntil ([&ctx, &engine]
    {
        return slotOf (ctx).getLatencySamples() == kLookAhead
            && engine.getAggregatePdcLatencySamples() == kLookAhead;
    }, kDrainWaitMs, [&ctx, &engine]
    {
        ctx.expect (engine.getChannelStrip (kOtherTrack).getPdcCompensationSamples() == kLookAhead,
                    "another track was not delayed to match the new latency");
        ctx.expect (activations (ctx) == 2, "the plug-in was not restarted exactly once");
        ctx.complete (ctx.verdict());
    }, "the plug-in's restart request never reached the slot's latency or PDC");
    return std::nullopt;
}

// Pumps one block every drain tick or so for `ms`, so a plug-in that asks from
// process() keeps asking, then runs `then`.
void keepPumping (ScenarioContext& ctx, int ms, std::function<void()> then)
{
    static constexpr int kStepMs = 33;
    if (ms <= 0) { then(); return; }
    ctx.later (kStepMs, [&ctx, ms, then = std::move (then)]() mutable
    {
        ctx.pump (1);
        keepPumping (ctx, ms - kStepMs, std::move (then));
    });
}

// A request made from inside activate() is dropped, not honoured on the next
// tick; a plug-in that asks on every block is spaced out and then held back.
std::optional<ScenarioResult> restartsArePaced (ScenarioContext& ctx)
{
    if (! loadOnTrack (ctx, /*armForTake*/ false)) return ctx.verdict();
    if (! setParam (ctx, "Ask in activate", 1.0)) return ctx.verdict();
    if (! setParam (ctx, "Look-ahead", 128.0)) return ctx.verdict();
    ctx.pump (1);

    ctx.waitUntil ([&ctx] { return activations (ctx) >= 2; }, kDrainWaitMs, [&ctx]
    {
        // Long enough for two more restarts if the one asked for inside
        // activate() were honoured, even spaced out.
        ctx.later (1200, [&ctx]
        {
            const int afterActivateAsks = activations (ctx);
            ctx.note ("activations after the request from inside activate(): "
                      + std::to_string (afterActivateAsks));
            ctx.expect (afterActivateAsks == 2,
                        "a request made from inside activate() restarted the plug-in again");

            setParam (ctx, "Ask in activate", 0.0);
            setParam (ctx, "Ask every block", 1.0);
            ctx.pump (1);
            keepPumping (ctx, 4000, [&ctx, afterActivateAsks]
            {
                const int storm = activations (ctx) - afterActivateAsks;
                ctx.note ("restarts over four seconds of asking on every block: "
                          + std::to_string (storm));
                ctx.expect (storm >= 2, "the plug-in's repeated requests were never honoured");
                ctx.expect (storm <= hosting::RestartPacer::kMaxBackToBack,
                            "a plug-in asking on every block was restarted without limit");
                setParam (ctx, "Ask every block", 0.0);
                ctx.pump (1);
                ctx.complete (ctx.verdict());
            });
        });
    }, "the first restart request was never honoured");
    return std::nullopt;
}

// A plug-in that fails to come back, and asks again while failing, is not
// retried on every tick.
std::optional<ScenarioResult> failedRestartIsNotRetried (ScenarioContext& ctx)
{
    if (! loadOnTrack (ctx, /*armForTake*/ false)) return ctx.verdict();
    if (! setParam (ctx, "Ask in activate", 1.0)) return ctx.verdict();
    if (! setParam (ctx, "Refuse activation", 1.0)) return ctx.verdict();
    if (! setParam (ctx, "Look-ahead", 64.0)) return ctx.verdict();
    ctx.pump (1);

    ctx.waitUntil ([&ctx] { return activations (ctx) >= 2; }, kDrainWaitMs, [&ctx]
    {
        ctx.later (1200, [&ctx]
        {
            const int count = activations (ctx);
            ctx.note ("activations after a refused restart: " + std::to_string (count));
            ctx.expect (count == 2, "a refused restart was retried");
            ctx.expect (! slotOf (ctx).isProcessingOnline(),
                        "a plug-in that refused to activate was left in the audio path");
            ctx.complete (ctx.verdict());
        });
    }, "the restart request was never honoured");
    return std::nullopt;
}

// A restart suspends the whole engine, which would put a gap in the take, so
// it waits for Stop.
std::optional<ScenarioResult> restartWaitsForTheTake (ScenarioContext& ctx)
{
    static constexpr int kLookAhead = 256;
    if (! loadOnTrack (ctx, /*armForTake*/ true)) return ctx.verdict();

    auto& engine = ctx.engine();
    engine.getTransport().setPlayhead (0);
    engine.record();
    if (! ctx.expect (engine.getTransport().isRecording(), "Record did not start with the track armed"))
        return ctx.verdict();
    ctx.cleanup ([&engine] { if (! engine.getTransport().isStopped()) engine.stop(); });

    if (! setParam (ctx, "Look-ahead", kLookAhead)) return ctx.verdict();
    keepPumping (ctx, 800, [&ctx, &engine]
    {
        ctx.expect (activations (ctx) == 1, "the plug-in was restarted while a take recorded");
        ctx.expect (slotOf (ctx).getLatencySamples() == 0,
                    "the latency moved while a take recorded");
        engine.stop();
        ctx.waitUntil ([&ctx] { return slotOf (ctx).getLatencySamples() == kLookAhead; },
                       kDrainWaitMs, [&ctx]
        {
            ctx.expect (activations (ctx) == 2, "the held restart did not run exactly once");
            ctx.complete (ctx.verdict());
        }, "the restart held through the take did not run after Stop");
    });
    return std::nullopt;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_CLAP
#define DUSK_CLAP_CASE(fn) fn
#else
#define DUSK_CLAP_CASE(fn) [] (ScenarioContext&) -> std::optional<ScenarioResult> \
    { return ScenarioResult::skip ("built without the native CLAP host"); }
#endif

const ScenarioRegistrar followsRegistrar { Scenario {
    "clap.latency_follows_restart", { "clap", "pdc", "latency" }, Needs::Engine,
    { "latency.clap" }, DUSK_CLAP_CASE (latencyFollowsRestart)
} };

const ScenarioRegistrar pacedRegistrar { Scenario {
    "clap.restart_requests_are_paced", { "clap", "latency" }, Needs::Engine,
    { "latency.clap" }, DUSK_CLAP_CASE (restartsArePaced)
} };

const ScenarioRegistrar refusedRegistrar { Scenario {
    "clap.refused_restart_is_not_retried", { "clap", "latency" }, Needs::Engine,
    { "latency.clap" }, DUSK_CLAP_CASE (failedRestartIsNotRetried)
} };

const ScenarioRegistrar takeRegistrar { Scenario {
    "clap.restart_waits_for_the_take", { "clap", "latency", "record" }, Needs::Engine,
    { "latency.clap" }, DUSK_CLAP_CASE (restartWaitsForTheTake)
} };

#undef DUSK_CLAP_CASE
} // namespace
} // namespace duskstudio::scenario
