#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"
#include "../../device/IODeviceCallback.h"

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

// A loop wrap, a seek, a scrub or Play ends what the timeline was playing and
// nothing the player holds through IN: a held key, the player's own sustain
// pedal and a tail still ringing all go on. The timeline's pedal comes up
// before its notes end, even when the loop start puts it straight back down,
// and the controllers it moved go back to their defaults unless the loop
// start sets them. Stop still silences everything.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;
constexpr float kBpm = 120.0f;
constexpr const char* kProbeName = "Dusk Every-CC Probe Fixture";
constexpr const char* kLatentProbeName = "Dusk Every-CC Latent Probe Fixture";
constexpr double kCounterScale = 65535.0;
constexpr double kMaxHeldNotes = 16.0;

// The timeline plays channel 1, the player channels 2 and 3.
constexpr int kTimelinePedalTick = 50;
constexpr int kLiveTick = 300;
constexpr int kLoopEndTick = 800;
constexpr std::uint8_t kLiveKey = 72, kSustainedKey = 74, kTailKey = 76;
constexpr int kPlayerPedalChannels = 1 << 1;
constexpr int kTimelinePedalChannels = 1 << 0;

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

double valueOf (const vst3::NativeVst3Slot& slot, const char* name)
{
    for (int i = 0; i < slot.paramCount(); ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr || info->name != name) continue;
        double value = 0.0;
        if (slot.getParamValue (info->id, value)) return value;
    }
    return -1.0;
}

// What the instrument holds after the jump besides the player's key and the
// key under the player's pedal: the region's notes the new position plays,
// and its pedal when the new position holds it down.
struct Kept
{
    int timelineHeld = 0;
    int timelinePedal = 0;
};

void checkKept (ScenarioContext& ctx, const std::string& name, const Counters& before, const Counters& after,
                Kept timeline = {})
{
    ctx.note (name + ": before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.held == 1 + timeline.timelineHeld,
                name + ": the timeline's note was left held or the player's key was cut");
    ctx.expect (after.sustained == 1, name + ": the player's sustained key was cut or the timeline's was left sustained");
    ctx.expect (after.pedalDown == (kPlayerPedalChannels | timeline.timelinePedal),
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
              const std::string& name, std::int64_t loopStart, std::int64_t loopEnd, Kept timeline = {})
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    transport.setLoopRange (loopStart, loopEnd);
    transport.setLoopEnabled (true);
    transport.setPlayhead (loopStart);
    engine.play();
    const int liveBlocks = blockAt (samplesAt (kLiveTick) - loopStart);
    ctx.pump (liveBlocks);
    holdKeys (ctx, input);
    const int tailBlocks = blockAt (loopEnd - loopStart) - 3;
    ctx.pump (tailBlocks - liveBlocks - 2);
    send (ctx, input, { { 0x92, kTailKey, 90 } });
    send (ctx, input, { { 0x82, kTailKey, 0 } });
    const auto before = readCounters (slot);
    for (int block = 0; transport.getPlayhead() >= samplesAt (kLiveTick) && block < 64; ++block)
        ctx.pump (1);
    ctx.pump (kSettleBlocks);
    ctx.note (name + ": playhead " + std::to_string ((long long) transport.getPlayhead()) + " after the wrap");
    checkKept (ctx, name, before, readCounters (slot), timeline);
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

// Holding Rewind moves the playhead a stretch at a time while the transport
// rolls, each move a jump of its own.
void scrubLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input)
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
    for (int step = 1; step <= 6; ++step)
    {
        transport.locate (samplesAt (kLoopEndTick - 20 - step * 127));
        ctx.pump (1);
    }
    ctx.pump (kSettleBlocks - 1);
    ctx.note ("scrub while rolling: playhead " + std::to_string ((long long) transport.getPlayhead()));
    checkKept (ctx, "scrub while rolling", before, readCounters (slot));
    stopLeg (ctx, slot, input, "scrub while rolling");
}

// IN going off while the transport rolls stops the track listening to its
// input, so the instrument is reset outright and the timeline chased back in.
// The reset's pedal-up still lands ahead of the timeline's pedal coming back
// down, so the note that pedal was holding ends.
void monitorOffLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = ctx.session().track (kTrack);
    transport.setLoopEnabled (false);
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (blockAt (samplesAt (kLiveTick)));
    holdKeys (ctx, input);
    ctx.pump (blockAt (samplesAt (750)) - blockAt (samplesAt (kLiveTick)) - 2);
    const auto before = readCounters (slot);
    track.inputMonitor.store (false, std::memory_order_relaxed);
    ctx.pump (kSettleBlocks);
    const auto after = readCounters (slot);
    ctx.note ("IN off: before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.sustained == 0, "IN off: the note the timeline's pedal held was left sustained");
    ctx.expect (after.held == 1, "IN off: the timeline's note under the playhead was not chased back in");
    ctx.expect (after.pedalDown == kTimelinePedalChannels, "IN off: the timeline's pedal did not come back down");
    ctx.expect (after.retriggers == 0, "IN off: a note was started again while it still sounded");
    stopLeg (ctx, slot, input, "IN off");
    track.inputMonitor.store (true, std::memory_order_relaxed);
    ctx.pump (kSettleBlocks);
}

// The region moves the mod wheel, expression, pitch bend and pressure inside
// the loop; only expression has a value from before the loop start.
void controllersLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, Track& track)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.lengthInTicks = 1000;
    region.lengthInSamples = samplesAt (1000);
    region.ccs.push_back ({ 1, 11, 90, 50 });
    region.ccs.push_back ({ 1, 11, 50, 300 });
    region.ccs.push_back ({ 1, 1, 100, 300 });
    region.ccs.push_back ({ 1, 0, 12000, 300, MidiCc::Kind::PitchBend });
    region.ccs.push_back ({ 1, 0, 70, 300, MidiCc::Kind::ChannelPressure });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));

    const auto loopStart = samplesAt (100);
    transport.setLoopRange (loopStart, samplesAt (kLoopEndTick));
    transport.setLoopEnabled (true);
    transport.setPlayhead (loopStart);
    engine.play();
    ctx.pump (blockAt (samplesAt (kLoopEndTick) - loopStart) - 2);
    const auto inside = [&slot] (const char* name) { return valueOf (slot, name); };
    const double modulation = inside ("Modulation"), expression = inside ("Expression"),
                 bend = inside ("Pitch Bend"), pressure = inside ("Pressure");
    for (int block = 0; transport.getPlayhead() >= samplesAt (300) && block < 64; ++block)
        ctx.pump (1);
    ctx.pump (kSettleBlocks);
    const double wrappedModulation = inside ("Modulation"), wrappedExpression = inside ("Expression"),
                 wrappedBend = inside ("Pitch Bend"), wrappedPressure = inside ("Pressure");
    ctx.note ("controllers before the wrap: mod " + std::to_string (modulation) + " expression "
              + std::to_string (expression) + " bend " + std::to_string (bend) + " pressure "
              + std::to_string (pressure) + " / after: mod " + std::to_string (wrappedModulation)
              + " expression " + std::to_string (wrappedExpression) + " bend " + std::to_string (wrappedBend)
              + " pressure " + std::to_string (wrappedPressure));
    const auto approx = [] (double a, double b) { return std::abs (a - b) < 1.0e-3; };
    ctx.expect (approx (modulation, 100.0 / 127.0) && approx (expression, 50.0 / 127.0) && ! approx (bend, 8192.0 / 16383.0)
                    && approx (pressure, 70.0 / 127.0),
                "controllers: the region's controllers never reached the instrument");
    ctx.expect (approx (wrappedModulation, 0.0), "controllers: the mod wheel stayed where the region left it");
    ctx.expect (approx (wrappedExpression, 90.0 / 127.0), "controllers: expression did not take the loop start's value");
    ctx.expect (approx (wrappedBend, 8192.0 / 16383.0), "controllers: pitch bend did not come back to centre");
    ctx.expect (approx (wrappedPressure, 0.0), "controllers: pressure stayed where the region left it");
    engine.pressStop();
    transport.setLoopEnabled (false);
    ctx.pump (kSettleBlocks);
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

// More of the region's notes sound across the loop end than one release has
// room for, so the wrap resets the instrument outright, the player's key
// with it.
void overfullSeamLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input, Track& track)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.lengthInTicks = 1000;
    region.lengthInSamples = samplesAt (1000);
    for (int key = 30; key < 80; ++key)
        region.notes.push_back ({ 1, key, 100, 100, 800 });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));

    transport.setLoopRange (0, samplesAt (kLoopEndTick));
    transport.setLoopEnabled (true);
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (blockAt (samplesAt (kLiveTick)));
    holdKeys (ctx, input);
    const auto before = readCounters (slot);
    for (int block = 0; transport.getPlayhead() >= samplesAt (kLiveTick) && block < 256; ++block)
        ctx.pump (1);
    ctx.pump (kSettleBlocks);
    const auto after = readCounters (slot);
    ctx.note ("over 48 at the seam: before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.panicPoints > before.panicPoints, "over 48 at the seam: the wrap did not reset the instrument");
    ctx.expect (after.held == 0 && after.sustained == 0, "over 48 at the seam: a note survived the reset");
    engine.pressStop();
    transport.setLoopEnabled (false);
    ctx.pump (kSettleBlocks);
    releaseKeys (ctx, input);
    publishRegion (track);
    ctx.pump (kSettleBlocks);
}

// With IN off the track listens to its input only while stopped, so Play
// ends a key held from before it, as IN going off does, and chases the
// timeline in.
void playWithMonitorOffLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = ctx.session().track (kTrack);
    transport.setLoopEnabled (false);
    track.inputMonitor.store (false, std::memory_order_relaxed);
    transport.setPlayhead (samplesAt (700));
    ctx.pump (kSettleBlocks);
    holdKeys (ctx, input);
    const auto before = readCounters (slot);
    engine.play();
    ctx.pump (kSettleBlocks);
    const auto after = readCounters (slot);
    ctx.note ("Play with IN off: before " + describe (before) + " / after " + describe (after));
    ctx.expect (before.held == 1 && before.sustained == 1, "Play with IN off: the key never reached the instrument");
    ctx.expect (after.panicPoints > before.panicPoints, "Play with IN off: the instrument was not reset");
    ctx.expect (after.held == 1, "Play with IN off: the key was left held or the region's note not chased in");
    ctx.expect (after.sustained == 0, "Play with IN off: the key under the player's pedal rang on");
    ctx.expect (after.pedalDown == kTimelinePedalChannels, "Play with IN off: the timeline's pedal did not come down");
    stopLeg (ctx, slot, input, "Play with IN off");
    track.inputMonitor.store (true, std::memory_order_relaxed);
    ctx.pump (kSettleBlocks);
}

bool loadProbe (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, const char* name)
{
    const auto fixture = *ctx.fixture ("panic_probe.vst3");
    vst3::Vst3Bundle bundle;
    std::string error;
    if (! ctx.expect (bundle.load (fixture.string(), error), "the fixture module did not load"))
    {
        ctx.note ("module error: " + error);
        return false;
    }
    std::string classId;
    for (const auto& plugin : bundle.plugins())
        if (plugin.isInstrument && plugin.name == name) { classId = plugin.id; break; }
    if (! ctx.expect (! classId.empty(), std::string ("the fixture does not advertise ") + name))
        return false;
    if (ctx.expect (slot.load (fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize, error, classId),
                    "the track could not load the probe"))
        return true;
    ctx.note ("load error: " + error);
    return false;
}

// The track set up to play live MIDI into the probe, cleared again when the
// scenario ends.
bool setUp (ScenarioContext& ctx, int input, const char* probeName)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = session.track (kTrack);
    midiprobe::makeLiveMidiTrack (ctx, kTrack, input);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (kBpm, std::memory_order_release);
    session.recomputeRtCounters();
    auto& strip = engine.getChannelStrip (kTrack);
    if (! loadProbe (ctx, strip.getNativeVst3Slot(), probeName))
        return false;
    ctx.cleanup ([&engine, &transport, &track, &strip]
    {
        engine.stop();
        transport.setLoopEnabled (false);
        track.inputMonitor.store (true, std::memory_order_relaxed);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        strip.unloadNativeVst3();
    });
    return true;
}

void oneFrameSeamLeg (ScenarioContext& ctx, vst3::NativeVst3Slot& slot, int input)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = ctx.session().track (kTrack);
    const auto pumpFrame = [&] (int frames = 1)
    {
        std::array<float, 2> output {};
        std::array<float*, 2> outputs { &output[0], &output[1] };
        device::CallbackContext callback;
        engine.audioDeviceIOCallback (nullptr, 0, outputs.data(), 2, frames, callback);
    };

    enum class Next { Continue, Stop, Seek, EditUp, EditDown, NarrowInput, Reprepare };
    for (const auto next : { Next::Continue, Next::Stop, Next::Seek, Next::EditUp, Next::EditDown, Next::NarrowInput, Next::Reprepare })
    {
        engine.stop();
        transport.setLoopEnabled (false);
        track.inputMonitor.store (true, std::memory_order_relaxed);
        publishRegion (track);
        ctx.pump (kSettleBlocks);
        holdKeys (ctx, input);
        transport.setLoopRange (samplesAt (100), samplesAt (kLoopEndTick));
        transport.setLoopEnabled (true);
        transport.setPlayhead (samplesAt (kLoopEndTick) - ScenarioContext::kBlockSize);
        engine.play();
        ctx.pump (1);
        const auto before = readCounters (slot);
        pumpFrame();
        const auto lifted = readCounters (slot);
        ctx.note ("one-frame seam: before " + describe (before) + " / lifted " + describe (lifted));
        ctx.expect (before.pedalDown == (kPlayerPedalChannels | kTimelinePedalChannels),
                    "one-frame seam: the timeline pedal was not held before the wrap");
        ctx.expect (lifted.pedalDown == kPlayerPedalChannels,
                    "one-frame seam: the pedal re-press erased its release");
        ctx.expect (lifted.pedalLifts > before.pedalLifts,
                    "one-frame seam: no pedal release reached the instrument");
        ctx.expect (lifted.sustained == 1 && lifted.held == 2,
                    "one-frame seam: a live voice was cut or a timeline voice was left sustained");
        pumpFrame (0);
        ctx.expect (readCounters (slot).pedalDown == lifted.pedalDown,
                    "an empty block consumed the deferred pedal");
        if (next == Next::Stop) engine.pressStop();
        if (next == Next::Seek)
        {
            transport.setLoopEnabled (false);
            transport.setPlayhead (0);
        }
        if (next == Next::EditUp || next == Next::EditDown)
        {
            auto regions = std::make_unique<std::vector<MidiRegion>> (track.midiRegions.current());
            regions->front().ccs.front().value = next == Next::EditUp ? 0 : 100;
            track.midiRegions.publish (std::move (regions));
        }
        if (next == Next::NarrowInput) track.inputMonitor.store (false, std::memory_order_relaxed);
        if (next == Next::Reprepare) engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
        pumpFrame();
        const auto restored = readCounters (slot);
        if (next == Next::Stop)
            checkStopped (ctx, "Stop before deferred pedal", lifted, restored);
        else if (next == Next::NarrowInput || next == Next::Reprepare)
        {
            ctx.expect (restored.held == 1 && restored.sustained == 0 && restored.pedalDown == 0,
                        "resetting the route restored an obsolete pedal or kept a live voice");
            pumpFrame();
            ctx.expect (readCounters (slot).pedalDown == kTimelinePedalChannels,
                        "the fresh reset did not restore its own deferred pedal");
        }
        else
            checkKept (ctx, "pedal after one frame, transition " + std::to_string ((int) next), before, restored,
                       { next == Next::Seek ? 0 : 1,
                         next == Next::Seek || next == Next::EditUp ? 0 : kTimelinePedalChannels });
        engine.pressStop();
        ctx.pump (kSettleBlocks);
        releaseKeys (ctx, input);
    }
}

ScenarioResult runSeamKeepsLiveKeys (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUp (ctx, input, kProbeName))
        return ctx.verdict();
    auto& track = ctx.session().track (kTrack);
    auto& slot = engine.getChannelStrip (kTrack).getNativeVst3Slot();
    publishRegion (track);
    ctx.pump (kSettleBlocks);

    wrapLeg (ctx, slot, input, "loop wrap inside a block", 0, samplesAt (kLoopEndTick));
    wrapLeg (ctx, slot, input, "loop wrap on a block boundary", 0,
             (samplesAt (kLoopEndTick) / ScenarioContext::kBlockSize) * ScenarioContext::kBlockSize);
    // The region's note on the loop start plays again at once.
    wrapLeg (ctx, slot, input, "loop wrap with the region's pedal held across the loop start",
             samplesAt (100), samplesAt (kLoopEndTick), { 1, kTimelinePedalChannels });
    wrapLeg (ctx, slot, input, "loop wrap with the region's pedal set on the loop start",
             samplesAt (kTimelinePedalTick), samplesAt (kLoopEndTick), { 0, kTimelinePedalChannels });
    // Rolled from the loop start, the seam lands on a block's last sample.
    wrapLeg (ctx, slot, input, "loop wrap on a block's last sample with the region's pedal held",
             samplesAt (100), samplesAt (100) + 136 * ScenarioContext::kBlockSize + ScenarioContext::kBlockSize - 1,
             { 1, kTimelinePedalChannels });
    seekLeg (ctx, slot, input);
    scrubLeg (ctx, slot, input);
    playLeg (ctx, slot, input);
    playWithMonitorOffLeg (ctx, slot, input);
    monitorOffLeg (ctx, slot, input);
    overfullSeamLeg (ctx, slot, input, track);
    controllersLeg (ctx, slot, track);
    oneFrameSeamLeg (ctx, slot, input);
    return ctx.verdict();
}

// An instrument with latency is scheduled that far ahead, so its MIDI window
// can start past the loop end while the transport has not yet wrapped. IN
// going off in that block resets the instrument and the seam's release falls
// on the same sample; the chase putting the region's pedal straight back down
// must still land after both, or the reset's pedal-up merges away and the key
// the reset ended rings on under the pedal.
ScenarioResult runLatentSeamFlush (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUp (ctx, input, kLatentProbeName))
        return ctx.verdict();
    auto& track = ctx.session().track (kTrack);
    auto& slot = engine.getChannelStrip (kTrack).getNativeVst3Slot();
    const int latency = engine.getChannelStrip (kTrack).getInsertPluginLatencySamples();
    ctx.note ("the probe reports " + std::to_string (latency) + " samples");
    if (! ctx.expect (latency > 300, "the latent probe reports too little latency to reach the loop end early"))
        return ctx.verdict();

    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.lengthInTicks = 1000;
    region.lengthInSamples = samplesAt (1000);
    region.ccs.push_back ({ 1, 64, 127, 10 });
    region.notes.push_back ({ 1, 64, 100, 300, 50 });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));

    // The block that starts 300 samples short of the loop end has its window
    // past it.
    static constexpr int kFlushBlock = 100;
    const auto loopStart = samplesAt (100);
    const auto loopEnd = loopStart + kFlushBlock * ScenarioContext::kBlockSize + 300;
    transport.setLoopRange (loopStart, loopEnd);
    transport.setLoopEnabled (true);
    transport.setPlayhead (loopStart);
    engine.play();
    ctx.pump (20);
    send (ctx, input, { { 0x90, kLiveKey, 90 } });
    ctx.pump (kFlushBlock - 21);
    const auto before = readCounters (slot);
    ctx.note ("block about to roll starts at " + std::to_string ((long long) transport.getPlayhead())
              + ", the loop ends at " + std::to_string ((long long) loopEnd));
    track.inputMonitor.store (false, std::memory_order_relaxed);
    ctx.pump (kSettleBlocks);
    const auto after = readCounters (slot);
    ctx.note ("IN off at a latent seam: before " + describe (before) + " / after " + describe (after));
    ctx.expect (before.held == 1 && before.pedalDown == kTimelinePedalChannels,
                "the key or the region's pedal never reached the instrument");
    ctx.expect (after.held == 0, "the reset left the key held");
    ctx.expect (after.sustained == 0, "the key the reset ended rang on under the region's pedal");
    ctx.expect (after.pedalDown == kTimelinePedalChannels, "the region's pedal did not come back down");
    engine.pressStop();
    ctx.pump (kSettleBlocks);
    return ctx.verdict();
}
#endif

const ScenarioRegistrar latentRegistrar { Scenario {
    "midi.latent_seam_flush_lifts_the_pedal",
    { "vst3", "midi", "panic", "transport" },
    Needs::Engine,
    { "panic_probe.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return runLatentSeamFlush (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }
} };

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
