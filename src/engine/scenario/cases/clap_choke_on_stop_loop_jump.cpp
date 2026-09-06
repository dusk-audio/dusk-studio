#include "../Scenario.h"
#include "MidiProbeHarness.h"

#include <cstdint>
#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrackIndex   = 0;
constexpr int kSettleBlocks = 3;
// Deliberately not a multiple of the block size: the transport wraps the
// playhead at a block boundary, so a block-aligned loop never puts the seam
// inside a block where the scheduler can emit its reset.
constexpr std::int64_t kLoopStart = 500;
constexpr std::int64_t kLoopEnd   = 3000;
constexpr std::int64_t kJumpTarget = 48000 * 10;

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

void checkChoked (ScenarioContext& ctx, const char* event,
                  const Counters& before, const Counters& after)
{
    const std::string prefix = std::string (event) + ": ";
    ctx.note (prefix + "before " + describe (before) + " / after " + describe (after));
    ctx.expect (after.voicesHeld == 0.0, prefix + "voices were still held afterwards");
    ctx.expect (after.chokesSeen > before.chokesSeen, prefix + "no choke reached the plugin");
    ctx.expect (after.noteOffsSeen == before.noteOffsSeen,
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
    }

    {
        transport.setLoopRange (kLoopStart, kLoopEnd);
        transport.setLoopEnabled (true);
        transport.setPlayhead (kLoopStart);
        engine.play();
        ctx.pump (kSettleBlocks);
        const auto held = holdVoices (ctx, input);
        ctx.expect (held.voicesHeld > 0.0, "loop wrap: the instrument never received the notes");

        // Enough blocks to cross the loop end from anywhere inside it.
        ctx.pump ((int) ((kLoopEnd - kLoopStart) / ScenarioContext::kBlockSize) + 2);
        ctx.note ("loop wrap: playhead landed at "
                  + std::to_string ((long long) transport.getPlayhead()));
        checkChoked (ctx, "loop wrap", held, readCounters (ctx));

        engine.stop();
        transport.setLoopEnabled (false);
        ctx.pump (kSettleBlocks);
    }

    {
        transport.setPlayhead (0);
        engine.play();
        ctx.pump (kSettleBlocks);
        const auto held = holdVoices (ctx, input);
        ctx.expect (held.voicesHeld > 0.0, "playhead jump: the instrument never received the notes");

        transport.setPlayhead (kJumpTarget);
        ctx.pump (kSettleBlocks);
        checkChoked (ctx, "playhead jump", held, readCounters (ctx));

        engine.stop();
        ctx.pump (1);
    }

    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "clap.choke_on_stop_loop_jump",
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
