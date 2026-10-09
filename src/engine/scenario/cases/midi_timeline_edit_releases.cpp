#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"
#include "../../../session/RegionEditActions.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A note the timeline started must get its note-off however the timeline stops
// playing it: its region ends under it, or an edit made while it sounds takes
// the note, or its end, away from where the transport is.
namespace duskstudio::scenario
{
namespace
{
using Run = ScenarioResult (*) (ScenarioContext&);

#if DUSKSTUDIO_HAS_NATIVE_CLAP
constexpr int kTrack = 0;
constexpr int kSettleBlocks = 3;
constexpr float kBpm = 120.0f;   // 50 samples a tick
constexpr int kKey = 60;
constexpr int kOtherKey = 67;
constexpr std::int64_t kLongTicks = 4000;
constexpr std::int64_t kNoteTicks = 3000;
constexpr std::int64_t kInsideTick = 1000;   // where the edits land, inside the note

// The CLAP host hands the hanging reset to the probe as note chokes.
struct Counters
{
    int voicesHeld   = 0;
    int chokesSeen   = 0;
    int noteOffsSeen = 0;
};

Counters readCounters (ScenarioContext& ctx)
{
    return { (int) midiprobe::counter (ctx, kTrack, "voicesHeld"),
             (int) midiprobe::counter (ctx, kTrack, "chokesSeen"),
             (int) midiprobe::counter (ctx, kTrack, "noteOffsSeen") };
}

std::string describe (const Counters& c)
{
    return "voicesHeld=" + std::to_string (c.voicesHeld) + " chokesSeen=" + std::to_string (c.chokesSeen)
         + " noteOffsSeen=" + std::to_string (c.noteOffsSeen);
}

std::int64_t samplesOf (std::int64_t ticks)
{
    return ticksToSamples (ticks, ScenarioContext::kSampleRate, kBpm);
}

MidiRegion regionOf (std::int64_t startTick, std::int64_t lengthTicks)
{
    MidiRegion region;
    region.timelineStart = samplesOf (startTick);
    region.lengthInTicks = lengthTicks;
    region.lengthInSamples = samplesOf (lengthTicks);
    region.recordedAtBPM = kBpm;
    return region;
}

MidiNote noteOf (int key, std::int64_t startTick, std::int64_t lengthTicks)
{
    return { 1, key, 100, startTick, lengthTicks };
}

// One region holding one long note on kKey.
MidiRegion longNoteRegion()
{
    auto region = regionOf (0, kLongTicks);
    region.notes.push_back (noteOf (kKey, 0, kNoteTicks));
    return region;
}

auto& regionsOf (ScenarioContext& ctx) { return ctx.session().track (kTrack).midiRegions; }

// The probe on a MIDI track at kBpm playing `regions`, stopped, history clear.
bool setUp (ScenarioContext& ctx, std::vector<MidiRegion> regions)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    track.inputMonitor.store (false, std::memory_order_relaxed);
    track.midiInputIndex.store (-1, std::memory_order_relaxed);
    ctx.keep (session.tempoBpm);
    session.tempoBpm.store (kBpm, std::memory_order_release);
    session.recomputeRtCounters();
    std::string error;
    if (! ctx.expect (midiprobe::loadPanicProbe (ctx, kTrack, error), "the track could not load the probe"))
    {
        ctx.note ("load error: " + error);
        return false;
    }
    ctx.cleanup ([&engine, &track]
    {
        engine.stop();
        engine.getUndoManager().clearUndoHistory();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    });
    engine.getUndoManager().clearUndoHistory();
    regionsOf (ctx).publish (std::make_unique<std::vector<MidiRegion>> (std::move (regions)));
    ctx.pump (kSettleBlocks);
    return true;
}

// Rolls until the transport has played past `tick` (at kBpm).
void rollPast (ScenarioContext& ctx, std::int64_t tick)
{
    const auto target = samplesOf (tick);
    auto& transport = ctx.engine().getTransport();
    for (int block = 0; transport.getPlayhead() <= target && block < 100000; ++block)
        ctx.pump (1);
}

void playFromStart (ScenarioContext& ctx)
{
    ctx.engine().getTransport().setPlayhead (0);
    ctx.engine().play();
}

// Plays from the start into the note, applies `edit` with it sounding, and
// expects the instrument to hold nothing two blocks later, the note ended by
// its note-off rather than a reset.
void editWhileSounding (ScenarioContext& ctx, const std::string& what, const std::function<bool()>& edit,
                        std::int64_t atTick = kInsideTick)
{
    playFromStart (ctx);
    rollPast (ctx, atTick);
    const auto sounding = readCounters (ctx);
    if (! ctx.expect (sounding.voicesHeld == 1, what + ": the note never reached the instrument"))
        return;
    if (! ctx.expect (edit(), what + ": the edit was refused"))
        return;
    ctx.pump (2);
    const auto after = readCounters (ctx);
    ctx.note (what + ": sounding " + describe (sounding) + " / after " + describe (after));
    ctx.expect (after.voicesHeld <= 0, what + " while the note sounded left it held");
    ctx.expect (after.chokesSeen == sounding.chokesSeen && after.noteOffsSeen > sounding.noteOffsSeen,
                what + " ended the note with a reset instead of its note-off");
    ctx.engine().stop();
    ctx.pump (kSettleBlocks);
}

bool performEdit (ScenarioContext& ctx, UndoableAction* action)
{
    auto& undo = ctx.engine().getUndoManager();
    undo.beginNewTransaction();
    return undo.perform (action);
}

bool editRegion (ScenarioContext& ctx, const std::function<void (MidiRegion&)>& change, int index = 0)
{
    const auto before = regionsOf (ctx).current()[(std::size_t) index];
    auto after = before;
    change (after);
    return performEdit (ctx, new MidiRegionEditAction (ctx.session(), ctx.engine(), kTrack, index, before, after));
}

// A note that runs past its region's end stops there, by its own note-off, and
// a note that starts after the end does not play.
ScenarioResult runRegionEnd (ScenarioContext& ctx)
{
    constexpr std::int64_t kRegionTicks = 400;
    auto region = regionOf (0, kRegionTicks);
    region.notes.push_back (noteOf (kKey, 0, 1000));
    // Past the end, in the same block as the end.
    region.notes.push_back (noteOf (kOtherKey, kRegionTicks + 2, 600));
    if (! setUp (ctx, { region })) return ctx.verdict();

    playFromStart (ctx);
    rollPast (ctx, kRegionTicks / 2);
    const auto inside = readCounters (ctx);
    rollPast (ctx, kRegionTicks + 20);
    const auto past = readCounters (ctx);
    ctx.note ("inside the region " + describe (inside) + " / past its end " + describe (past));
    if (! ctx.expect (inside.voicesHeld == 1, "the note never reached the instrument"))
        return ctx.verdict();
    ctx.expect (past.voicesHeld <= 0, "a note running past its region's end was left held");
    ctx.expect (past.noteOffsSeen > inside.noteOffsSeen && past.chokesSeen == inside.chokesSeen,
                "the note did not end on its own note-off at the region's end");
    return ctx.verdict();
}

// Shortening the region under a sounding note ends it: at once when the new end
// is behind the transport, at the new end when it is still ahead.
ScenarioResult runRegionShortened (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    editWhileSounding (ctx, "shortening the region to end behind the transport", [&ctx]
    {
        return editRegion (ctx, [] (MidiRegion& r)
        {
            r.lengthInTicks = kInsideTick / 2;
            r.lengthInSamples = samplesOf (r.lengthInTicks);
        });
    });

    regionsOf (ctx).publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { longNoteRegion() }));
    playFromStart (ctx);
    rollPast (ctx, kInsideTick);
    static constexpr std::int64_t kNewEnd = kInsideTick + 500;
    ctx.expect (editRegion (ctx, [] (MidiRegion& r)
    {
        r.lengthInTicks = kNewEnd;
        r.lengthInSamples = samplesOf (r.lengthInTicks);
    }), "the edit was refused");
    ctx.pump (2);
    const auto ahead = readCounters (ctx);
    rollPast (ctx, kNewEnd + 20);
    const auto past = readCounters (ctx);
    ctx.note ("end moved ahead of the transport: " + describe (ahead) + " / past it " + describe (past));
    ctx.expect (ahead.voicesHeld == 1, "an end moved but still ahead cut the note early");
    ctx.expect (past.voicesHeld <= 0, "the note was left held past the region's new end");
    return ctx.verdict();
}

// Muting one region ends its note with a note-off; another region's note on
// the track keeps sounding, not cut and played again by a reset.
ScenarioResult runRegionMute (ScenarioContext& ctx)
{
    auto other = regionOf (0, kLongTicks);
    other.notes.push_back (noteOf (kOtherKey, 0, kNoteTicks));
    if (! setUp (ctx, { longNoteRegion(), other })) return ctx.verdict();

    playFromStart (ctx);
    rollPast (ctx, kInsideTick);
    const auto sounding = readCounters (ctx);
    if (! ctx.expect (sounding.voicesHeld == 2, "the two notes never reached the instrument"))
        return ctx.verdict();
    ctx.expect (editRegion (ctx, [] (MidiRegion& r) { r.muted = true; }), "the mute was refused");
    ctx.pump (2);
    const auto muted = readCounters (ctx);
    rollPast (ctx, kNoteTicks + 20);
    const auto over = readCounters (ctx);
    ctx.note ("sounding " + describe (sounding) + " / muted " + describe (muted) + " / over " + describe (over));
    ctx.expect (muted.voicesHeld == 1, "muting a region did not end its note, and only its note");
    ctx.expect (muted.chokesSeen == sounding.chokesSeen, "muting a region reset the notes other regions hold");
    ctx.expect (over.voicesHeld <= 0, "a note was left held after the other region's note ended");
    return ctx.verdict();
}

ScenarioResult runRegionDelete (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    editWhileSounding (ctx, "deleting the region", [&ctx]
    {
        return performEdit (ctx, new DeleteMidiRegionAction (ctx.session(), ctx.engine(), kTrack, 0));
    });
    return ctx.verdict();
}

// Moved away by an edit, and by a drag, which moves it in place as the pointer
// goes.
ScenarioResult runRegionMove (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    editWhileSounding (ctx, "moving the region away from the transport", [&ctx]
    {
        return editRegion (ctx, [] (MidiRegion& r) { r.timelineStart += samplesOf (2 * kLongTicks); });
    });

    regionsOf (ctx).publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { longNoteRegion() }));
    editWhileSounding (ctx, "dragging the region away from the transport", [&ctx]
    {
        auto& live = regionsOf (ctx).currentMutable();
        live[0].timelineStart += samplesOf (2 * kLongTicks);
        regionsOf (ctx).editedInPlace();
        return true;
    });
    return ctx.verdict();
}

// The take that comes in has nothing on the key the old one held.
ScenarioResult runTakeSwitch (ScenarioContext& ctx)
{
    auto region = longNoteRegion();
    MidiTakeRef older { kLongTicks, { noteOf (kOtherKey, 2000, 200) }, {}, {} };
    region.previousTakes.push_back (older);
    if (! setUp (ctx, { region })) return ctx.verdict();
    editWhileSounding (ctx, "switching the region's take", [&ctx]
    {
        return editRegion (ctx, [] (MidiRegion& r) { cycleTake (r, true); });
    });
    return ctx.verdict();
}

// An undo that takes the region away, and one that shortens it: the region
// was made longer by the step that is undone.
ScenarioResult runUndo (ScenarioContext& ctx)
{
    if (! setUp (ctx, {})) return ctx.verdict();
    ctx.expect (performEdit (ctx, new CreateMidiRegionAction (ctx.session(), kTrack, longNoteRegion())),
                "creating the region was refused");
    editWhileSounding (ctx, "undoing the region's creation", [&ctx] { return undoTransaction (ctx.engine()); });

    auto shortRegion = regionOf (0, kInsideTick / 2);
    regionsOf (ctx).publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { shortRegion }));
    ctx.expect (editRegion (ctx, [] (MidiRegion& r)
    {
        r = longNoteRegion();
    }), "lengthening the region was refused");
    editWhileSounding (ctx, "undoing the step that lengthened the region", [&ctx]
    {
        return undoTransaction (ctx.engine());
    });
    return ctx.verdict();
}

// The piano roll publishes its edits: a sounding note transposed, or deleted.
ScenarioResult runNoteEdit (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    editWhileSounding (ctx, "transposing the sounding note", [&ctx]
    {
        return editRegion (ctx, [] (MidiRegion& r) { r.notes[0].noteNumber = kKey + 2; });
    });
    regionsOf (ctx).publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { longNoteRegion() }));
    editWhileSounding (ctx, "deleting the sounding note", [&ctx]
    {
        return editRegion (ctx, [] (MidiRegion& r) { r.notes.clear(); });
    });
    return ctx.verdict();
}

// Twice the tempo halves where the note ends, which puts it behind the
// transport two thirds of the way through the note.
ScenarioResult runTempoChange (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    editWhileSounding (ctx, "doubling the tempo", [&ctx]
    {
        applyTempoChange (ctx.session(), 2.0f * kBpm, ScenarioContext::kSampleRate);
        return true;
    }, 2 * kNoteTicks / 3);
    return ctx.verdict();
}

// Record pressed while playing: the armed track takes its input from then on,
// so the timeline's note on it ends there.
ScenarioResult runRecordStart (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    track.recordArmed.store (true, std::memory_order_relaxed);
    session.recomputeRtCounters();
    ctx.cleanup ([&track, &session]
    {
        track.recordArmed.store (false, std::memory_order_relaxed);
        session.recomputeRtCounters();
    });
    editWhileSounding (ctx, "pressing Record while playing", [&engine]
    {
        engine.record();
        return engine.getTransport().isRecording();
    });
    return ctx.verdict();
}

// Already reset at the seam: the note is cut at the loop end and plays again
// from its start on the next pass.
ScenarioResult runLoopEnd (ScenarioContext& ctx)
{
    constexpr std::int64_t kLoopTicks = 1000;
    auto region = regionOf (0, kLongTicks);
    region.notes.push_back (noteOf (kKey, 500, kNoteTicks));
    if (! setUp (ctx, { region })) return ctx.verdict();
    auto& transport = ctx.engine().getTransport();
    transport.setLoopRange (0, samplesOf (kLoopTicks));
    transport.setLoopEnabled (true);
    ctx.cleanup ([&transport] { transport.setLoopEnabled (false); });

    playFromStart (ctx);
    rollPast (ctx, 700);
    const auto inside = readCounters (ctx);
    for (int block = 0; transport.getPlayhead() >= samplesOf (700) && block < 1000; ++block)
        ctx.pump (1);
    ctx.pump (1);
    const auto wrapped = readCounters (ctx);
    rollPast (ctx, 600);
    const auto again = readCounters (ctx);
    ctx.note ("inside " + describe (inside) + " / wrapped " + describe (wrapped) + " / again " + describe (again));
    ctx.expect (inside.voicesHeld == 1, "the note never reached the instrument");
    ctx.expect (wrapped.voicesHeld <= 0, "a note held across the loop end was left held after the wrap");
    ctx.expect (again.voicesHeld == 1, "the note did not play again on the next pass");
    return ctx.verdict();
}

// The piano roll's split at a point inside the sounding note: the second half
// ends it, wherever the transport is.
ScenarioResult runSplitNote (ScenarioContext& ctx)
{
    if (! setUp (ctx, { longNoteRegion() })) return ctx.verdict();
    for (const std::int64_t splitAt : { kInsideTick + 500, kInsideTick - 500 })
    {
        regionsOf (ctx).publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { longNoteRegion() }));
        playFromStart (ctx);
        rollPast (ctx, kInsideTick);
        ctx.expect (editRegion (ctx, [splitAt] (MidiRegion& r)
        {
            r.notes = { noteOf (kKey, 0, splitAt), noteOf (kKey, splitAt, kNoteTicks - splitAt) };
        }), "the split was refused");
        rollPast (ctx, kNoteTicks + 20);
        const auto after = readCounters (ctx);
        ctx.note ("split at tick " + std::to_string (splitAt) + ": " + describe (after));
        ctx.expect (after.voicesHeld <= 0, "a note split while it sounded was left held");
        ctx.engine().stop();
        ctx.pump (kSettleBlocks);
    }
    return ctx.verdict();
}
#else
ScenarioResult withoutClap (ScenarioContext&)
{
    return ScenarioResult::skip ("built without the native CLAP host");
}
constexpr Run runRegionEnd = withoutClap, runRegionShortened = withoutClap, runRegionMute = withoutClap,
              runRegionDelete = withoutClap, runRegionMove = withoutClap, runTakeSwitch = withoutClap,
              runUndo = withoutClap, runNoteEdit = withoutClap, runTempoChange = withoutClap,
              runRecordStart = withoutClap, runLoopEnd = withoutClap, runSplitNote = withoutClap;
#endif

const std::vector<std::string> kTags { "midi", "panic", "clap", "region" };
const std::vector<std::string> kFixtures { "panic_probe.clap" };

std::function<std::optional<ScenarioResult> (ScenarioContext&)> running (Run run)
{
    return [run] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return run (ctx); };
}

const ScenarioRegistrar endRegistrar { Scenario {
    "midi.release_at_region_end", kTags, Needs::Engine, kFixtures, running (runRegionEnd) } };
const ScenarioRegistrar shortenedRegistrar { Scenario {
    "midi.release_on_region_shortened", kTags, Needs::Engine, kFixtures, running (runRegionShortened) } };
const ScenarioRegistrar muteRegistrar { Scenario {
    "midi.release_on_region_mute", kTags, Needs::Engine, kFixtures, running (runRegionMute) } };
const ScenarioRegistrar deleteRegistrar { Scenario {
    "midi.release_on_region_delete", kTags, Needs::Engine, kFixtures, running (runRegionDelete) } };
const ScenarioRegistrar moveRegistrar { Scenario {
    "midi.release_on_region_move", kTags, Needs::Engine, kFixtures, running (runRegionMove) } };
const ScenarioRegistrar takeRegistrar { Scenario {
    "midi.release_on_take_switch", kTags, Needs::Engine, kFixtures, running (runTakeSwitch) } };
const ScenarioRegistrar undoRegistrar { Scenario {
    "midi.release_on_undo", kTags, Needs::Engine, kFixtures, running (runUndo) } };
const ScenarioRegistrar noteEditRegistrar { Scenario {
    "midi.release_on_note_edit", kTags, Needs::Engine, kFixtures, running (runNoteEdit) } };
const ScenarioRegistrar tempoRegistrar { Scenario {
    "midi.release_on_tempo_change", kTags, Needs::Engine, kFixtures, running (runTempoChange) } };
const ScenarioRegistrar recordRegistrar { Scenario {
    "midi.release_on_record_start", kTags, Needs::Engine, kFixtures, running (runRecordStart) } };
const ScenarioRegistrar loopRegistrar { Scenario {
    "midi.release_at_loop_end", kTags, Needs::Engine, kFixtures, running (runLoopEnd) } };
const ScenarioRegistrar splitRegistrar { Scenario {
    "midi.split_note_keeps_its_end", kTags, Needs::Engine, kFixtures, running (runSplitNote) } };
} // namespace
} // namespace duskstudio::scenario
