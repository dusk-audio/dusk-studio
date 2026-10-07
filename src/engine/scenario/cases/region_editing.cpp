#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 3;

using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

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

// Deleting a region leaves the take it came from on the track, and undo
// brings the region back naming that take.
ScenarioResult deleteKeepsItsTake (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto& takes = ctx.session().track (kTrack).takes;
    AudioTake take;
    take.id = ctx.session().allocateTakeId();
    take.name = "Take 1";
    take.lengthInSamples = 48000;
    takes.push_back (take);
    auto region = regionAt (0, 48000);
    region.takeId = take.id;
    regs.push_back (region);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new DeleteRegionAction (ctx.session(), ctx.engine(), kTrack, 0)),
                "the delete was refused");
    ctx.expect (regs.empty(), "the region is still on the track");
    ctx.expect (takes.size() == 1 && takes[0].id == take.id && takes[0].lengthInSamples == 48000,
                "deleting the region removed or changed its take");
    ctx.expect (undo.undo() && regs.size() == 1, "undo did not bring the region back");
    if (regs.size() == 1)
        ctx.expect (regs[0].takeId == take.id, "undo brought the region back without its take");
    ctx.expect (takes.size() == 1, "undo changed the track's takes");
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

std::filesystem::path pathOf (const SessionFile& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

// A sine of one phase at every level, so two files of it line up on the
// timeline when their regions read them from the same offset.
std::vector<float> tone (int length, float level)
{
    std::vector<float> samples ((std::size_t) length);
    for (int i = 0; i < length; ++i)
        samples[(std::size_t) i] = level * std::sin (6.283185307f * 220.0f * (float) i
                                                     / (float) ScenarioContext::kSampleRate);
    return samples;
}

std::optional<SessionFile> writtenFile (ScenarioContext& ctx, const std::string& name,
                                        const std::vector<float>& samples)
{
    const auto path = ctx.tempDir() / (name + ".wav");
    dusk::audio::WriteSpec spec;
    spec.sampleRate = ScenarioContext::kSampleRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    if (writer == nullptr || ! writer->write (channels, 1, (std::int64_t) samples.size()) || ! writer->flush())
        return std::nullopt;
    return SessionFile (path.u8string().c_str());
}

std::vector<float> samplesOf (const SessionFile& file)
{
    auto reader = dusk::audio::FileReader::open (pathOf (file));
    if (reader == nullptr) return {};
    std::vector<float> samples ((std::size_t) reader->info().numFrames);
    float* dest[] = { samples.data() };
    if (reader->read (dest, 1, 0, (std::int64_t) samples.size()) != (std::int64_t) samples.size())
        return {};
    return samples;
}

AudioRegion regionOf (const SessionFile& file, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    auto r = regionAt (start, length, offset);
    r.file = file;
    return r;
}

std::size_t rendersIn (ScenarioContext& ctx)
{
    const auto takesDir = pathOf (ctx.session().getSessionDirectory().getChildFile ("takes"));
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it (takesDir, error), end; ! error && it != end; it.increment (error))
        ++count;
    return count;
}

constexpr std::int64_t kSeam = 64;

// A comp that plays one take throughout is three regions of its file meeting at
// crossfaded seams. Joined, they are one region that plays the file, and
// nothing is rendered.
ScenarioResult joinOfOneFileAcrossSeamsPlaysTheFile (ScenarioContext& ctx)
{
    const auto file = writtenFile (ctx, "join-one-file", tone (14400, 0.5f));
    if (! file) return ScenarioResult::fail ("could not write the source file");
    auto& regs = regionsOf (ctx);
    auto head = regionOf (*file, 1000, 4832, 200);
    head.fadeOutSamples = kSeam;
    head.fadeOutShape = FadeShape::RaisedCosine;
    auto middle = regionOf (*file, 5768, 4864, 4968);
    middle.fadeInSamples = middle.fadeOutSamples = kSeam;
    middle.fadeInShape = middle.fadeOutShape = FadeShape::RaisedCosine;
    auto tail = regionOf (*file, 10568, 3832, 9768);
    tail.fadeInSamples = kSeam;
    tail.fadeInShape = FadeShape::RaisedCosine;
    head.takeId = middle.takeId = tail.takeId = 7;
    regs = { head, middle, tail };
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    ctx.expect (undo.perform (new JoinRegionsAction (ctx.session(), ctx.engine(), kTrack, { 0, 1, 2 })),
                "the join was refused");
    if (ctx.expect (regs.size() == 1, "the join did not leave one region"))
    {
        ctx.expect (regs[0].file == *file && sameSpan (regs[0], 1000, 13400, 200),
                    "the joined region does not play the file straight through");
        ctx.expect (regs[0].fadeInSamples == 0 && regs[0].fadeOutSamples == 0,
                    "a seam fade was left on the joined region");
        ctx.expect (regs[0].takeId == 7, "the joined region stopped naming the take it plays");
    }
    ctx.expect (rendersIn (ctx) == 0, "joining regions of one file on one alignment rendered a file");
    ctx.expect (undo.undo() && regs.size() == 3 && sameSpan (regs[1], 5768, 4864, 4968),
                "undo did not put the three regions back");

    regs[1].sourceOffset += 10;
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new JoinRegionsAction (ctx.session(), ctx.engine(), kTrack, { 0, 1, 2 }))
                    && regs.size() == 1 && regs[0].file != *file,
                "overlapping regions that read the file from different places joined without a render");
    return ctx.verdict();
}

// Two takes meeting at a seam join into a file that holds the crossfade the
// regions played, and the fades at the outer edges stay on the region instead
// of going into the file as well.
ScenarioResult joinRendersASeamAsItPlayed (ScenarioContext& ctx)
{
    constexpr std::int64_t kLength = 9600, kMeet = 4800;
    const auto loud = tone ((int) kLength, 0.5f);
    const auto quiet = tone ((int) kLength, 0.3f);
    const auto first = writtenFile (ctx, "join-seam-first", loud);
    const auto second = writtenFile (ctx, "join-seam-second", quiet);
    if (! first || ! second) return ScenarioResult::fail ("could not write the source files");
    auto& regs = regionsOf (ctx);
    const std::int64_t overlapStart = kMeet - kSeam / 2, overlapEnd = kMeet + kSeam / 2;
    auto left = regionOf (*first, 0, overlapEnd, 0);
    left.fadeInSamples = 480;
    left.fadeInShape = FadeShape::Exp;
    left.fadeOutSamples = kSeam;
    left.fadeOutShape = FadeShape::RaisedCosine;
    auto right = regionOf (*second, overlapStart, kLength - overlapStart, overlapStart);
    right.fadeInSamples = kSeam;
    right.fadeInShape = FadeShape::RaisedCosine;
    right.fadeOutSamples = 960;
    right.fadeOutShape = FadeShape::Log;
    regs = { left, right };
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    if (! ctx.expect (undo.perform (new JoinRegionsAction (ctx.session(), ctx.engine(), kTrack, { 0, 1 })),
                      "the join was refused")
        || ! ctx.expect (regs.size() == 1, "the join did not leave one region"))
        return ctx.verdict();
    const auto joined = samplesOf (regs[0].file);
    if (! ctx.expect (regs[0].file != *first && regs[0].file != *second && (std::int64_t) joined.size() == kLength,
                      "the join did not render the whole span into a file of its own"))
        return ctx.verdict();

    float worst = 0.0f, overlapPeak = 0.0f;
    std::int64_t worstAt = 0;
    for (std::int64_t i = 0; i < kLength; ++i)
    {
        float expected = 0.0f;
        if (i < overlapEnd)
            expected += loud[(std::size_t) i]
                      * (i >= overlapStart ? applyFadeShape ((float) (overlapEnd - i) / (float) kSeam,
                                                             FadeShape::RaisedCosine)
                                           : 1.0f);
        if (i >= overlapStart)
            expected += quiet[(std::size_t) i]
                      * (i < overlapEnd ? applyFadeShape ((float) (i - overlapStart) / (float) kSeam,
                                                          FadeShape::RaisedCosine)
                                        : 1.0f);
        const float off = std::abs (joined[(std::size_t) i] - expected);
        if (off > worst) { worst = off; worstAt = i; }
        if (i >= overlapStart && i < overlapEnd)
            overlapPeak = std::max (overlapPeak, std::abs (joined[(std::size_t) i]));
    }
    ctx.expect (worst < 1.0e-4f, "the joined file is " + std::to_string (worst) + " away from the crossfade at sample "
                                     + std::to_string (worstAt));
    ctx.expect (overlapPeak <= 0.5f + 1.0e-4f,
                "the seam peaks at " + std::to_string (overlapPeak) + ", above either take");
    ctx.expect (regs[0].fadeInSamples == 480 && regs[0].fadeInShape == FadeShape::Exp
                    && regs[0].fadeOutSamples == 960 && regs[0].fadeOutShape == FadeShape::Log,
                "the joined region lost a fade from its outer edges");
    ctx.expect (undo.undo() && regs.size() == 2 && regs[1].file == *second,
                "undo did not separate the regions again");
    return ctx.verdict();
}

// Regions with a space between them join into a file that is silent there.
ScenarioResult joinRendersAGapAsSilence (ScenarioContext& ctx)
{
    const auto source = tone (9600, 0.5f);
    const auto file = writtenFile (ctx, "join-gap", source);
    if (! file) return ScenarioResult::fail ("could not write the source file");
    auto& regs = regionsOf (ctx);
    regs = { regionOf (*file, 0, 2400, 0), regionOf (*file, 3400, 2400, 2400) };
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    undo.beginNewTransaction();

    if (! ctx.expect (undo.perform (new JoinRegionsAction (ctx.session(), ctx.engine(), kTrack, { 0, 1 })),
                      "the join was refused")
        || ! ctx.expect (regs.size() == 1 && sameSpan (regs[0], 0, 5800, 0),
                         "the join did not leave one region over the whole span"))
        return ctx.verdict();
    const auto joined = samplesOf (regs[0].file);
    if (! ctx.expect (regs[0].file != *file && joined.size() == 5800, "the join did not render the span"))
        return ctx.verdict();
    float gapPeak = 0.0f, worst = 0.0f;
    for (std::size_t i = 0; i < joined.size(); ++i)
    {
        if (i >= 2400 && i < 3400)
            gapPeak = std::max (gapPeak, std::abs (joined[i]));
        else
            worst = std::max (worst, std::abs (joined[i] - source[i < 2400 ? i : i - 1000]));
    }
    ctx.expect (! (gapPeak > 0.0f), "the gap between the regions is not silent in the joined file");
    ctx.expect (worst < 1.0e-4f, "the joined file does not hold the regions either side of the gap");
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
    "region.delete_keeps_its_take", { "region", "edit", "undo", "takes" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (deleteKeepsItsTake, ctx); } } };
const ScenarioRegistrar editRegistrar { Scenario {
    "region.edit_undo_restores_every_field", { "region", "edit", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (editUndoRestoresEveryField, ctx); } } };
const ScenarioRegistrar joinRegistrar { Scenario {
    "region.join_rejoins_a_split", { "region", "edit", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (joinRejoinsASplit, ctx); } } };
const ScenarioRegistrar joinOneFileRegistrar { Scenario {
    "region.join_of_one_file_across_seams_plays_the_file", { "region", "edit", "undo", "take" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (joinOfOneFileAcrossSeamsPlaysTheFile, ctx); } } };
const ScenarioRegistrar joinSeamRegistrar { Scenario {
    "region.join_renders_a_seam_as_it_played", { "region", "edit", "undo", "take" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (joinRendersASeamAsItPlayed, ctx); } } };
const ScenarioRegistrar joinGapRegistrar { Scenario {
    "region.join_renders_a_gap_as_silence", { "region", "edit" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (joinRendersAGapAsSilence, ctx); } } };
const ScenarioRegistrar batchedRegistrar { Scenario {
    "region.batched_edits_publish_and_rebuild_once", { "region", "edit", "undo", "midi" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (batchedEditsPublishAndRebuildOnce, ctx); } } };
} // namespace
} // namespace duskstudio::scenario
