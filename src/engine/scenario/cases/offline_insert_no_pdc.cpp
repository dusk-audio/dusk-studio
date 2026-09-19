#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/ChannelStrip.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace duskstudio::scenario
{
namespace
{
ScenarioResult auxReturnAtPdcLimit (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const int savedFactor = session.oversamplingFactor.load();
    ctx.cleanup ([&engine, &session, savedFactor]
    {
        engine.setAuxStemCapture (1, nullptr, nullptr);
        session.oversamplingFactor.store (savedFactor);
        for (int a = 0; a < 2; ++a)
        {
            session.auxLane (a).hardwareInserts[0].enabled.store (false);
            session.auxLane (a).hardwareInserts[0].routing.publish (
                std::make_unique<HardwareInsertRouting>());
            engine.getAuxLaneStrip (a).insertMode[0].store (AuxLaneStrip::kInsertEmpty);
        }
        engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
    });

    for (int a = 0; a < 2; ++a)
    {
        auto& hardware = session.auxLane (a).hardwareInserts[0];
        HardwareInsertRouting routing;
        routing.latencySamples = a == 0 ? ChannelStrip::kMaxPdcSamples : 0;
        if (a == 1) { routing.inputChL = 0; routing.inputChR = 1; }
        hardware.routing.publish (std::make_unique<HardwareInsertRouting> (routing));
        hardware.enabled.store (true);
        engine.getAuxLaneStrip (a).insertMode[0].store (AuxLaneStrip::kInsertHardware);
    }

    constexpr int kFrames = ScenarioContext::kBlockSize;
    std::array<float, kFrames> inL {}, inR {}, outL {}, outR {}, wetL {}, wetR {};
    const float* inputs[] = { inL.data(), inR.data() };
    float* outputs[] = { outL.data(), outR.data() };
    engine.setAuxStemCapture (1, wetL.data(), wetR.data());

    for (int factor : { 1, 2, 4 })
    {
        session.oversamplingFactor.store (factor);
        engine.prepareForSelfTest (ScenarioContext::kSampleRate, kFrames);
        ctx.pump (8);
        engine.applyMasterPdcTargetsNow();
        ctx.expect (engine.getMasterDryPdcTargetSamples() == ChannelStrip::kMaxPdcSamples,
                    "the deepest aux lane did not reach the PDC limit");
        const int expected = engine.getAuxReturnLatencySamples()
                           - engine.getTrackOutputLatencySamples();
        int peakAtL = -1, peakAtR = -1;
        float peakL = 0.0f, peakR = 0.0f;
        for (int block = 0; block <= expected / kFrames + 1; ++block)
        {
            inL.fill (0.0f); inR.fill (0.0f);
            wetL.fill (0.0f); wetR.fill (0.0f);
            if (block == 0) { inL[0] = 0.5f; inR[0] = 0.25f; }
            engine.audioDeviceIOCallback (inputs, 2, outputs, 2, kFrames, {});
            for (int i = 0; i < kFrames; ++i)
            {
                if (std::abs (wetL[(size_t) i]) > peakL)
                { peakL = std::abs (wetL[(size_t) i]); peakAtL = block * kFrames + i; }
                if (std::abs (wetR[(size_t) i]) > peakR)
                { peakR = std::abs (wetR[(size_t) i]); peakAtR = block * kFrames + i; }
            }
        }
        ctx.note (std::to_string (factor) + "x: aux impulse L=" + std::to_string (peakAtL)
                  + " R=" + std::to_string (peakAtR) + " expected=" + std::to_string (expected));
        ctx.expect (peakL > 0.1f && peakR > 0.1f, "the aux return was silent");
        ctx.expect (peakAtL == expected && peakAtR == expected,
                    "the aux return lost bus alignment at the PDC limit");
    }
    engine.setAuxStemCapture (1, nullptr, nullptr);
    return ctx.verdict();
}

const ScenarioRegistrar auxPdcRegistrar { Scenario {
    "engine.aux_return_at_pdc_limit", { "engine", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return auxReturnAtPdcLimit (ctx); }
} };

#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kOfflineStrip = 0;
constexpr int kOtherStrip   = 3;
constexpr int kFixtureLatencySamples = 64;

ScenarioResult runOfflineInsert (ScenarioContext& ctx)
{
    const auto fixturePath = ctx.fixture ("relayout.vst3");
    if (! fixturePath)
        return ScenarioResult::skip ("missing fixture: relayout.vst3");
    const auto fixture = *fixturePath;
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
