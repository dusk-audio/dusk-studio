#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../dsp/AuxLaneStrip.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/Base64.h"
#include "../../../session/Session.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

// Parameter writes a CLAP insert has not taken yet - a host write still queued
// for the audio thread, a MIDI-binding write still queued for the drain timer,
// an edit the plug-in's GUI asked to have flushed - reach the plug-in through
// the engine's own Save capture and drain timer, not a stand-in for them.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrack = 5;
constexpr int kAuxLane = 2;
constexpr int kAuxSlot = 0;
constexpr const char* kParamTouchId = "studio.dusk.test.param-touch";
constexpr clap_id kAlpha = 40;
constexpr clap_id kBeta  = 41;
// Ticks of the 30 Hz drain past the idle wait an insert must sit out first.
constexpr int kDrainWaitMs = 3000;

// The leading members of the fixture's studio.dusk.test.param-touch-host.
struct ParamTouchHostControl
{
    bool (CLAP_ABI *markDirty) (const clap_plugin_t*);
    bool (CLAP_ABI *requestFlush) (const clap_plugin_t*);
    bool (CLAP_ABI *rescan) (const clap_plugin_t*, std::uint32_t flags);
    bool (CLAP_ABI *clear) (const clap_plugin_t*, clap_id id, std::uint32_t flags);
    bool (CLAP_ABI *editFromGui) (const clap_plugin_t*, clap_id id, double value);
};

const ParamTouchHostControl* hostControl (clap::NativeClapSlot& slot)
{
    const auto* instance = slot.getInstance();
    if (instance == nullptr) return nullptr;
    const auto* plugin = instance->getPlugin();
    return static_cast<const ParamTouchHostControl*> (
        plugin->get_extension (plugin, "studio.dusk.test.param-touch-host"));
}

std::optional<std::array<double, 2>> savedValues (const std::string& base64)
{
    const auto bytes = dusk::base64::decode (base64.data(), base64.size());
    if (bytes.size() != 20) return std::nullopt;
    std::array<double, 2> values {};
    std::memcpy (values.data(), bytes.data() + 4, sizeof (values));
    return values;
}

double liveValue (clap::NativeClapSlot& slot, clap_id id)
{
    double value = -1.0;
    slot.getParamValue (id, value);
    return value;
}

bool loadBoth (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    ctx.cleanup ([&engine]
    {
        engine.getChannelStrip (kTrack).unloadNativeClap();
        engine.getAuxLaneStrip (kAuxLane).unloadNativeClap (kAuxSlot);
    });
    const auto fixture = ctx.fixture ("param_touch.clap");
    std::string error;
    if (! ctx.expect (fixture.has_value()
                          && engine.getChannelStrip (kTrack).getNativeClapSlot().load (
                                 *fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize,
                                 error, kParamTouchId),
                      "could not load the track CLAP: " + error))
        return false;
    return ctx.expect (engine.getAuxLaneStrip (kAuxLane).getNativeClapSlot (kAuxSlot).load (
                           *fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize,
                           error, kParamTouchId),
                       "could not load the aux CLAP: " + error);
}

std::optional<ScenarioResult> saveCapturesQueuedEdits (ScenarioContext& ctx)
{
    if (! loadBoth (ctx)) return ctx.verdict();
    auto& engine = ctx.engine();
    auto& trackSlot = engine.getChannelStrip (kTrack).getNativeClapSlot();
    auto& auxSlot = engine.getAuxLaneStrip (kAuxLane).getNativeClapSlot (kAuxSlot);
    ctx.pump (2);

    const auto* control = hostControl (trackSlot);
    if (! ctx.expect (control != nullptr, "the fixture's host control is missing"))
        return ctx.verdict();
    trackSlot.setParamValue (kAlpha, 0.25);
    ctx.expect (control->editFromGui (trackSlot.getInstance()->getPlugin(), kBeta, 0.5),
                "the host offers the plug-in no request_flush");
    auxSlot.setBypassed (true);
    auxSlot.queueParamBinding (1, 0.75f);

    if (! ctx.expect (engine.publishPluginStateForSave (true), "the Save capture failed"))
        return ctx.verdict();
    const auto track = savedValues (ctx.session().track (kTrack).nativeClapStateBase64.toStdString());
    const auto aux = savedValues (
        ctx.session().auxLane (kAuxLane).nativeClapStateBase64[(std::size_t) kAuxSlot].toStdString());
    if (ctx.expect (track.has_value(), "the track's saved state is not the fixture's"))
    {
        ctx.note ("track saved alpha " + std::to_string ((*track)[0]) + ", beta " + std::to_string ((*track)[1]));
        ctx.expect (std::abs ((*track)[0] - 0.25) < 1.0e-9, "Save missed the track's queued host write");
        ctx.expect (std::abs ((*track)[1] - 0.5) < 1.0e-9, "Save missed the edit the track plug-in's GUI asked to flush");
    }
    if (ctx.expect (aux.has_value(), "the aux slot's saved state is not the fixture's"))
    {
        ctx.note ("aux saved beta " + std::to_string ((*aux)[1]));
        ctx.expect (std::abs ((*aux)[1] - 0.75) < 1.0e-6, "Save missed the bypassed aux insert's MIDI-binding write");
    }
    return ctx.verdict();
}

// Nothing pumps a block, so only the drain timer can answer.
std::optional<ScenarioResult> idleInsertTakesFlush (ScenarioContext& ctx)
{
    if (! loadBoth (ctx)) return ctx.verdict();
    auto& engine = ctx.engine();
    auto& trackSlot = engine.getChannelStrip (kTrack).getNativeClapSlot();
    auto& auxSlot = engine.getAuxLaneStrip (kAuxLane).getNativeClapSlot (kAuxSlot);

    const auto* control = hostControl (trackSlot);
    if (! ctx.expect (control != nullptr, "the fixture's host control is missing"))
        return ctx.verdict();
    const auto trackRevision = trackSlot.stateRevision();
    ctx.expect (control->editFromGui (trackSlot.getInstance()->getPlugin(), kAlpha, 0.6),
                "the host offers the plug-in no request_flush");
    auxSlot.setBypassed (true);
    auxSlot.setParamValue (kBeta, 0.3);

    ctx.waitUntil ([&trackSlot, &auxSlot]
    {
        return std::abs (liveValue (trackSlot, kAlpha) - 0.6) < 1.0e-9
            && std::abs (liveValue (auxSlot, kBeta) - 0.3) < 1.0e-9;
    }, kDrainWaitMs, [&ctx, &trackSlot, trackRevision]
    {
        ctx.expect (trackSlot.stateRevision() > trackRevision,
                    "the GUI edit the flush delivered left the revision alone");
        ctx.expect (trackSlot.lastTouchedParamIndex() == 0,
                    "the GUI edit the flush delivered did not report its parameter");
        ctx.complete (ctx.verdict());
    }, "an idle insert never took its plug-in's flush request or a queued write");
    return std::nullopt;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_CLAP
#define DUSK_CLAP_CASE(fn) fn
#else
#define DUSK_CLAP_CASE(fn) [] (ScenarioContext&) -> std::optional<ScenarioResult> \
    { return ScenarioResult::skip ("built without the native CLAP host"); }
#endif

const ScenarioRegistrar saveRegistrar { Scenario {
    "clap.save_captures_queued_edits", { "clap", "state", "plugin" }, Needs::Engine,
    { "param_touch.clap" }, DUSK_CLAP_CASE (saveCapturesQueuedEdits)
} };

const ScenarioRegistrar idleRegistrar { Scenario {
    "clap.idle_insert_takes_flush", { "clap", "params", "plugin" }, Needs::Engine,
    { "param_touch.clap" }, DUSK_CLAP_CASE (idleInsertTakesFlush)
} };

#undef DUSK_CLAP_CASE
} // namespace
} // namespace duskstudio::scenario
