#include "../Scenario.h"
#include "MidiProbeHarness.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// A plug-in that played notes on a MIDI track and then stops being fed the
// track's MIDI - the track switched to an audio mode with the plug-in left
// loaded, or froze - never hears the note-offs for the keys still down. It must
// not keep those voices, whether it runs on without MIDI or is fed again later.
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

void keys (ScenarioContext& ctx, int input, bool down)
{
    const auto status = (std::uint8_t) (down ? 0x90 : 0x80);
    dusk::MidiBuffer events;
    midiprobe::addMessage (events, status, 60, down ? 100 : 0);
    midiprobe::addMessage (events, status, 67, down ? 100 : 0);
    ctx.pumpWithMidi (input, std::move (events));
    ctx.pump (1);
}

// The probe on a live MIDI track with two keys held. False, with the reason
// recorded, when that cannot be set up.
bool holdingTwoKeys (ScenarioContext& ctx, int input)
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
    keys (ctx, input, true);
    return ctx.expect (voicesHeld (ctx) == 2, "the instrument never received the notes");
}

// Switched to an audio mode with the plug-in still loaded and IN on, the strip
// runs the plug-in as an effect on the track's input, with no MIDI.
ScenarioResult runModeSwitch (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! holdingTwoKeys (ctx, input)) return ctx.verdict();

    ctx.session().track (kTrack).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();
    ctx.pump (1);
    keys (ctx, input, false);
    ctx.pump (kSettleBlocks);
    const int after = voicesHeld (ctx);
    ctx.note ("voices once the track is stereo: " + std::to_string (after));
    ctx.expect (after <= 0, "the plug-in kept the held notes after the track left MIDI mode");
    return ctx.verdict();
}

// Switched to mono with IN off and the transport stopped, nothing feeds the
// strip and the plug-in does not run at all. Back in MIDI mode it does, and the
// note-offs it missed must not leave it holding the keys.
ScenarioResult runModeRoundTrip (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! holdingTwoKeys (ctx, input)) return ctx.verdict();

    auto& track = ctx.session().track (kTrack);
    track.inputMonitor.store (false, std::memory_order_relaxed);
    track.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();
    ctx.pump (1);
    keys (ctx, input, false);
    ctx.pump (kSettleBlocks);

    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();
    ctx.pump (kSettleBlocks);
    const int after = voicesHeld (ctx);
    ctx.note ("voices back in MIDI mode: " + std::to_string (after));
    ctx.expect (after <= 0, "the plug-in came back to MIDI mode still holding the notes it missed");

    keys (ctx, input, true);
    const int playing = voicesHeld (ctx);
    keys (ctx, input, false);
    ctx.expect (playing == 2 && voicesHeld (ctx) <= 0, "the track no longer played normally after the round trip");
    return ctx.verdict();
}

// A frozen track's plug-in does not run; the keys come up while it is frozen.
ScenarioResult runFreeze (ScenarioContext& ctx)
{
    const int input = ctx.engine().getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! holdingTwoKeys (ctx, input)) return ctx.verdict();

    auto& frozen = ctx.session().track (kTrack).frozen;
    ctx.keep (frozen);
    frozen.store (true, std::memory_order_release);
    ctx.pump (1);
    keys (ctx, input, false);
    ctx.pump (kSettleBlocks);
    frozen.store (false, std::memory_order_release);
    ctx.pump (kSettleBlocks);
    const int after = voicesHeld (ctx);
    ctx.note ("voices once unfrozen: " + std::to_string (after));
    ctx.expect (after <= 0, "the plug-in came out of the freeze still holding the notes it missed");
    return ctx.verdict();
}
#else
ScenarioResult withoutClap (ScenarioContext&)
{
    return ScenarioResult::skip ("built without the native CLAP host");
}
constexpr Run runModeSwitch = withoutClap, runModeRoundTrip = withoutClap, runFreeze = withoutClap;
#endif

const std::vector<std::string> kTags { "midi", "panic", "clap" };
const std::vector<std::string> kFixtures { "panic_probe.clap" };

std::function<std::optional<ScenarioResult> (ScenarioContext&)> running (Run run)
{
    return [run] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return run (ctx); };
}

const ScenarioRegistrar switchRegistrar { Scenario {
    "midi.release_on_mode_switch", kTags, Needs::Engine, kFixtures, running (runModeSwitch) } };
const ScenarioRegistrar roundTripRegistrar { Scenario {
    "midi.release_on_mode_round_trip", kTags, Needs::Engine, kFixtures, running (runModeRoundTrip) } };
const ScenarioRegistrar freezeRegistrar { Scenario {
    "midi.release_on_unfreeze", kTags, Needs::Engine, kFixtures, running (runFreeze) } };
} // namespace
} // namespace duskstudio::scenario
