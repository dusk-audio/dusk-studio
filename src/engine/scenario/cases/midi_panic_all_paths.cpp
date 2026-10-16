#include "../Scenario.h"
#include "MidiProbeHarness.h"

#include <string>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
// One instrument on each of the three routing states a panic has to reach.
// kSoloSource carries no plugin: its solo is what closes the gate on kSoloedOut,
// and kOpenTrack is soloed alongside it so one track stays audible.
constexpr int kOpenTrack   = 0;
constexpr int kMutedTrack  = 1;
constexpr int kSoloedOut   = 2;
constexpr int kSoloSource  = 3;
constexpr int kSettleBlocks = 3;
// The choke lands on the first block after the stop; what takes longer is the
// strip draining the DC the released voice left behind.
constexpr int kSilenceBlocks = 256;

const int kProbeTracks[] = { kOpenTrack, kMutedTrack, kSoloedOut };

const char* trackLabel (int track)
{
    if (track == kOpenTrack)  return "open";
    if (track == kMutedTrack) return "muted";
    return "soloed-out";
}

void noteCounters (ScenarioContext& ctx, const char* when)
{
    for (const int t : kProbeTracks)
        ctx.note (std::string (when) + " " + trackLabel (t)
                  + ": voicesHeld=" + std::to_string ((int) midiprobe::counter (ctx, t, "voicesHeld"))
                  + " chokesSeen=" + std::to_string ((int) midiprobe::counter (ctx, t, "chokesSeen"))
                  + " noteOffsSeen=" + std::to_string ((int) midiprobe::counter (ctx, t, "noteOffsSeen"))
                  + " outLDb=" + std::to_string (ctx.engine().getChannelStrip (t).getOutLDb()));
}

ScenarioResult runPanic (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();

    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0)
        return ScenarioResult::skip ("the MIDI input bank has no injectable input");

    for (const int t : { kOpenTrack, kMutedTrack, kSoloedOut, kSoloSource })
        midiprobe::makeLiveMidiTrack (ctx, t, input);
    session.track (kMutedTrack).strip.mute.store (true, std::memory_order_relaxed);
    session.track (kOpenTrack).strip.solo.store (true, std::memory_order_relaxed);
    session.track (kSoloSource).strip.solo.store (true, std::memory_order_relaxed);
    session.recomputeRtCounters();

    for (const int t : kProbeTracks)
    {
        std::string error;
        if (! ctx.expect (midiprobe::loadPanicProbe (ctx, t, error),
                          std::string ("the ") + trackLabel (t)
                              + " track could not load the probe"))
        {
            ctx.note ("load error: " + error);
            return ctx.verdict();
        }
    }

    ctx.cleanup ([&engine]
    {
        for (const int t : kProbeTracks)
            engine.setTrackMidiCapacityForTest (t, dusk::kMidiRoutingBlockBytes);
        engine.stop();
    });
    for (const bool overflow : { false, true })
    {
        ctx.note (overflow ? "Stop with overflowing live MIDI" : "Stop with ordinary live MIDI");
        engine.play();
        ctx.pump (kSettleBlocks);

        dusk::MidiBuffer notes;
        midiprobe::addMessage (notes, 0x90, 60, 100);
        midiprobe::addMessage (notes, 0x90, 67, 100);
        for (int channel = 0; channel < 16; ++channel)
        {
            midiprobe::addMessage (notes, (std::uint8_t) (0xE0 | channel), 127, 127);
            const std::uint8_t pressure[] { (std::uint8_t) (0xD0 | channel), 90 };
            notes.addEvent (pressure, 2, 0);
        }
        ctx.pumpWithMidi (input, std::move (notes));
        ctx.pump (1);

        noteCounters (ctx, "held");
        for (const int t : kProbeTracks)
        {
            ctx.expect (midiprobe::counter (ctx, t, "voicesHeld") > 0.0,
                        std::string ("the ") + trackLabel (t) + " track's instrument never received the notes");
            ctx.expect ((int) midiprobe::counter (ctx, t, "bentChannels") == 65535
                        && (int) midiprobe::counter (ctx, t, "pressedChannels") == 65535,
                        std::string ("expression never reached every channel on the ") + trackLabel (t) + " track");
        }
        ctx.expect (engine.getChannelStrip (kOpenTrack).getOutLDb() > -60.0f,
                    "the audible track produced no output while holding voices");

        if (overflow)
            for (const int t : kProbeTracks) engine.setTrackMidiCapacityForTest (t, 1536);
        engine.stop();
        if (overflow)
        {
            dusk::MidiBuffer burst;
            for (int i = 0; i < 200; ++i)
                midiprobe::addMessage (burst, 0xB0, 1, (std::uint8_t) (i & 127));
            ctx.pumpWithMidi (input, std::move (burst));
        }
        ctx.pump (kSettleBlocks);

        noteCounters (ctx, "after stop");
        for (const int t : kProbeTracks)
        {
            ctx.expect (midiprobe::counter (ctx, t, "voicesHeld") <= 0.0,
                        std::string ("the ") + trackLabel (t) + " track kept its voices through the panic");
            ctx.expect (midiprobe::counter (ctx, t, "chokesSeen") >= 1.0,
                        std::string ("the panic never reached the ") + trackLabel (t) + " track");
            ctx.expect ((int) midiprobe::counter (ctx, t, "bentChannels") == 0
                        && (int) midiprobe::counter (ctx, t, "pressedChannels") == 0,
                        std::string ("Stop left expression active on the ") + trackLabel (t) + " track");
            for (const auto* name : { "cc121Seen", "pitchCentersSeen", "pressureZerosSeen" })
                ctx.expect (midiprobe::counter (ctx, t, name) >= (overflow ? 32.0 : 16.0),
                            std::string ("Stop omitted ") + name + " on the " + trackLabel (t) + " track");
            engine.setTrackMidiCapacityForTest (t, dusk::kMidiRoutingBlockBytes);
        }
        const int silentAfter = midiprobe::pumpUntilSilent (ctx, kOpenTrack, kSilenceBlocks);
        ctx.note ("the audible track fell silent " + std::to_string (silentAfter) + " blocks after the panic");
        ctx.expect (silentAfter >= 0, "the audible track was still producing output after the panic");
    }

    return ctx.verdict();
}
#endif

const ScenarioRegistrar registrar { Scenario {
    "midi.panic_all_paths",
    { "midi", "panic", "clap" },
    Needs::Engine,
    { "panic_probe.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return runPanic (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native CLAP host");
       #endif
    }
} };
} // namespace
} // namespace duskstudio::scenario
