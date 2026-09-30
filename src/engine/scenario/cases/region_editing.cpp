#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 3;

AudioRegion regionAt (std::int64_t start, std::int64_t length, std::int64_t offset = 0)
{
    AudioRegion r;
    r.timelineStart = start;
    r.lengthInSamples = length;
    r.sourceOffset = offset;
    return r;
}

bool nearly (float a, float b) { return std::abs (a - b) < 1.0e-6f; }

bool sameSpan (const AudioRegion& r, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    return r.timelineStart == start && r.lengthInSamples == length && r.sourceOffset == offset;
}

auto& regionsOf (ScenarioContext& ctx) { return ctx.session().track (kTrack).regions; }

// Cmd/Ctrl+E: the left half keeps the start, the right half starts at the
// split and reads on from where the left stopped, so the audio is continuous.
ScenarioResult splitKeepsAudioContinuous (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (1000, 48000, 500));
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new SplitRegionAction (ctx.session(), ctx.engine(), kTrack, 0, 25000)),
                "the split refused a point inside the region");
    if (ctx.expect (regs.size() == 2, "the split did not leave two regions"))
    {
        ctx.expect (sameSpan (regs[0], 1000, 24000, 500), "the left half is not the region's first part");
        ctx.expect (sameSpan (regs[1], 25000, 24000, 24500),
                    "the right half does not continue the source where the left stops");
    }

    ctx.expect (undo.undo() && regs.size() == 1 && sameSpan (regs[0], 1000, 48000, 500),
                "undo did not restore the whole region");
    ctx.expect (undo.redo() && regs.size() == 2, "redo did not split again");

    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new SplitRegionAction (ctx.session(), ctx.engine(), kTrack, 0, 1000)),
                "a split at the region's own start was accepted");
    return ctx.verdict();
}

// Freezing bakes a track, so its regions are locked against edits.
ScenarioResult frozenTrackRefusesEdits (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (0, 48000));
    ctx.session().track (kTrack).frozen.store (true, std::memory_order_relaxed);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (! undo.perform (new SplitRegionAction (ctx.session(), ctx.engine(), kTrack, 0, 24000)),
                "a frozen track's region was split");
    ctx.expect (! undo.perform (new DeleteRegionAction (ctx.session(), ctx.engine(), kTrack, 0)),
                "a frozen track's region was deleted");
    ctx.expect (regs.size() == 1 && sameSpan (regs[0], 0, 48000, 0), "the frozen region changed");
    ctx.session().track (kTrack).frozen.store (false, std::memory_order_relaxed);
    return ctx.verdict();
}

// Paste and duplicate insert a copy; undo takes the same copy back out.
ScenarioResult pasteInsertsAndUndoRemoves (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (0, 48000));
    auto copy = regs[0];
    copy.timelineStart = 48000;
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new PasteRegionAction (ctx.session(), ctx.engine(), kTrack, copy)),
                "the paste was refused");
    ctx.expect (regs.size() == 2 && sameSpan (regs[1], 48000, 48000, 0),
                "the copy did not land where it was placed");
    ctx.expect (undo.undo() && regs.size() == 1 && sameSpan (regs[0], 0, 48000, 0),
                "undo did not remove the copy");
    return ctx.verdict();
}

// Deleting a region takes its take history with it, and undo brings both back.
ScenarioResult deleteRestoresTakesOnUndo (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto region = regionAt (0, 48000);
    for (int i = 1; i <= 3; ++i)
    {
        TakeRef take;
        take.sourceOffset = 1000 * i;
        take.lengthInSamples = 48000;
        region.previousTakes.push_back (take);
    }
    regs.push_back (region);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new DeleteRegionAction (ctx.session(), ctx.engine(), kTrack, 0)),
                "the delete was refused");
    ctx.expect (regs.empty(), "the region is still on the track");
    ctx.expect (undo.undo() && regs.size() == 1, "undo did not bring the region back");
    if (regs.size() == 1)
    {
        ctx.expect (regs[0].previousTakes.size() == 3, "undo lost the region's take history");
        ctx.expect (regs[0].previousTakes.size() == 3 && regs[0].previousTakes[2].sourceOffset == 3000,
                    "undo reordered the take history");
    }
    return ctx.verdict();
}

// Moves, trims, fades and gain all collapse to "region is now this"; undo
// puts back every field.
ScenarioResult editUndoRestoresEveryField (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (1000, 48000));
    const auto before = regs[0];
    auto after = before;
    after.timelineStart = 5000;
    after.lengthInSamples = 30000;
    after.sourceOffset = 2000;
    after.fadeInSamples = 480;
    after.fadeOutSamples = 960;
    after.gainDb = -6.0f;
    after.muted = true;
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new RegionEditAction (ctx.session(), ctx.engine(), kTrack, 0, before, after)),
                "the edit was refused");
    if (! ctx.expect (regs.size() == 1, "the edit changed the number of regions"))
        return ctx.verdict();
    ctx.expect (sameSpan (regs[0], 5000, 30000, 2000) && regs[0].fadeInSamples == 480
                    && regs[0].fadeOutSamples == 960 && nearly (regs[0].gainDb, -6.0f) && regs[0].muted,
                "the edit did not apply every field");
    ctx.expect (undo.undo(), "undo was refused");
    if (! ctx.expect (regs.size() == 1, "undo changed the number of regions"))
        return ctx.verdict();
    ctx.expect (sameSpan (regs[0], 1000, 48000, 0) && regs[0].fadeInSamples == 0
                    && regs[0].fadeOutSamples == 0 && nearly (regs[0].gainDb, 0.0f) && ! regs[0].muted,
                "undo did not restore every field");
    return ctx.verdict();
}

// Join glues abutting regions of one source back into a single region, the
// way a split made them, and undo separates them again.
ScenarioResult joinRejoinsASplit (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (1000, 24000, 500));
    regs.push_back (regionAt (25000, 24000, 24500));
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new JoinRegionsAction (ctx.session(), ctx.engine(), kTrack, { 0, 1 })),
                "the join was refused");
    ctx.expect (regs.size() == 1 && sameSpan (regs[0], 1000, 48000, 500),
                "the join did not rebuild the region the split came from");
    ctx.expect (undo.undo() && regs.size() == 2 && sameSpan (regs[1], 25000, 24000, 24500),
                "undo did not separate the regions again");
    return ctx.verdict();
}

// An edit over several MIDI regions of one track publishes the track's regions
// once on perform, undo and redo: the snapshot keeps one retired vector, so a
// second publish inside an audio block frees the vector that block reads. A
// batch of region edits, and its undo and redo, rebuilds playback once.
ScenarioResult batchedEditsPublishAndRebuildOnce (ScenarioContext& ctx)
{
    constexpr int kMidiTrack = kTrack + 1;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& lane = session.track (kMidiTrack).midiRegions;
    session.track (kMidiTrack).mode.store ((int) Track::Mode::Midi);
    std::vector<MidiRegion> seeded (3);
    for (int i = 0; i < 3; ++i)
    {
        seeded[(size_t) i].timelineStart = 48000 * i;
        seeded[(size_t) i].lengthInSamples = 48000;
    }
    lane.publish (std::make_unique<std::vector<MidiRegion>> (seeded));
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (0, 48000));
    regs.push_back (regionAt (48000, 48000));

    auto& undo = engine.getUndoManager();
    auto& playback = engine.getPlaybackEngine();
    undo.clearUndoHistory();
    auto published = lane.generation();
    auto rebuilt = playback.rebuildCount();
    const auto once = [&] (std::uint64_t publishes, const std::string& what)
    {
        const auto p = lane.generation() - published;
        const auto r = playback.rebuildCount() - rebuilt;
        ctx.expect (p == publishes && r == 1, what + " published the MIDI track " + std::to_string (p)
                                               + " times and rebuilt playback " + std::to_string (r) + " times");
        published = lane.generation();
        rebuilt = playback.rebuildCount();
    };
    const auto muted = [&lane, &regs] (bool on)
    {
        const auto& live = lane.current();
        return live.size() == 3 && live[0].muted == on && ! live[1].muted && live[2].muted == on
            && regs.size() == 2 && regs[0].muted == on && regs[1].muted == on;
    };
    const auto starts = [&lane]
    {
        std::vector<std::int64_t> at;
        for (const auto& r : lane.current()) at.push_back (r.timelineStart);
        return at;
    };

    undo.beginNewTransaction();
    {
        const RegionRebuildBatch batch (engine);
        std::vector<MidiRegionEditAction::Change> changes;
        for (const int i : { 0, 2 })
        {
            auto after = seeded[(size_t) i];
            after.muted = true;
            changes.push_back ({ i, seeded[(size_t) i], after });
        }
        ctx.expect (undo.perform (new MidiRegionEditAction (session, engine, kMidiTrack, std::move (changes))),
                    "the MIDI edit was refused");
        for (int i = 0; i < 2; ++i)
        {
            auto after = regs[(size_t) i];
            after.muted = true;
            ctx.expect (undo.perform (new RegionEditAction (session, engine, kTrack, i, regs[(size_t) i], after)),
                        "an audio edit was refused");
        }
    }
    once (1, "muting two MIDI regions and two audio regions");
    ctx.expect (muted (true), "the batch did not mute its regions, and only those");
    ctx.expect (undoTransaction (engine), "undo was refused");
    once (1, "undoing the mute");
    ctx.expect (muted (false), "undo did not unmute every region");
    ctx.expect (redoTransaction (engine), "redo was refused");
    once (1, "redoing the mute");
    ctx.expect (muted (true), "redo did not mute the regions again");

    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteMidiRegionAction (session, engine, kMidiTrack, std::vector<int> { 0, 2 })),
                "the delete was refused");
    once (1, "deleting two MIDI regions");
    ctx.expect (starts() == std::vector<std::int64_t> { 48000 }, "the delete did not leave just the middle region");
    ctx.expect (undoTransaction (engine), "undoing the delete was refused");
    once (1, "undoing the delete");
    ctx.expect (starts() == std::vector<std::int64_t> { 0, 48000, 96000 },
                "undo did not put each region back in its slot");
    ctx.expect (redoTransaction (engine), "redoing the delete was refused");
    once (1, "redoing the delete");
    ctx.expect (starts() == std::vector<std::int64_t> { 48000 }, "redo did not delete the two regions again");

    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new DeleteMidiRegionAction (session, engine, kMidiTrack, std::vector<int> { 0, 5 })),
                "a delete reaching past the last region was accepted");
    ctx.expect (lane.generation() == published && starts() == std::vector<std::int64_t> { 48000 },
                "a refused delete changed or published the regions");
    return ctx.verdict();
}

std::optional<ScenarioResult> run (ScenarioResult (*body) (ScenarioContext&), ScenarioContext& ctx)
{
    return body (ctx);
}

const ScenarioRegistrar splitRegistrar { Scenario {
    "region.split_keeps_audio_continuous", { "region", "edit", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (splitKeepsAudioContinuous, ctx); } } };
const ScenarioRegistrar frozenRegistrar { Scenario {
    "region.frozen_track_refuses_edits", { "region", "edit", "freeze" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (frozenTrackRefusesEdits, ctx); } } };
const ScenarioRegistrar pasteRegistrar { Scenario {
    "region.paste_inserts_and_undo_removes", { "region", "edit", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (pasteInsertsAndUndoRemoves, ctx); } } };
const ScenarioRegistrar deleteRegistrar { Scenario {
    "region.delete_restores_takes_on_undo", { "region", "edit", "undo", "takes" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (deleteRestoresTakesOnUndo, ctx); } } };
const ScenarioRegistrar editRegistrar { Scenario {
    "region.edit_undo_restores_every_field", { "region", "edit", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (editUndoRestoresEveryField, ctx); } } };
const ScenarioRegistrar joinRegistrar { Scenario {
    "region.join_rejoins_a_split", { "region", "edit", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (joinRejoinsASplit, ctx); } } };
const ScenarioRegistrar batchedRegistrar { Scenario {
    "region.batched_edits_publish_and_rebuild_once", { "region", "edit", "undo", "midi" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (batchedEditsPublishAndRebuildOnce, ctx); } } };
} // namespace
} // namespace duskstudio::scenario
