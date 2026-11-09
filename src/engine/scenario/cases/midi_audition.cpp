#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../../session/TrackMove.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The piano roll keyboard's notes through the real callback: they wait out a
// suspended engine, a note-off queued or held across a track move still
// reaches the strip that heard its note-on, and the keyboard opens a track to
// its own notes only.
namespace duskstudio::scenario
{
namespace
{
using Run = ScenarioResult (*) (ScenarioContext&);

#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kSettleBlocks = 3;

struct Heard
{
    int ons = 0, offs = 0, voices = 0, chokes = 0, onKey = 0;
};

Heard heardOn (ScenarioContext& ctx, int track)
{
    return { (int) midiprobe::counter (ctx, track, "noteOnsSeen"), (int) midiprobe::counter (ctx, track, "noteOffsSeen"),
             (int) midiprobe::counter (ctx, track, "voicesHeld"), (int) midiprobe::counter (ctx, track, "chokesSeen"),
             (int) midiprobe::counter (ctx, track, "lastNoteOnKey") };
}

std::string describe (const Heard& h)
{
    return "ons=" + std::to_string (h.ons) + " offs=" + std::to_string (h.offs) + " voices=" + std::to_string (h.voices)
         + " chokes=" + std::to_string (h.chokes) + " lastOnKey=" + std::to_string (h.onKey);
}

// A MIDI track with no live input at all, the instrument the probe.
bool probeTrack (ScenarioContext& ctx, int track)
{
    auto& t = ctx.session().track (track);
    t.mode.store ((int) Track::Mode::Midi);
    t.inputMonitor.store (false);
    t.midiInputIndex.store (-1);
    ctx.session().recomputeRtCounters();
    std::string error;
    if (ctx.expect (midiprobe::loadPanicProbe (ctx, track, error), "track " + std::to_string (track + 1) + " could not load the probe"))
        return true;
    ctx.note ("load error: " + error);
    return false;
}

ScenarioResult runSuspended (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! probeTrack (ctx, 0)) return ctx.verdict();
    ctx.pump (kSettleBlocks);
    ctx.cleanup ([&engine] { engine.releaseAuditions(); engine.resumeProcessing(); });

    engine.suspendProcessing();
    ctx.expect (engine.auditionNoteOn (0, 1, 60, 100), "the note was not taken while processing was suspended");
    ctx.pump (2);
    ctx.expect (heardOn (ctx, 0).ons == 0, "a suspended engine played the note");
    engine.resumeProcessing();
    ctx.pump (1);
    const auto on = heardOn (ctx, 0);
    ctx.expect (on.ons == 1 && on.voices == 1 && on.onKey == 60, "the note waiting out the suspension never played: " + describe (on));

    engine.suspendProcessing();
    engine.auditionNoteOff (0, 1, 60);
    ctx.pump (2);
    ctx.expect (heardOn (ctx, 0).voices == 1, "a suspended engine ended the note");
    engine.resumeProcessing();
    ctx.pump (1);
    const auto off = heardOn (ctx, 0);
    ctx.expect (off.offs == 1 && off.voices == 0, "the note-off queued while suspended was lost: " + describe (off));

    ctx.expect (engine.auditionNoteOn (0, 1, 67, 100), "the third note was not taken");
    ctx.pump (1);
    engine.releaseAuditions();
    ctx.expect (! engine.isAuditionHeld (0, 1, 67), "releasing the auditions left the note held");
    ctx.pump (1);
    const auto released = heardOn (ctx, 0);
    ctx.expect (released.offs == 2 && released.voices == 0 && released.chokes == off.chokes,
                "releasing the auditions did not end the held note with its own note-off: " + describe (released));
    return ctx.verdict();
}

ScenarioResult runTrackMove (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! probeTrack (ctx, 0) || ! probeTrack (ctx, 1)) return ctx.verdict();
    ctx.pump (kSettleBlocks);

    std::array<int, Session::kNumTracks> swapped {};
    for (int t = 0; t < Session::kNumTracks; ++t) swapped[(std::size_t) t] = t;
    swapped[0] = 1;
    swapped[1] = 0;
    const auto plan = trackMoveFromNewToOld (swapped);
    const auto* moving = &engine.getChannelStrip (0);
    ctx.cleanup ([&engine, moving, swapped]
    {
        engine.releaseAuditions();
        if (&engine.getChannelStrip (0) != moving)
            if (const auto back = trackMoveFromNewToOld (swapped))
                engine.moveTracks (*back);
    });

    ctx.expect (engine.auditionNoteOn (0, 1, 60, 100), "the first note was not taken");
    ctx.pump (1);
    const auto held = heardOn (ctx, 0);
    if (! ctx.expect (held.voices == 1, "the track never heard the first note: " + describe (held))) return ctx.verdict();

    // Glide on, with the callback not yet run: the first note's note-off and
    // the next note-on are still queued when the move lands.
    engine.auditionNoteOff (0, 1, 60);
    ctx.expect (engine.auditionNoteOn (0, 1, 64, 100), "the second note was not taken");
    if (! ctx.expect (plan && engine.moveTracks (*plan), "the move was refused")
        || ! ctx.expect (&engine.getChannelStrip (1) == moving, "the strip did not move with its track"))
        return ctx.verdict();
    ctx.expect (engine.isAuditionHeld (1, 1, 64) && ! engine.isAuditionHeld (0, 1, 64),
                "the held note did not move with its track");
    ctx.pump (1);

    const auto moved = heardOn (ctx, 1);
    const auto other = heardOn (ctx, 0);
    ctx.note ("moved strip " + describe (moved) + " / strip now in its slot " + describe (other));
    ctx.expect (moved.ons == 2 && moved.offs == 1 && moved.voices == 1 && moved.onKey == 64 && moved.chokes == held.chokes,
                "the moved strip did not hear the queued note-off and note-on as its own");
    ctx.expect (other.ons == 0 && other.offs == 0, "the strip that took the slot heard the moved track's notes");

    engine.auditionNoteOff (1, 1, 64);
    ctx.pump (1);
    const auto released = heardOn (ctx, 1);
    ctx.expect (released.offs == 2 && released.voices == 0, "the moved strip kept the note: " + describe (released));

    if (const auto back = trackMoveFromNewToOld (swapped))
        ctx.expect (engine.moveTracks (*back), "the tracks did not move back");
    return ctx.verdict();
}
// The keyboard opening a stopped, IN-off track must not open it to the live
// input as well: a key already held on the input is reset ahead of the
// keyboard's note, and one played meanwhile never reaches the instrument.
ScenarioResult runLiveInputStaysShut (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    if (! probeTrack (ctx, 0)) return ctx.verdict();
    ctx.session().track (0).midiInputIndex.store (input);
    // Long enough for a gate an earlier case left ringing to close.
    ctx.pump ((int) (ScenarioContext::kSampleRate * (midi::AuditionGate::kMaxRingSeconds + 0.1))
              / ScenarioContext::kBlockSize);
    ctx.cleanup ([&engine] { engine.releaseAuditions(); });

    dusk::MidiBuffer chord;
    midiprobe::addMessage (chord, 0x90, 48, 100);
    ctx.pumpWithMidi (input, std::move (chord));
    ctx.pump (1);
    const auto silent = heardOn (ctx, 0);
    ctx.expect (silent.voices == 1 && silent.onKey == 48, "the IN-off track did not take its live input: " + describe (silent));
    ctx.expect (engine.getChannelStrip (0).getOutLDb() <= midiprobe::kSilentDb, "the IN-off track was heard before the keyboard played");

    ctx.expect (engine.auditionNoteOn (0, 1, 60, 100), "the keyboard's note was not taken");
    ctx.pump (1);
    const auto playing = heardOn (ctx, 0);
    ctx.expect (playing.voices == 1 && playing.onKey == 60,
                "the live key held on the input was not reset ahead of the keyboard's note: " + describe (playing));

    dusk::MidiBuffer live;
    midiprobe::addMessage (live, 0x90, 72, 100);
    ctx.pumpWithMidi (input, std::move (live));
    ctx.pump (1);
    const auto after = heardOn (ctx, 0);
    ctx.note ("held on the input " + describe (silent) + " / keyboard " + describe (playing) + " / live key " + describe (after));
    ctx.expect (after.ons == playing.ons && after.voices == 1 && after.onKey == 60,
                "a live key played while only the keyboard opened the strip reached the instrument");

    engine.auditionNoteOff (0, 1, 60);
    ctx.pump (1);
    ctx.expect (heardOn (ctx, 0).voices == 0, "the keyboard's note did not end");
    return ctx.verdict();
}

// Nothing piles up for later while the callback takes nothing in: a second
// note-on waits for a block to have run, and none is taken in Mastering,
// whose blocks never reach the tracks. Note-offs are always taken.
ScenarioResult runPacedNoteOns (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    if (! probeTrack (ctx, 0)) return ctx.verdict();
    ctx.pump (kSettleBlocks);
    const auto stage = engine.getStage();
    ctx.cleanup ([&engine, stage] { engine.releaseAuditions(); engine.setStage (stage); });

    ctx.expect (engine.auditionNoteOn (0, 1, 60, 100), "the first note was not taken");
    engine.auditionNoteOff (0, 1, 60);
    ctx.expect (! engine.auditionNoteOn (0, 1, 62, 100), "a second note-on was taken before any block ran");
    ctx.pump (1);
    ctx.expect (engine.auditionNoteOn (0, 1, 62, 100), "the next note-on was refused after a block had run");
    ctx.pump (1);
    const auto played = heardOn (ctx, 0);
    ctx.expect (played.ons == 2 && played.offs == 1 && played.voices == 1 && played.onKey == 62,
                "the paced notes did not play in turn: " + describe (played));

    engine.setStage (AudioEngine::Stage::Mastering);
    ctx.expect (! engine.auditionNoteOn (0, 1, 64, 100), "a note-on was taken in the Mastering stage");
    engine.auditionNoteOff (0, 1, 62);
    ctx.expect (! engine.isAuditionHeld (0, 1, 62), "the note-off was refused in the Mastering stage");
    ctx.pump (3);
    ctx.expect (heardOn (ctx, 0).offs == 1, "a Mastering block reached the track");
    engine.setStage (stage);
    ctx.pump (1);
    const auto back = heardOn (ctx, 0);
    ctx.note ("played " + describe (played) + " / back from Mastering " + describe (back));
    ctx.expect (back.ons == 2 && back.offs == 2 && back.voices == 0,
                "leaving Mastering did not deliver just the waiting note-off");
    return ctx.verdict();
}

// A held key ends at Stop and, during playback, at a loop seam, the way a key
// held on the track's MIDI input does.
ScenarioResult runEndsAtStopAndSeam (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    if (! probeTrack (ctx, 0)) return ctx.verdict();
    ctx.pump (kSettleBlocks);
    ctx.cleanup ([&engine, &transport]
    {
        engine.releaseAuditions();
        engine.stop();
        transport.setLoopEnabled (false);
    });

    engine.play();
    ctx.pump (kSettleBlocks);
    ctx.expect (engine.auditionNoteOn (0, 1, 60, 100), "the note was not taken during playback");
    ctx.pump (1);
    const auto rolling = heardOn (ctx, 0);
    ctx.expect (rolling.voices == 1, "the note never played during playback: " + describe (rolling));
    engine.stop();
    ctx.pump (kSettleBlocks);
    const auto stopped = heardOn (ctx, 0);
    ctx.expect (stopped.voices == 0 && stopped.chokes > rolling.chokes, "Stop left the held key sounding: " + describe (stopped));
    engine.auditionNoteOff (0, 1, 60);
    ctx.pump (1);

    static constexpr std::int64_t loopStart = 500, loopEnd = 3000;
    transport.setLoopRange (loopStart, loopEnd);
    transport.setLoopEnabled (true);
    transport.setPlayhead (loopStart);
    engine.play();
    ctx.pump (kSettleBlocks);
    ctx.expect (engine.auditionNoteOn (0, 1, 62, 100), "the note was not taken in the loop");
    ctx.pump (1);
    const auto looping = heardOn (ctx, 0);
    ctx.expect (looping.voices == 1, "the note never played in the loop: " + describe (looping));
    ctx.pump ((int) ((loopEnd - loopStart) / ScenarioContext::kBlockSize) + 2);
    const auto wrapped = heardOn (ctx, 0);
    ctx.note ("rolling " + describe (rolling) + " / stopped " + describe (stopped)
              + " / looping " + describe (looping) + " / wrapped " + describe (wrapped));
    ctx.expect (wrapped.voices == 0 && wrapped.chokes > looping.chokes, "the loop seam left the held key sounding");
    engine.auditionNoteOff (0, 1, 62);
    ctx.pump (1);
    return ctx.verdict();
}
#else
ScenarioResult withoutClap (ScenarioContext&)
{
    return ScenarioResult::skip ("built without the native CLAP host");
}
constexpr Run runSuspended = withoutClap, runTrackMove = withoutClap, runLiveInputStaysShut = withoutClap,
              runPacedNoteOns = withoutClap, runEndsAtStopAndSeam = withoutClap;
#endif

const std::vector<std::string> kTags { "midi", "audition", "clap" };
const std::vector<std::string> kFixtures { "panic_probe.clap" };

std::function<std::optional<ScenarioResult> (ScenarioContext&)> running (Run run)
{
    return [run] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return run (ctx); };
}

const ScenarioRegistrar suspendedRegistrar { Scenario {
    "midi.audition_waits_out_a_suspended_engine", kTags, Needs::Engine, kFixtures, running (runSuspended) } };
const ScenarioRegistrar moveRegistrar { Scenario {
    "midi.audition_note_off_follows_track_move", kTags, Needs::Engine, kFixtures, running (runTrackMove) } };
const ScenarioRegistrar liveInputRegistrar { Scenario {
    "midi.audition_keeps_live_input_shut_with_in_off", kTags, Needs::Engine, kFixtures, running (runLiveInputStaysShut) } };
const ScenarioRegistrar pacedRegistrar { Scenario {
    "midi.audition_note_ons_wait_for_the_callback", kTags, Needs::Engine, kFixtures, running (runPacedNoteOns) } };
const ScenarioRegistrar stopSeamRegistrar { Scenario {
    "midi.audition_ends_at_stop_and_loop_seam", kTags, Needs::Engine, kFixtures, running (runEndsAtStopAndSeam) } };
} // namespace
} // namespace duskstudio::scenario
