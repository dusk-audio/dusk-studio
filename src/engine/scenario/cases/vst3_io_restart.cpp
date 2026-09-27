#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// A native VST3 plug-in that changes its buses outputs silence until the host
// restarts it. These drive the engine's own message-thread drain: the fixture
// announces the change from a parameter, or from inside its own activation,
// and the drain's ticks on the message loop do the rest.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kTrack = 0;

// Enough drain ticks at 30 Hz for a restart that is going to happen to have
// happened, twice over.
constexpr int kDrainWaitMs = 2000;
// A restart that runs at once lands on the next 33 ms drain tick; a second
// leaves room for a loaded machine and is still far short of waiting for Stop.
constexpr int kPromptWaitMs = 1000;

vst3::NativeVst3Slot& slotOf (ScenarioContext& ctx)
{
    return ctx.engine().getChannelStrip (kTrack).getNativeVst3Slot();
}

bool setParam (ScenarioContext& ctx, const char* name, double value)
{
    auto& slot = slotOf (ctx);
    for (int i = 0; i < slot.paramCount(); ++i)
        if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == name)
        {
            slot.setParamValue (info->id, value);
            return true;
        }
    return ctx.expect (false, std::string ("the fixture has no \"") + name + "\" parameter");
}

// Back in the audio path with the buses it announced: nothing holds
// processBlock silent any more.
bool runningWithOutputs (ScenarioContext& ctx, size_t outputs)
{
    auto& slot = slotOf (ctx);
    auto* instance = slot.getInstance();
    return instance != nullptr && ! instance->ioChangePending()
        && instance->portLayout().outputs.size() == outputs
        && slot.isProcessingOnline() && slot.hasActiveInstance();
}

// Offline, the slot hands its input straight back rather than the silence a
// plug-in waiting for its restart gives. Nothing else drives the engine here,
// so the slot can be called directly.
bool passesDry (ScenarioContext& ctx)
{
    static constexpr int kFrames = ScenarioContext::kBlockSize;
    std::vector<float> inL ((size_t) kFrames, 0.5f), inR ((size_t) kFrames, -0.25f);
    std::vector<float> outL ((size_t) kFrames, 0.0f), outR ((size_t) kFrames, 0.0f);
    slotOf (ctx).processStereo (inL.data(), inR.data(), outL.data(), outR.data(), kFrames);
    for (size_t i = 0; i < (size_t) kFrames; ++i)
        if (std::abs (outL[i] - inL[i]) > 1.0e-6f || std::abs (outR[i] - inR[i]) > 1.0e-6f)
            return false;
    return true;
}

// Loads the fixture on a mono track whose input is monitored (and, for a take,
// armed), so the strip runs its insert on every pumped block.
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

    const auto fixture = ctx.fixture ("relayout.vst3");
    std::string error;
    auto& engine = ctx.engine();
    ctx.cleanup ([&engine] { engine.getChannelStrip (kTrack).getNativeVst3Slot().unload(); });
    if (! ctx.expect (fixture.has_value()
                          && slotOf (ctx).load (*fixture, ScenarioContext::kSampleRate,
                                                ScenarioContext::kBlockSize, error),
                      "the relayout fixture did not load"))
    {
        ctx.note ("load error: " + error);
        return false;
    }
    ctx.pump (4);
    return ctx.expect (runningWithOutputs (ctx, 2), "the fixture was not running after it loaded");
}

// The restart suspends the engine for a moment, a gap in every take being
// recorded; waiting for Stop would leave the insert silent in what is heard,
// and in a PRINT take, for the rest of it. So it does not wait.
std::optional<ScenarioResult> ioChangeRestartsDuringTake (ScenarioContext& ctx)
{
    if (! loadOnTrack (ctx, /*armForTake*/ true)) return ctx.verdict();

    auto& engine = ctx.engine();
    engine.getTransport().setPlayhead (0);
    engine.record();
    if (! ctx.expect (engine.getTransport().isRecording(), "Record did not start with the track armed"))
        return ctx.verdict();
    ctx.cleanup ([&engine] { if (! engine.getTransport().isStopped()) engine.stop(); });

    const auto changedAt = std::chrono::steady_clock::now();
    if (! setParam (ctx, "Expand Outputs", 1.0)) return ctx.verdict();
    ctx.expect (slotOf (ctx).getInstance()->ioChangePending(),
                "the fixture did not announce its new outputs");
    ctx.pump (1);

    ctx.waitUntil ([&ctx] { return runningWithOutputs (ctx, 3); }, kPromptWaitMs,
                   [&ctx, &engine, changedAt]
    {
        ctx.note ("back with its new outputs after "
                  + std::to_string (std::chrono::duration_cast<std::chrono::milliseconds> (
                        std::chrono::steady_clock::now() - changedAt).count())
                  + " ms");
        ctx.expect (engine.getTransport().isRecording(),
                    "the take was no longer recording when the plug-in came back");
        engine.stop();
        ctx.complete (ctx.verdict());
    }, "a plug-in that changed its outputs during a take was not restarted within a second");
    return std::nullopt;
}

// A plug-in that announces the change again from inside the restart, with the
// buses it has just been activated with, runs on; it is neither restarted
// again and again nor taken offline.
std::optional<ScenarioResult> repeatFromActivationRunsOn (ScenarioContext& ctx)
{
    if (! loadOnTrack (ctx, /*armForTake*/ false)) return ctx.verdict();
    if (! setParam (ctx, "Repeat IO On Activate", 1.0)) return ctx.verdict();
    if (! setParam (ctx, "Expand Outputs", 1.0)) return ctx.verdict();

    ctx.waitUntil ([&ctx] { return runningWithOutputs (ctx, 3); }, kDrainWaitMs, [&ctx]
    {
        // Longer than a run of back-to-back restarts and the quiet second that
        // would follow if the repeated change were honoured.
        ctx.later (1500, [&ctx]
        {
            ctx.pump (1);
            ctx.expect (runningWithOutputs (ctx, 3),
                        "a change repeated from inside the restart left the plug-in "
                        "silent or offline");
            ctx.complete (ctx.verdict());
        });
    }, "the plug-in was not restarted for its new outputs");
    return std::nullopt;
}

// A plug-in whose every activation moves its buses again cannot be run. After
// its burst of restarts it is taken offline, passes the dry signal and the
// Plug-in unavailable alert names it, once; it gets one more restart after a
// quiet second and, asking again at once, stays offline, still passing dry.
std::optional<ScenarioResult> restartStormGoesOffline (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto alerts = std::make_shared<std::vector<AudioEngine::PluginLoadFailure>>();
    engine.setPluginRestoreAlertSink ([alerts] (std::vector<AudioEngine::PluginLoadFailure> failures)
    {
        alerts->insert (alerts->end(), failures.begin(), failures.end());
    });
    ctx.cleanup ([&engine] { engine.setPluginRestoreAlertSink ({}); });

    if (! loadOnTrack (ctx, /*armForTake*/ false)) return ctx.verdict();
    if (! setParam (ctx, "Flip Outputs On Activate", 1.0)) return ctx.verdict();
    if (! setParam (ctx, "Expand Outputs", 1.0)) return ctx.verdict();

    ctx.waitUntil ([&ctx] { return ! slotOf (ctx).isProcessingOnline(); }, kDrainWaitMs,
                   [&ctx, alerts]
    {
        ctx.expect (alerts->size() == 1, "taking the plug-in offline raised no alert");
        ctx.expect (passesDry (ctx), "the plug-in taken offline did not pass the dry signal");
        ctx.later (2500, [&ctx, alerts]
        {
            ctx.expect (! slotOf (ctx).isProcessingOnline(),
                        "a plug-in that moves its buses in every activation came back online");
            ctx.expect (passesDry (ctx),
                        "the plug-in kept offline after its last restart did not pass the dry signal");
            ctx.expect (slotOf (ctx).getLatencySamples() == 0,
                        "an offline plug-in still reported latency");
            if (ctx.expect (alerts->size() == 1, "the alert was raised more than once"))
            {
                const auto& failure = alerts->front();
                ctx.note ("alert: " + failure.location.toStdString() + " - "
                          + failure.pluginName.toStdString() + " [" + failure.format + "]: "
                          + failure.reason);
                ctx.expect (failure.location == "Track 1" && failure.format == "VST3",
                            "the alert did not name the track and format");
            }
            ctx.complete (ctx.verdict());
        });
    }, "a plug-in that moves its buses in every activation was restarted without end");
    return std::nullopt;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_VST3
#define DUSK_VST3_CASE(fn) fn
#else
#define DUSK_VST3_CASE(fn) [] (ScenarioContext&) -> std::optional<ScenarioResult> \
    { return ScenarioResult::skip ("built without the native VST3 host"); }
#endif

const ScenarioRegistrar takeRegistrar { Scenario {
    "vst3.io_change_restarts_during_take", { "vst3", "record" }, Needs::Engine,
    { "relayout.vst3" }, DUSK_VST3_CASE (ioChangeRestartsDuringTake)
} };

const ScenarioRegistrar repeatRegistrar { Scenario {
    "vst3.io_change_repeated_from_activation_runs_on", { "vst3" }, Needs::Engine,
    { "relayout.vst3" }, DUSK_VST3_CASE (repeatFromActivationRunsOn)
} };

const ScenarioRegistrar stormRegistrar { Scenario {
    "vst3.io_restart_storm_goes_offline", { "vst3" }, Needs::Engine,
    { "relayout.vst3" }, DUSK_VST3_CASE (restartStormGoesOffline)
} };

#undef DUSK_VST3_CASE
} // namespace
} // namespace duskstudio::scenario
