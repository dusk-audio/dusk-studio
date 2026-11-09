#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"

#if DUSKSTUDIO_HAS_NATIVE_VST3
 #include "../../vst3/Vst3Bundle.h"
#endif

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// A loop wrap, a seek or Play ends what the timeline was playing and nothing
// the player holds: a key held through the track's input, the player's own
// sustain pedal and a tail still ringing all go on, and the timeline's pedal
// comes up. Stop still silences everything.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;
constexpr float kBpm = 120.0f;
constexpr const char* kProbeName = "Dusk Every-CC Probe Fixture";
constexpr double kCounterScale = 65535.0;
constexpr double kMaxHeldNotes = 16.0;

// The timeline plays channel 1, the player channels 2 and 3.
constexpr int kTimelinePedalTick = 50;
constexpr int kLiveTick = 300;
constexpr int kLoopEndTick = 800;
constexpr std::uint8_t kLiveKey = 72, kSustainedKey = 74, kTailKey = 76;
constexpr int kPlayerPedalChannels = 1 << 1;

std::int64_t samplesAt (int tick)
{
    return ticksToSamples (tick, ScenarioContext::kSampleRate, kBpm);
}

int blockAt (std::int64_t sample)
{
    return (int) (sample / ScenarioContext::kBlockSize);
}

struct Counters
{
    int held = 0, noteOffs = 0, panicPoints = 0, retriggers = 0, sustained = 0,
        pedalDown = 0, pedalLifts = 0, tailsRinging = 0, tailsCut = 0;
};

Counters readCounters (const vst3::NativeVst3Slot& slot)
{
    const auto read = [&slot] (const char* name)
    {
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* info = slot.paramInfo (i);
            if (info == nullptr || info->name != name) continue;
            double value = 0.0;
            if (slot.getParamValue (info->id, value))
                return (int) std::lround (value * (info->name == "Held Notes" ? kMaxHeldNotes : kCounterScale));
        }
        return -1;
    };
    return { read ("Held Notes"), read ("Note Offs Seen"), read ("Panic Points Seen"), read ("Retriggers"),
             read ("Sustained Notes"), read ("Pedal Down Channels"), read ("Pedal Lifts Seen"),
             read ("Tails Ringing"), read ("Tails Cut") };
}

std::string describe (const Counters& c)
{
    return "held=" + std::to_string (c.held) + " sustained=" + std::to_string (c.sustained)
         + " pedalDown=" + std::to_string (c.pedalDown) + " pedalLifts=" + std::to_string (c.pedalLifts)
         + " noteOffs=" + std::to_string (c.noteOffs) + " panicPoints=" + std::to_string (c.panicPoints)
         + " tailsRinging=" + std::to_string (c.tailsRinging) + " tailsCut=" + std::to_string (c.tailsCut)
         + " retriggers=" + std::to_string (c.retriggers);
}

void publishRegion (Track& track)
{
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.timelineStart = 0;
    region.lengthInTicks = 1000;
    region.lengthInSamples = samplesAt (1000);
    region.ccs.push_back ({ 1, 64, 127, kTimelinePedalTick });
    region.notes.push_back ({ 1, 60, 100, 100, 800 });
    region.notes.push_back ({ 1, 64, 100, 600, 100 });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));
}

void send (ScenarioContext& ctx, int input, std::initializer_list<std::array<std::uint8_t, 3>> messages)
{
    dusk::MidiBuffer buffer;
    for (const auto& m : messages)
        midiprobe::addMessage (buffer, m[0], m[1], m[2]);
    ctx.pumpWithMidi (input, std::move (buffer));
}

// The player's part: a key held on channel 2 under its own sustain pedal,
// with a second key released under the pedal.
void holdKeys (ScenarioContext& ctx, int input)
{
    send (ctx, input, { { 0x91, kLiveKey, 90 }, { 0xB1, 64, 127 }, { 0x91, kSustainedKey, 90 } });
    send (ctx, input, { { 0x81, kSustainedKey, 0 } });
}

void releaseKeys (ScenarioContext& ctx, int input)
{
    send (ctx, input, { { 0x81, kLiveKey, 0 }, { 0xB1, 64, 0 } });
}

void checkKept (ScenarioContext& ctx, const std::string& name, const Counters& before, const Counters& after)
{
    ctx.note (name + ": before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.held == 1, name + ": the timeline's note was left held or the player's key was cut");
    ctx.expect (after.sustained == 1, name + ": the player's sustained key was cut or the timeline's was left sustained");
    ctx.expect (after.pedalDown == kPlayerPedalChannels,
                name + ": the player's sustain pedal was lifted or the timeline's was left down");
    ctx.expect (after.panicPoints == before.panicPoints, name + ": all notes off or all sound off reached the instrument");
    ctx.expect (after.tailsCut == before.tailsCut, name + ": a release tail was cut short");
    ctx.expect (after.retriggers == 0, name + ": a note was started again while it still sounded");
}

void checkStopped (ScenarioContext& ctx, const std::string& name, const Counters& before, const Counters& after)
{
    ctx.note (name + ": after Stop " + describe (after));
    ctx.expect (after.held == 0 && after.sustained == 0, name + ": Stop left a note sounding");
    ctx.expect (after.pedalDown == 0, name + ": Stop left a sustain pedal down");
    ctx.expect (after.panicPoints > before.panicPoints, name + ": Stop sent no all notes off");
}

void stopLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input, const std::string& name)
{
    const auto before = readCounters (slot);
    ctx.engine().pressStop();
    ctx.pump (kSettleBlocks);
    checkStopped (ctx, name, before, readCounters (slot));
    releaseKeys (ctx, input);
    ctx.pump (kSettleBlocks);
}

void wrapLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input,
              const std::string& name, std::int64_t loopEnd)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    transport.setLoopRange (0, loopEnd);
    transport.setLoopEnabled (true);
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (blockAt (samplesAt (kLiveTick)));
    holdKeys (ctx, input);
    const int tailBlock = blockAt (loopEnd) - 3;
    ctx.pump (tailBlock - blockAt (samplesAt (kLiveTick)) - 2);
    send (ctx, input, { { 0x92, kTailKey, 90 } });
    send (ctx, input, { { 0x82, kTailKey, 0 } });
    const auto before = readCounters (slot);
    for (int block = 0; transport.getPlayhead() >= samplesAt (kLiveTick) && block < 64; ++block)
        ctx.pump (1);
    ctx.pump (kSettleBlocks);
    ctx.note (name + ": playhead " + std::to_string ((long long) transport.getPlayhead()) + " after the wrap");
    checkKept (ctx, name, before, readCounters (slot));
    stopLeg (ctx, slot, input, name);
    transport.setLoopEnabled (false);
}

void seekLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    transport.setLoopEnabled (false);
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (blockAt (samplesAt (kLiveTick)));
    holdKeys (ctx, input);
    ctx.pump (blockAt (samplesAt (kLoopEndTick - 20)) - blockAt (samplesAt (kLiveTick)) - 2);
    send (ctx, input, { { 0x92, kTailKey, 90 } });
    send (ctx, input, { { 0x82, kTailKey, 0 } });
    const auto before = readCounters (slot);
    transport.setPlayhead (samplesAt (20));
    ctx.pump (kSettleBlocks);
    checkKept (ctx, "seek while rolling", before, readCounters (slot));
    stopLeg (ctx, slot, input, "seek while rolling");
}

void playLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input)
{
    auto& transport = ctx.engine().getTransport();
    transport.setLoopEnabled (false);
    transport.setPlayhead (samplesAt (700));
    ctx.pump (kSettleBlocks);
    holdKeys (ctx, input);
    send (ctx, input, { { 0x92, kTailKey, 90 } });
    send (ctx, input, { { 0x82, kTailKey, 0 } });
    const auto before = readCounters (slot);
    ctx.engine().play();
    ctx.pump (kSettleBlocks);
    // The region's note held across tick 700 comes in with the chase.
    const auto after = readCounters (slot);
    ctx.note ("Play: before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.held == 2, "Play: the player's key was cut or the timeline's note was not chased in");
    ctx.expect (after.sustained == 1, "Play: the player's sustained key was cut");
    ctx.expect ((after.pedalDown & kPlayerPedalChannels) != 0, "Play: the player's sustain pedal was lifted");
    ctx.expect (after.panicPoints == before.panicPoints, "Play: all notes off or all sound off reached the instrument");
    ctx.expect (after.tailsCut == before.tailsCut, "Play: a release tail was cut short");
    ctx.expect (after.retriggers == 0, "Play: a note was started again while it still sounded");
    stopLeg (ctx, slot, input, "Play");
}

ScenarioResult runSeamKeepsLiveKeys (ScenarioContext& ctx)
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
    publishRegion (track);
    ctx.pump (kSettleBlocks);

    wrapLeg (ctx, slot, input, "loop wrap inside a block", samplesAt (kLoopEndTick));
    wrapLeg (ctx, slot, input, "loop wrap on a block boundary",
             (samplesAt (kLoopEndTick) / ScenarioContext::kBlockSize) * ScenarioContext::kBlockSize);
    seekLeg (ctx, slot, input);
    playLeg (ctx, slot, input);
    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "midi.seam_keeps_live_keys",
    { "vst3", "midi", "panic", "transport" },
    Needs::Engine,
    { "panic_probe.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runSeamKeepsLiveKeys (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
