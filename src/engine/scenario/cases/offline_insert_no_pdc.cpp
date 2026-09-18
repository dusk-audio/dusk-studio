#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"

#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kOfflineStrip = 0;
constexpr int kOtherStrip   = 3;
constexpr int kFixtureLatencySamples = 64;

ScenarioResult runOfflineInsert (ScenarioContext& ctx)
{
    const auto fixture = *ctx.fixture ("relayout.vst3");
    auto& engine = ctx.engine();

    // The fixture reports latency only with "Latency Mode" on, and the host reads
    // the number at activation, so the parameter move has to be followed by a
    // re-activation before the slot can report anything but zero.
    auto loadWithLatency = [&] (int trackIndex)
    {
        auto& slot = engine.getChannelStrip (trackIndex).getNativeVst3Slot();
        std::string error;
        if (! slot.load (fixture, ScenarioContext::kSampleRate,
                         ScenarioContext::kBlockSize, error))
        {
            ctx.note ("track " + std::to_string (trackIndex + 1) + " load error: " + error);
            return false;
        }

        for (int i = 0; i < slot.paramCount(); ++i)
            if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == "Latency Mode")
                slot.setParamValue (info->id, 1.0);

        if (! slot.reactivate (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error))
        {
            ctx.note ("track " + std::to_string (trackIndex + 1) + " reactivate error: " + error);
            return false;
        }
        return true;
    };

    if (! ctx.expect (loadWithLatency (kOfflineStrip), "could not arm the first insert"))
        return ctx.verdict();
    if (! ctx.expect (loadWithLatency (kOtherStrip), "could not arm the second insert"))
        return ctx.verdict();

    auto& offlineSlot = engine.getChannelStrip (kOfflineStrip).getNativeVst3Slot();
    auto& otherSlot   = engine.getChannelStrip (kOtherStrip).getNativeVst3Slot();

    if (! ctx.expect (offlineSlot.getLatencySamples() == kFixtureLatencySamples,
                  "the fixture reported no latency, so there is nothing for PDC to drop"))
    {
        ctx.note ("reported latency: " + std::to_string (offlineSlot.getLatencySamples()));
        return ctx.verdict();
    }

    engine.recomputePdc();
    const int online = engine.getAggregatePdcLatencySamples();
    ctx.note ("aggregate PDC with both inserts online: " + std::to_string (online));
    ctx.expect (online == kFixtureLatencySamples, "an online insert did not reach the aggregate PDC");

    // Quarantine is how a failed re-activation takes an insert offline: the
    // instance stays alive for its editor and state, but the audio path passes
    // dry, so it must stop contributing latency.
    offlineSlot.quarantineAfterFailedReactivation();
    engine.recomputePdc();
    ctx.expect (offlineSlot.getLatencySamples() == 0, "an offline insert still reported latency");
    ctx.expect (otherSlot.getLatencySamples() == kFixtureLatencySamples,
            "taking one insert offline changed another strip's latency");
    ctx.expect (engine.getAggregatePdcLatencySamples() == kFixtureLatencySamples,
            "the remaining strip's latency vanished from the aggregate PDC");

    otherSlot.setBypassed (true);
    engine.recomputePdc();
    ctx.expect (engine.getAggregatePdcLatencySamples() == 0,
            "PDC survived every insert going offline");

    // Unloading keeps a slot's bypass flag, and the world reset does not touch
    // it, so the next scenario to load a VST3 on that strip would start bypassed.
    otherSlot.setBypassed (false);
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "plugin.offline_insert_no_pdc",
    { "plugin", "pdc", "vst3" },
    Needs::Engine,
    { "relayout.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runOfflineInsert (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
