#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../../vst3/Vst3Bundle.h"
#endif

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A VST3 synth built with JUCE assigns every MIDI controller to a parameter,
// all notes off included, whether or not its voices act on it. Stopping a
// region's playback, from inside a loop or not, must still end every note the
// instrument was given.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;
constexpr int kSilenceBlocks = 256;
constexpr float kBpm = 120.0f;
constexpr const char* kProbeName = "Dusk Every-CC Probe Fixture";
constexpr double kCounterScale = 65535.0;
constexpr double kMaxHeldNotes = 16.0;
// CC 0-127, channel pressure and pitch bend, per channel.
constexpr int kMappableControllers = 130;

// Off the block grid, like a region recorded wherever the take began.
constexpr std::int64_t kRegionStart = 64962;
constexpr int kRegionTicks = 4000;
// The E minor chord with B4 over it, and where in it Stop is pressed.
constexpr int kStopTick = 3758;
constexpr int kChordNotes = 4;
constexpr int kLiveKey = 64;

std::int64_t samplesAt (int tick)
{
    return ticksToSamples (tick, ScenarioContext::kSampleRate, kBpm);
}

struct Leg
{
    const char* name;
    bool loop;
    int loopStartTick;
    int loopEndTick;
    int wraps;
    int heldAfterWrap;
};
// Over the region, the chord ends before the loop does. Inside the chord the
// seam cuts it and B4 with it, and chases the chord back in on every pass.
constexpr Leg kLegs[] {
    { "no loop", false, 0, kRegionTicks, 0, 0 },
    { "loop over the region, first pass", true, 0, kRegionTicks, 0, 0 },
    { "loop over the region, after a wrap", true, 0, kRegionTicks, 1, 0 },
    { "loop inside the chord, after a wrap", true, 1000, 3800, 1, 3 },
};

double counter (const vst3::NativeVst3Slot& slot, const char* name)
{
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr || info->name != name) continue;
        double value = 0.0;
        if (slot.getParamValue (info->id, value))
            return value * (info->name == "Held Notes" ? kMaxHeldNotes : kCounterScale);
    }
    return -1.0;
}

struct Counters
{
    int held = 0, noteOffs = 0, panicPoints = 0, retriggers = 0;
};

Counters readCounters (const vst3::NativeVst3Slot& slot)
{
    const auto read = [&slot] (const char* name) { return (int) std::lround (counter (slot, name)); };
    return { read ("Held Notes"), read ("Note Offs Seen"), read ("Panic Points Seen"), read ("Retriggers") };
}

std::string describe (const Counters& c)
{
    return "held=" + std::to_string (c.held) + " noteOffs=" + std::to_string (c.noteOffs)
         + " panicPoints=" + std::to_string (c.panicPoints) + " retriggers=" + std::to_string (c.retriggers);
}

void publishChordRegion (Track& track)
{
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.timelineStart = kRegionStart;
    region.lengthInTicks = kRegionTicks;
    region.lengthInSamples = samplesAt (kRegionTicks);
    region.notes.push_back ({ 1, 59, 41, 393, 3488 });
    region.notes.push_back ({ 1, 55, 63, 397, 3494 });
    region.notes.push_back ({ 1, 52, 60, 399, 3496 });
    region.notes.push_back ({ 1, 72, 61, 1349, 604 });
    region.notes.push_back ({ 1, 71, 51, 1888, 1437 });
    region.notes.push_back ({ 1, 71, 43, 3527, 370 });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));
}

void runLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input,
             const Leg& leg, bool monitoring)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = ctx.session().track (kTrack);
    const std::string name = std::string (leg.name) + (monitoring ? ", IN on" : ", IN off");

    track.inputMonitor.store (monitoring, std::memory_order_relaxed);
    transport.setLoopRange (kRegionStart + samplesAt (leg.loopStartTick),
                            kRegionStart + samplesAt (leg.loopEndTick));
    transport.setLoopEnabled (leg.loop);
    ctx.pump (kSettleBlocks);

    const auto before = readCounters (slot);
    ctx.expect (before.held == 0, name + ": a note was still held before the leg began");

    const auto rollStart = kRegionStart + samplesAt (leg.loopStartTick);
    const auto rolled = (std::int64_t) leg.wraps * samplesAt (leg.loopEndTick - leg.loopStartTick)
                      + samplesAt (kStopTick - leg.loopStartTick);
    const int blocks = (int) (rolled / ScenarioContext::kBlockSize);
    transport.setPlayhead (rollStart);
    engine.play();
    int pumped = 0;
    if (leg.wraps > 0)
    {
        pumped = (int) (samplesAt (leg.loopEndTick - leg.loopStartTick) / ScenarioContext::kBlockSize) + 4;
        ctx.pump (pumped);
        const auto wrapped = readCounters (slot);
        ctx.note (name + ": just past the seam " + describe (wrapped));
        ctx.expect (wrapped.held == leg.heldAfterWrap,
                    name + ": the loop seam left the instrument holding a note the loop start does not play");
    }
    static constexpr int kLiveKeyLead = 8;
    int expectedHeld = kChordNotes;
    if (monitoring)
    {
        ctx.pump (blocks - pumped - kLiveKeyLead);
        dusk::MidiBuffer key;
        midiprobe::addMessage (key, 0x91, kLiveKey, 90);
        ctx.pumpWithMidi (input, std::move (key));
        ctx.pump (kLiveKeyLead - 1);
        ++expectedHeld;
    }
    else
    {
        ctx.pump (blocks - pumped);
    }

    const auto playing = readCounters (slot);
    ctx.note (name + ": playhead " + std::to_string ((long long) transport.getPlayhead())
              + " before Stop, " + describe (playing));
    ctx.expect (playing.held == expectedHeld,
                name + ": the instrument was not holding the chord" + (monitoring ? " and the live key" : "")
                + " when Stop was pressed");

    engine.pressStop();
    ctx.pump (kSettleBlocks);
    const auto stopped = readCounters (slot);
    ctx.note (name + ": after Stop " + describe (stopped));
    ctx.expect (stopped.held == 0, name + ": notes were still held after Stop");
    ctx.expect (stopped.noteOffs - playing.noteOffs >= playing.held,
                name + ": Stop did not send a note-off for every held note");
    ctx.expect (stopped.panicPoints > playing.panicPoints,
                name + ": the instrument's all-notes-off parameters were not sent");
    ctx.expect (stopped.retriggers == 0, name + ": a note was started again while it still sounded");
    // A stopped MIDI track is heard through IN only, so listen through it.
    track.inputMonitor.store (true, std::memory_order_relaxed);
    const int silentAfter = midiprobe::pumpUntilSilent (ctx, kTrack, kSilenceBlocks);
    ctx.expect (silentAfter >= 0, name + ": the track was still audible after Stop");

    if (monitoring)
    {
        dusk::MidiBuffer up;
        midiprobe::addMessage (up, 0x81, kLiveKey, 0);
        ctx.pumpWithMidi (input, std::move (up));
        ctx.pump (1);
    }
}

ScenarioResult runStopEndsMappedSynthNotes (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");

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
    midiprobe::makeLiveMidiTrack (ctx, kTrack, input);
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
    ctx.cleanup ([&engine, &transport, &track, &strip]
    {
        engine.stop();
        transport.setLoopEnabled (false);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        strip.unloadNativeVst3();
    });
    ctx.expect (slot.getInstance()->midiCcMappingCount() == 16 * kMappableControllers,
                "the probe does not map every controller on every channel");

    publishChordRegion (track);
    for (const auto& leg : kLegs)
        for (const bool monitoring : { true, false })
            runLeg (ctx, slot, input, leg, monitoring);
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "vst3.stop_ends_mapped_synth_notes",
    { "vst3", "midi", "panic", "transport" },
    Needs::Engine,
    { "panic_probe.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runStopEndsMappedSynthNotes (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
