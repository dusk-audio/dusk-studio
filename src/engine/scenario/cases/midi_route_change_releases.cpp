#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"
#include "../../../session/TrackMove.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A key held while the track stops listening to where it comes from: its
// note-off arrives from a source, or on a channel, the track no longer takes.
// The instrument must not be left holding the note.
namespace duskstudio::scenario
{
namespace
{
using Run = ScenarioResult (*) (ScenarioContext&);

#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;

// The probe publishes whole-number counts as parameters.
struct Counters
{
    int voicesHeld   = 0;
    int chokesSeen   = 0;
    int noteOffsSeen = 0;
};

Counters readCounters (ScenarioContext& ctx, int track)
{
    return { (int) midiprobe::counter (ctx, track, "voicesHeld"),
             (int) midiprobe::counter (ctx, track, "chokesSeen"),
             (int) midiprobe::counter (ctx, track, "noteOffsSeen") };
}

std::string describe (const Counters& c)
{
    return "voicesHeld=" + std::to_string (c.voicesHeld)
         + " chokesSeen=" + std::to_string (c.chokesSeen)
         + " noteOffsSeen=" + std::to_string (c.noteOffsSeen);
}

// Two keys going down, or up, on one channel of an input, then a block for the
// instrument to hear them.
void keys (ScenarioContext& ctx, int input, bool down, int channel)
{
    const auto status = (std::uint8_t) ((down ? 0x90 : 0x80) | (channel - 1));
    dusk::MidiBuffer events;
    midiprobe::addMessage (events, status, 60, down ? 100 : 0);
    midiprobe::addMessage (events, status, 67, down ? 100 : 0);
    ctx.pumpWithMidi (input, std::move (events));
    ctx.pump (1);
}

// The probe on a live MIDI track that listens to the on-screen keyboard's
// input, the one input a scenario can feed. False with the reason recorded.
bool setUpProbeTrack (ScenarioContext& ctx, int input)
{
    midiprobe::makeLiveMidiTrack (ctx, kTrack, input);
    ctx.session().recomputeRtCounters();
    std::string error;
    if (! ctx.expect (midiprobe::loadPanicProbe (ctx, kTrack, error), "the track could not load the probe"))
    {
        ctx.note ("load error: " + error);
        return false;
    }
    ctx.pump (kSettleBlocks);
    return true;
}

// The keys go down, the route changes, then the keys come up on the input and
// channel they went down on. Whatever became of those note-offs, the
// instrument must end up holding nothing.
void holdAcross (ScenarioContext& ctx, int input, int channel, const std::string& change,
                 const std::function<void()>& apply)
{
    keys (ctx, input, true, channel);
    const auto held = readCounters (ctx, kTrack);
    if (! ctx.expect (held.voicesHeld > 0, change + ": the instrument never received the notes"))
        return;

    apply();
    ctx.session().recomputeRtCounters();
    ctx.pump (1);
    keys (ctx, input, false, channel);

    const auto after = readCounters (ctx, kTrack);
    ctx.note (change + ": held " + describe (held) + " / after release " + describe (after));
    ctx.expect (after.voicesHeld <= 0, change + " with keys down left the instrument holding them");
}

ScenarioResult runInputChange (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUpProbeTrack (ctx, input)) return ctx.verdict();

    holdAcross (ctx, input, 1, "choosing no MIDI input", [&]
    {
        ctx.session().track (kTrack).midiInputIndex.store (-1);
    });
    return ctx.verdict();
}

ScenarioResult runMonitorOff (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUpProbeTrack (ctx, input)) return ctx.verdict();

    // Stopped, a MIDI track hears its input whatever IN says; rolling, only
    // with IN on.
    auto& engine = ctx.engine();
    engine.play();
    ctx.pump (kSettleBlocks);
    holdAcross (ctx, input, 1, "turning IN off while the transport rolls", [&]
    {
        ctx.session().track (kTrack).inputMonitor.store (false);
    });
    engine.stop();
    ctx.pump (kSettleBlocks);
    return ctx.verdict();
}

ScenarioResult runDisarm (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUpProbeTrack (ctx, input)) return ctx.verdict();

    // No input of its own and IN off: the track plays the on-screen keyboard
    // only because it is armed.
    auto& track = ctx.session().track (kTrack);
    track.midiInputIndex.store (-1);
    track.inputMonitor.store (false);
    track.recordArmed.store (true);
    ctx.session().recomputeRtCounters();
    ctx.pump (kSettleBlocks);

    holdAcross (ctx, input, 1, "disarming a track that plays the on-screen keyboard", [&]
    {
        track.recordArmed.store (false);
    });
    return ctx.verdict();
}

ScenarioResult runChannelFilter (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUpProbeTrack (ctx, input)) return ctx.verdict();

    auto& channel = ctx.session().track (kTrack).midiChannel;
    holdAcross (ctx, input, 1, "moving the MIDI channel off the held notes' channel", [&]
    {
        channel.store (2);
    });

    // Opening the filter up loses nothing, so a held note waits for its own
    // note-off rather than being cut by a reset.
    ctx.pump (kSettleBlocks);
    keys (ctx, input, true, 2);
    const auto held = readCounters (ctx, kTrack);
    channel.store (0);
    ctx.pump (2);
    const auto widened = readCounters (ctx, kTrack);
    keys (ctx, input, false, 2);
    const auto released = readCounters (ctx, kTrack);
    ctx.note ("widened to every channel: " + describe (held) + " / " + describe (widened)
              + " / " + describe (released));
    ctx.expect (held.voicesHeld > 0 && widened.voicesHeld == held.voicesHeld
                    && widened.chokesSeen == held.chokesSeen,
                "opening the channel filter reset notes the track still hears");
    ctx.expect (released.voicesHeld <= 0 && released.noteOffsSeen > widened.noteOffsSeen,
                "the note held across the wider filter did not end on its own note-off");
    return ctx.verdict();
}

std::array<int, Session::kNumTracks> firstTwoSwapped()
{
    std::array<int, Session::kNumTracks> order {};
    for (int t = 0; t < Session::kNumTracks; ++t) order[(size_t) t] = t;
    order[0] = 1;
    order[1] = 0;
    return order;
}

ScenarioResult runTrackMove (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! setUpProbeTrack (ctx, input)) return ctx.verdict();

    // The moving track hears channel 1 only and its neighbour every channel, so
    // a moved track judged against the route its new slot had before would
    // read as narrowed and be reset instead of hearing its note-offs.
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    session.track (kTrack).midiChannel.store (1);
    midiprobe::makeLiveMidiTrack (ctx, kTrack + 1, input);
    session.recomputeRtCounters();
    ctx.pump (kSettleBlocks);

    const auto* moving = &engine.getChannelStrip (kTrack);
    ctx.cleanup ([&engine, moving]
    {
        engine.getTransport().setState (Transport::State::Stopped);
        if (&engine.getChannelStrip (kTrack) != moving)
            if (const auto back = trackMoveFromNewToOld (firstTwoSwapped()))
                engine.moveTracks (*back);
    });

    keys (ctx, input, true, 1);
    const auto held = readCounters (ctx, kTrack);
    if (! ctx.expect (held.voicesHeld > 0, "the instrument never received the notes"))
        return ctx.verdict();

    const auto plan = trackMoveFromNewToOld (firstTwoSwapped());
    if (! ctx.expect (plan && engine.moveTracks (*plan), "the move was refused")
        || ! ctx.expect (&engine.getChannelStrip (kTrack + 1) == moving, "the strip did not move with its track"))
        return ctx.verdict();

    ctx.pump (1);
    keys (ctx, input, false, 1);
    const auto after = readCounters (ctx, kTrack + 1);
    ctx.note ("moved: held " + describe (held) + " / after release " + describe (after));
    ctx.expect (after.voicesHeld <= 0, "the moved track's instrument kept the notes");
    ctx.expect (after.chokesSeen == held.chokesSeen && after.noteOffsSeen > held.noteOffsSeen,
                "the moved track was reset rather than hearing its own note-offs");
    return ctx.verdict();
}
#else
ScenarioResult withoutClap (ScenarioContext&)
{
    return ScenarioResult::skip ("built without the native CLAP host");
}
constexpr Run runInputChange = withoutClap, runMonitorOff = withoutClap, runDisarm = withoutClap,
              runChannelFilter = withoutClap, runTrackMove = withoutClap;
#endif

const std::vector<std::string> kTags { "midi", "panic", "clap" };
const std::vector<std::string> kFixtures { "panic_probe.clap" };

std::function<std::optional<ScenarioResult> (ScenarioContext&)> running (Run run)
{
    return [run] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return run (ctx); };
}

const ScenarioRegistrar inputRegistrar { Scenario {
    "midi.release_on_input_change", kTags, Needs::Engine, kFixtures, running (runInputChange) } };
const ScenarioRegistrar monitorRegistrar { Scenario {
    "midi.release_on_monitor_off", kTags, Needs::Engine, kFixtures, running (runMonitorOff) } };
const ScenarioRegistrar disarmRegistrar { Scenario {
    "midi.release_on_disarm", kTags, Needs::Engine, kFixtures, running (runDisarm) } };
const ScenarioRegistrar channelRegistrar { Scenario {
    "midi.release_on_channel_filter", kTags, Needs::Engine, kFixtures, running (runChannelFilter) } };
const ScenarioRegistrar moveRegistrar { Scenario {
    "midi.held_note_follows_track_move", kTags, Needs::Engine, kFixtures, running (runTrackMove) } };
} // namespace
} // namespace duskstudio::scenario
