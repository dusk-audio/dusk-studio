#include "../Scenario.h"
#include "MidiProbeHarness.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Stop chokes every voice a CLAP instrument holds. A loop wrap or a jump while
// rolling ends only what the timeline played, so keys held through the track's
// input go on sounding across it.
namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrackIndex   = 0;
constexpr int kSettleBlocks = 3;
constexpr std::int64_t kJumpTarget = 48000 * 10;

// The first loop ends inside a block, so its seam falls there. The second ends
// on a block boundary, so the block before the seam ends on it and the
// transport has wrapped before the next, which meets the seam at its head.
struct LoopLeg
{
    const char* name;
    std::int64_t start;
    std::int64_t end;
};
constexpr LoopLeg kLoopLegs[] {
    { "loop wrap", 500, 3000 },
    { "block-aligned loop wrap", 2 * ScenarioContext::kBlockSize, 12 * ScenarioContext::kBlockSize },
};

struct Counters
{
    double voicesHeld   = 0.0;
    double chokesSeen   = 0.0;
    double noteOffsSeen = 0.0;
};

Counters readCounters (ScenarioContext& ctx)
{
    return { midiprobe::counter (ctx, kTrackIndex, "voicesHeld"),
             midiprobe::counter (ctx, kTrackIndex, "chokesSeen"),
             midiprobe::counter (ctx, kTrackIndex, "noteOffsSeen") };
}

std::string describe (const Counters& c)
{
    return "voicesHeld=" + std::to_string ((int) c.voicesHeld)
         + " chokesSeen=" + std::to_string ((int) c.chokesSeen)
         + " noteOffsSeen=" + std::to_string ((int) c.noteOffsSeen);
}

// Puts two voices on the instrument and reports what the fixture holds once the
// notes have been through a block.
Counters holdVoices (ScenarioContext& ctx, int input)
{
    dusk::MidiBuffer notes;
    midiprobe::addMessage (notes, 0x90, 60, 100);
    midiprobe::addMessage (notes, 0x90, 67, 100);
    ctx.pumpWithMidi (input, std::move (notes));
    ctx.pump (1);
    return readCounters (ctx);
}

void releaseVoices (ScenarioContext& ctx, int input)
{
    dusk::MidiBuffer notes;
    midiprobe::addMessage (notes, 0x80, 60, 0);
    midiprobe::addMessage (notes, 0x80, 67, 0);
    ctx.pumpWithMidi (input, std::move (notes));
    ctx.pump (1);
}

void checkKept (ScenarioContext& ctx, const std::string& event,
                const Counters& before, const Counters& after)
{
    const std::string prefix = event + ": ";
    ctx.note (prefix + "before " + describe (before) + " / after " + describe (after));
    ctx.expect ((int) after.voicesHeld == (int) before.voicesHeld, prefix + "the held keys were cut");
    ctx.expect ((int) after.chokesSeen == (int) before.chokesSeen, prefix + "a choke reached the plugin");
    ctx.expect ((int) after.noteOffsSeen == (int) before.noteOffsSeen, prefix + "a note-off reached the plugin");
}

void checkChoked (ScenarioContext& ctx, const char* event,
                  const Counters& before, const Counters& after)
{
    const std::string prefix = std::string (event) + ": ";
    ctx.note (prefix + "before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.voicesHeld <= 0.0, prefix + "voices were still held afterwards");
    ctx.expect (after.chokesSeen > before.chokesSeen, prefix + "no choke reached the plugin");
    ctx.expect ((int) after.noteOffsSeen == (int) before.noteOffsSeen,
                prefix + "the host sent note-offs instead of choking the voices");
}

ScenarioResult runChoke (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");

    midiprobe::makeLiveMidiTrack (ctx, kTrackIndex, input);
    session.recomputeRtCounters();

    std::string error;
    if (! ctx.expect (midiprobe::loadPanicProbe (ctx, kTrackIndex, error),
                      "the track could not load the probe"))
    {
        ctx.note ("load error: " + error);
        return ctx.verdict();
    }

    {
        engine.play();
        ctx.pump (kSettleBlocks);
        const auto held = holdVoices (ctx, input);
        ctx.expect (held.voicesHeld > 0.0, "stop: the instrument never received the notes");

        engine.stop();
        ctx.pump (kSettleBlocks);
        checkChoked (ctx, "stop", held, readCounters (ctx));
        releaseVoices (ctx, input);
    }

    for (const auto& leg : kLoopLegs)
    {
        const std::string name = leg.name;
        transport.setLoopRange (leg.start, leg.end);
        transport.setLoopEnabled (true);
        transport.setPlayhead (leg.start);
        engine.play();
        ctx.pump (kSettleBlocks);
        const auto held = holdVoices (ctx, input);
        ctx.expect (held.voicesHeld > 0.0, name + ": the instrument never received the notes");

        // Enough blocks to cross the loop end from anywhere inside it.
        ctx.pump ((int) ((leg.end - leg.start) / ScenarioContext::kBlockSize) + 2);
        ctx.note (name + ": playhead landed at "
                  + std::to_string ((long long) transport.getPlayhead()));
        checkKept (ctx, name, held, readCounters (ctx));

        engine.stop();
        transport.setLoopEnabled (false);
        ctx.pump (kSettleBlocks);
        releaseVoices (ctx, input);
    }

    {
        transport.setPlayhead (0);
        engine.play();
        ctx.pump (kSettleBlocks);
        const auto held = holdVoices (ctx, input);
        ctx.expect (held.voicesHeld > 0.0, "playhead jump: the instrument never received the notes");

        transport.setPlayhead (kJumpTarget);
        ctx.pump (kSettleBlocks);
        checkKept (ctx, "playhead jump", held, readCounters (ctx));

        engine.stop();
        ctx.pump (1);
        releaseVoices (ctx, input);
    }

    // A note a region plays gets its note-off from Stop as well as the choke,
    // for an instrument that would ignore the reset.
    {
        auto& track = session.track (kTrackIndex);
        auto regions = std::make_unique<std::vector<MidiRegion>>();
        MidiRegion region;
        region.lengthInTicks = 4 * kMidiTicksPerQuarter;
        region.lengthInSamples = ticksToSamples (region.lengthInTicks, ScenarioContext::kSampleRate,
                                                 session.tempoBpm.load (std::memory_order_acquire));
        region.notes.push_back ({ 1, 48, 100, 0, region.lengthInTicks });
        regions->push_back (std::move (region));
        track.midiRegions.publish (std::move (regions));
        ctx.cleanup ([&track] { track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>()); });

        transport.setPlayhead (0);
        engine.play();
        ctx.pump (kSettleBlocks);
        const auto playing = readCounters (ctx);
        ctx.expect ((int) playing.voicesHeld == 1, "region note: the instrument never received it");
        engine.stop();
        ctx.pump (kSettleBlocks);
        const auto stopped = readCounters (ctx);
        ctx.note ("region note: before " + describe (playing) + " / after " + describe (stopped));
        ctx.expect (stopped.voicesHeld <= 0.0, "region note: it was still held after Stop");
        ctx.expect ((int) stopped.noteOffsSeen == (int) playing.noteOffsSeen + 1,
                    "region note: Stop did not end it with a note-off");
        ctx.expect (stopped.chokesSeen > playing.chokesSeen, "region note: Stop sent no choke");
    }

    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "clap.choke_on_stop_only",
    { "clap", "midi", "panic", "transport" },
    Needs::Engine,
    { "panic_probe.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return runChoke (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native CLAP host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
