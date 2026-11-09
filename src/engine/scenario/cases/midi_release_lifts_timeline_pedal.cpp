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
// rolls - a region muted under them, a note shortened, or Record pressed on
// the armed track. When what is left of the timeline still holds its sustain
// pedal there, they ring on under it, as the notes it released before did;
// when the edit took the pedal away, it comes up and lets them go, even when
// the edit ends no note.
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

MidiRegion emptyRegion()
{
    MidiRegion region;
    region.lengthInTicks = 2000;
    region.lengthInSamples = samplesAt (2000);
    region.recordedAtBPM = kBpm;
    return region;
}

// The pedal goes down at tick 50 and stays, and one note is already released
// under it by the time of the edit.
MidiRegion pedalRegion()
{
    auto region = emptyRegion();
    region.ccs.push_back ({ 1, 64, 127, 50 });
    region.notes.push_back ({ 1, 64, 100, 300, 100 });
    return region;
}

// The note held over the edit.
constexpr int kHeldKey = 60;
MidiNote heldNote() { return { 1, kHeldKey, 100, 100, 1400 }; }

void publish (Track& track, std::vector<MidiRegion> regions)
{
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::move (regions)));
}

std::vector<MidiRegion> oneRegion()
{
    auto region = pedalRegion();
    region.notes.push_back (heldNote());
    return { region };
}

bool editRegion (ScenarioContext& ctx, Track& track, int index, const std::function<void (MidiRegion&)>& change)
{
    const auto before = track.midiRegions.current()[(std::size_t) index];
    auto after = before;
    change (after);
    auto& undo = ctx.engine().getUndoManager();
    undo.beginNewTransaction();
    return undo.perform (new MidiRegionEditAction (ctx.session(), ctx.engine(), kTrack, index, before, after));
}

// The pedal gone: nothing left sounding. The pedal kept: the note the edit
// ends and the one released before both ring on under it.
constexpr Sounding kAllLetGo { 0, 0, 0 };
constexpr Sounding kRingingUnderThePedal { 0, 2, 1 };
// An edit that ends no note: the other region's note plays on, and the note
// released before rings on only while the pedal stays down.
constexpr Sounding kPedalLetUp { 1, 0, 0 };
constexpr Sounding kNothingChanged { 1, 1, 1 };

void leg (ScenarioContext& ctx, const vst3::NativeVst3Slot& slot, const std::string& what,
          const std::function<bool()>& edit, Sounding expected = kAllLetGo)
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
    ctx.expect (after.held == expected.held,
                what + (expected.held != 0 ? ": a note the edit left playing was cut"
                                           : ": the region's note was left held"));
    ctx.expect (after.sustained == expected.sustained,
                what + (expected.pedalDown != 0 ? ": the notes ringing under the pedal were cut"
                                                : ": the region's pedal left its notes sustained"));
    ctx.expect (after.pedalDown == expected.pedalDown,
                what + (expected.pedalDown != 0 ? ": the pedal the timeline still holds came up"
                                                : ": the region's pedal was left down"));
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

    publish (track, oneRegion());
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "muting the region that holds the pedal", [&ctx, &track]
    {
        return editRegion (ctx, track, 0, [] (MidiRegion& r) { r.muted = true; });
    });

    // A second region plays the held note; the first keeps the pedal down.
    auto notes = emptyRegion();
    notes.notes.push_back (heldNote());
    publish (track, { pedalRegion(), notes });
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "muting another region under the held pedal", [&ctx, &track]
    {
        return editRegion (ctx, track, 1, [] (MidiRegion& r) { r.muted = true; });
    }, kRingingUnderThePedal);

    publish (track, { pedalRegion(), notes });
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "muting the pedal's region while another region's note plays on", [&ctx, &track]
    {
        return editRegion (ctx, track, 0, [] (MidiRegion& r) { r.muted = true; });
    }, kPedalLetUp);

    publish (track, { pedalRegion(), notes });
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "an edit that ends no note and leaves the pedal down", [&ctx, &track]
    {
        return editRegion (ctx, track, 1, [] (MidiRegion& r) { r.notes.push_back ({ 1, 72, 100, 1800, 100 }); });
    }, kNothingChanged);

    publish (track, oneRegion());
    ctx.pump (kSettleBlocks);
    leg (ctx, slot, "shortening the held note under the held pedal", [&ctx, &track]
    {
        return editRegion (ctx, track, 0, [] (MidiRegion& r)
        {
            for (auto& note : r.notes)
                if (note.noteNumber == kHeldKey) note.lengthInTicks = kInsideTick / 2;
        });
    }, kRingingUnderThePedal);

    publish (track, oneRegion());
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
