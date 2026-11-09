#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"
#include "../../../session/RegionEditActions.h"

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../../vst3/Vst3Bundle.h"
#endif

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The timeline's notes end when it stops playing them while the transport
// rolls - a region muted under them, or Record pressed on the armed track -
// and so does the hold its own sustain pedal has on them.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;
constexpr float kBpm = 120.0f;
constexpr const char* kProbeName = "Dusk Every-CC Probe Fixture";
constexpr int kInsideTick = 600;

std::int64_t samplesAt (int tick)
{
    return ticksToSamples (tick, ScenarioContext::kSampleRate, kBpm);
}

int counter (const vst3::NativeVst3Slot& slot, const char* name)
{
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr || info->name != name) continue;
        double value = 0.0;
        if (slot.getParamValue (info->id, value))
            return (int) std::lround (value * (info->name == "Held Notes" ? 16.0 : 65535.0));
    }
    return -1;
}

struct Sounding
{
    int held = 0, sustained = 0, pedalDown = 0;
};

Sounding readSounding (const vst3::NativeVst3Slot& slot)
{
    return { counter (slot, "Held Notes"), counter (slot, "Sustained Notes"), counter (slot, "Pedal Down Channels") };
}

std::string describe (const Sounding& s)
{
    return "held=" + std::to_string (s.held) + " sustained=" + std::to_string (s.sustained)
         + " pedalDown=" + std::to_string (s.pedalDown);
}

// The pedal goes down at tick 50 and stays; one note is held over the edit
// and one already released under the pedal.
MidiRegion pedalRegion()
{
    MidiRegion region;
    region.lengthInTicks = 2000;
    region.lengthInSamples = samplesAt (2000);
    region.recordedAtBPM = kBpm;
    region.ccs.push_back ({ 1, 64, 127, 50 });
    region.notes.push_back ({ 1, 60, 100, 100, 1400 });
    region.notes.push_back ({ 1, 64, 100, 300, 100 });
    return region;
}

void publish (Track& track)
{
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { pedalRegion() }));
}

void leg (ScenarioContext& ctx, const vst3::NativeVst3Slot& slot, const std::string& what,
          const std::function<bool()>& edit)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    transport.setPlayhead (0);
    engine.play();
    for (int block = 0; transport.getPlayhead() <= samplesAt (kInsideTick) && block < 1000; ++block)
        ctx.pump (1);
    const auto sounding = readSounding (slot);
    if (! ctx.expect (sounding.held == 1 && sounding.sustained == 1 && sounding.pedalDown == 1,
                      what + ": the region's notes and pedal never reached the instrument"))
        ctx.note (what + ": " + describe (sounding));
    if (! ctx.expect (edit(), what + ": the edit was refused"))
        return;
    ctx.pump (kSettleBlocks);
    const auto after = readSounding (slot);
    ctx.note (what + ": sounding " + describe (sounding) + " / after " + describe (after));
    ctx.expect (after.held == 0, what + ": the region's note was left held");
    ctx.expect (after.sustained == 0, what + ": the region's pedal left its notes sustained");
    ctx.expect (after.pedalDown == 0, what + ": the region's pedal was left down");
    engine.stop();
    ctx.pump (kSettleBlocks);
}

ScenarioResult runReleaseLiftsPedal (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    const auto fixture = *ctx.fixture ("panic_probe.vst3");
    vst3::Vst3Bundle bundle;
    std::string error;
    if (! ctx.expect (bundle.load (fixture.string(), error), "the fixture module did not load"))
    {
        ctx.note ("module error: " + error);
        return ctx.verdict();
    }
    std::string classId;
    for (const auto& plugin : bundle.plugins())
        if (plugin.isInstrument && plugin.name == kProbeName) { classId = plugin.id; break; }
    if (! ctx.expect (! classId.empty(), "the fixture advertises no every-CC instrument class"))
        return ctx.verdict();

    auto& track = session.track (kTrack);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    track.inputMonitor.store (false, std::memory_order_relaxed);
    track.midiInputIndex.store (-1, std::memory_order_relaxed);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (kBpm, std::memory_order_release);
    session.recomputeRtCounters();

    auto& strip = engine.getChannelStrip (kTrack);
    auto& slot = strip.getNativeVst3Slot();
    if (! ctx.expect (slot.load (fixture, ScenarioContext::kSampleRate,
                                 ScenarioContext::kBlockSize, error, classId),
                      "the track could not load the probe"))
    {
        ctx.note ("load error: " + error);
        return ctx.verdict();
    }
    ctx.cleanup ([&engine, &track, &strip, &session]
    {
        engine.stop();
        track.recordArmed.store (false, std::memory_order_relaxed);
        session.recomputeRtCounters();
        engine.getUndoManager().clearUndoHistory();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        strip.unloadNativeVst3();
    });
    engine.getUndoManager().clearUndoHistory();

    publish (track);
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "muting the region", [&ctx, &session, &engine, &track]
    {
        const auto before = track.midiRegions.current()[0];
        auto after = before;
        after.muted = true;
        auto& undo = engine.getUndoManager();
        undo.beginNewTransaction();
        return undo.perform (new MidiRegionEditAction (session, engine, kTrack, 0, before, after));
    });

    publish (track);
    track.recordArmed.store (true, std::memory_order_relaxed);
    session.recomputeRtCounters();
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "pressing Record while playing", [&engine]
    {
        engine.record();
        return engine.getTransport().isRecording();
    });
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "midi.release_lifts_timeline_pedal",
    { "vst3", "midi", "panic", "region" },
    Needs::Engine,
    { "panic_probe.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runReleaseLiftsPedal (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
