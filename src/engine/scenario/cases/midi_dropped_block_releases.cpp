#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A MIDI block with more events than the track can carry loses some of them,
// and a lost note-off would leave its note sounding. The instrument must not be
// left holding anything, and the timeline's held notes must come back.
namespace duskstudio::scenario
{
namespace
{
using Run = ScenarioResult (*) (ScenarioContext&);

#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;

int voicesHeld (ScenarioContext& ctx)
{
    return (int) midiprobe::counter (ctx, kTrack, "voicesHeld");
}

bool loadProbe (ScenarioContext& ctx)
{
    std::string error;
    if (ctx.expect (midiprobe::loadPanicProbe (ctx, kTrack, error), "the track could not load the probe"))
        return true;
    ctx.note ("load error: " + error);
    return false;
}

// Room for the hanging reset and a few dozen messages besides, so a burst of
// live controllers runs the routing buffer out before the note-offs behind it.
constexpr std::size_t kCappedRoutingBytes = 1024;
constexpr int kBurstControllers = 200;

ScenarioResult runRoutingOverflow (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");

    midiprobe::makeLiveMidiTrack (ctx, kTrack, input);
    ctx.session().recomputeRtCounters();
    if (! loadProbe (ctx)) return ctx.verdict();
    ctx.cleanup ([&engine] { engine.setTrackMidiCapacityForTest (kTrack, dusk::kMidiRoutingBlockBytes); });
    ctx.pump (kSettleBlocks);

    dusk::MidiBuffer down;
    midiprobe::addMessage (down, 0x90, 60, 100);
    midiprobe::addMessage (down, 0x90, 67, 100);
    ctx.pumpWithMidi (input, std::move (down));
    ctx.pump (1);
    const int held = voicesHeld (ctx);
    if (! ctx.expect (held == 2, "the instrument never received the notes"))
        return ctx.verdict();

    engine.setTrackMidiCapacityForTest (kTrack, kCappedRoutingBytes);
    dusk::MidiBuffer burst;
    for (int i = 0; i < kBurstControllers; ++i)
        midiprobe::addMessage (burst, 0xB0, 1, (std::uint8_t) (i & 0x7F),
                               i * (ScenarioContext::kBlockSize - 1) / kBurstControllers);
    midiprobe::addMessage (burst, 0x80, 60, 0, ScenarioContext::kBlockSize - 1);
    midiprobe::addMessage (burst, 0x80, 67, 0, ScenarioContext::kBlockSize - 1);
    ctx.pumpWithMidi (input, std::move (burst));
    ctx.pump (1);
    const int afterOverflow = voicesHeld (ctx);
    ctx.note ("held " + std::to_string (held) + ", after the overflowing block "
              + std::to_string (afterOverflow));
    ctx.expect (afterOverflow <= 0, "a block the routing buffer could not hold left the notes held");

    // The track plays as before once the block fits again.
    engine.setTrackMidiCapacityForTest (kTrack, dusk::kMidiRoutingBlockBytes);
    dusk::MidiBuffer again;
    midiprobe::addMessage (again, 0x90, 62, 100);
    ctx.pumpWithMidi (input, std::move (again));
    ctx.pump (1);
    ctx.expect (voicesHeld (ctx) == 1, "the track no longer played after the overflow");
    dusk::MidiBuffer up;
    midiprobe::addMessage (up, 0x80, 62, 0);
    ctx.pumpWithMidi (input, std::move (up));
    ctx.pump (1);
    ctx.expect (voicesHeld (ctx) <= 0, "a note after the overflow did not end on its note-off");
    return ctx.verdict();
}

// At 120 BPM a tick is 50 samples, so the burst and the short note's end land
// in block 10 and the long note runs to block 39.
constexpr int kBurstTick = 52;
constexpr int kShortNoteTicks = 54;
constexpr int kLongNoteTicks = 200;
constexpr int kBurstBlock = 10;
// More controllers than the generated-event budget lets one block carry.
constexpr int kDenseControllers = 6000;

ScenarioResult runDenseTimelineBlock (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (120.0f, std::memory_order_release);
    session.recomputeRtCounters();
    if (! loadProbe (ctx)) return ctx.verdict();
    ctx.cleanup ([&engine] { engine.stop(); });

    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.timelineStart = 0;
    region.lengthInTicks = 2 * kLongNoteTicks;
    region.lengthInSamples = ticksToSamples (region.lengthInTicks, ScenarioContext::kSampleRate, 120.0f);
    region.notes.push_back ({ 1, 60, 100, 0, kLongNoteTicks });
    region.notes.push_back ({ 1, 64, 100, 0, kShortNoteTicks });
    for (int i = 0; i < kDenseControllers; ++i)
        region.ccs.push_back ({ 1, 1, i & 0x7F, kBurstTick });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));
    ctx.pump (kSettleBlocks);

    engine.getTransport().setPlayhead (0);
    engine.play();
    ctx.pump (kBurstBlock);
    const int beforeBurst = voicesHeld (ctx);
    ctx.pump (1);
    const int atBurst = voicesHeld (ctx);
    ctx.pump (1);
    const int afterBurst = voicesHeld (ctx);
    ctx.pump (40 - kBurstBlock);
    const int afterLongNote = voicesHeld (ctx);
    ctx.note ("voices before the dense block " + std::to_string (beforeBurst) + ", in it "
              + std::to_string (atBurst) + ", the block after " + std::to_string (afterBurst)
              + ", once the long note is over " + std::to_string (afterLongNote));

    if (! ctx.expect (beforeBurst == 2, "the timeline's two notes never reached the instrument"))
        return ctx.verdict();
    ctx.expect (afterBurst == 1, "the long note held across the dense block was not chased back in");
    ctx.expect (afterLongNote <= 0, "a note-off the dense block could not carry left its note held");
    return ctx.verdict();
}
// Breath or expression recorded with a long take: more controllers in one
// region than the scheduler may look at in a block. Only the ones that fall in
// the block are its business, so the notes beside them must still play.
constexpr int kDenseTakeControllers = 40000;
constexpr int kDenseTakeNoteTick = 100;
constexpr int kDenseTakeNoteTicks = 200;

ScenarioResult runDenseControllerTake (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (120.0f, std::memory_order_release);
    session.recomputeRtCounters();
    if (! loadProbe (ctx)) return ctx.verdict();
    ctx.cleanup ([&engine, &track]
    {
        engine.stop();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    });

    auto regions = std::make_unique<std::vector<MidiRegion>>();
    MidiRegion region;
    region.timelineStart = 0;
    region.lengthInTicks = kDenseTakeControllers;
    region.lengthInSamples = ticksToSamples (region.lengthInTicks, ScenarioContext::kSampleRate, 120.0f);
    for (int i = 0; i < kDenseTakeControllers; ++i)
        region.ccs.push_back ({ 1, 2, i & 0x7F, i });
    region.notes.push_back ({ 1, 60, 100, kDenseTakeNoteTick, kDenseTakeNoteTicks });
    regions->push_back (std::move (region));
    track.midiRegions.publish (std::move (regions));
    ctx.pump (kSettleBlocks);

    const auto blockOf = [] (int tick)
    {
        return (int) (ticksToSamples (tick, ScenarioContext::kSampleRate, 120.0f)
                      / ScenarioContext::kBlockSize);
    };
    engine.getTransport().setPlayhead (0);
    engine.play();
    ctx.pump (blockOf (kDenseTakeNoteTick + kDenseTakeNoteTicks / 2));
    const int during = voicesHeld (ctx);
    ctx.pump (blockOf (kDenseTakeNoteTick + kDenseTakeNoteTicks) + 2
              - blockOf (kDenseTakeNoteTick + kDenseTakeNoteTicks / 2));
    const int after = voicesHeld (ctx);
    ctx.note ("voices in the note " + std::to_string (during) + ", after it "
              + std::to_string (after));
    ctx.expect (during == 1, "the note beside a dense controller take never played");
    ctx.expect (after <= 0, "the note beside a dense controller take did not end");
    return ctx.verdict();
}

// More regions than the scheduler may look at in a block. Whatever it could
// not get through may hold a note-off, so the block is the hanging reset, not
// a silent gap that leaves the instrument as it was.
constexpr int kOverfullRegions = 33000;

ScenarioResult runOverfullTimeline (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (120.0f, std::memory_order_release);
    session.recomputeRtCounters();
    if (! loadProbe (ctx)) return ctx.verdict();
    ctx.cleanup ([&engine, &track]
    {
        engine.stop();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    });

    // Far past where the transport plays, so none of them sounds.
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    regions->reserve ((std::size_t) kOverfullRegions);
    for (int i = 0; i < kOverfullRegions; ++i)
    {
        MidiRegion region;
        region.timelineStart = 100000000 + (std::int64_t) i * 100;
        region.lengthInTicks = 1;
        region.lengthInSamples = ticksToSamples (1, ScenarioContext::kSampleRate, 120.0f);
        regions->push_back (std::move (region));
    }
    track.midiRegions.publish (std::move (regions));
    ctx.pump (kSettleBlocks);

    // Starting the transport sends its own reset; count the ones after it.
    const auto chokes = [&ctx] { return (int) midiprobe::counter (ctx, kTrack, "chokesSeen"); };
    engine.getTransport().setPlayhead (0);
    engine.play();
    ctx.pump (2);
    const int started = chokes();
    ctx.pump (4);
    const int rolling = chokes();
    ctx.note ("chokes once rolling " + std::to_string (started) + ", four blocks later "
              + std::to_string (rolling));
    ctx.expect (rolling > started, "a block the scheduler could not get through reached the instrument without a reset");
    return ctx.verdict();
}
#else
ScenarioResult withoutClap (ScenarioContext&)
{
    return ScenarioResult::skip ("built without the native CLAP host");
}
constexpr Run runRoutingOverflow = withoutClap, runDenseTimelineBlock = withoutClap,
              runDenseControllerTake = withoutClap, runOverfullTimeline = withoutClap;
#endif

const std::vector<std::string> kTags { "midi", "panic", "clap" };
const std::vector<std::string> kFixtures { "panic_probe.clap" };

std::function<std::optional<ScenarioResult> (ScenarioContext&)> running (Run run)
{
    return [run] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return run (ctx); };
}

const ScenarioRegistrar overflowRegistrar { Scenario {
    "midi.release_on_routing_overflow", kTags, Needs::Engine, kFixtures, running (runRoutingOverflow) } };
const ScenarioRegistrar denseRegistrar { Scenario {
    "midi.release_on_dense_timeline_block", kTags, Needs::Engine, kFixtures, running (runDenseTimelineBlock) } };
const ScenarioRegistrar denseTakeRegistrar { Scenario {
    "midi.notes_beside_dense_controllers", kTags, Needs::Engine, kFixtures, running (runDenseControllerTake) } };
const ScenarioRegistrar overfullRegistrar { Scenario {
    "midi.release_on_overfull_timeline", kTags, Needs::Engine, kFixtures, running (runOverfullTimeline) } };
} // namespace
} // namespace duskstudio::scenario
