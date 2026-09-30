#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../PlaybackEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"
#include "../../../session/TakeComp.h"
#include "../../../foundation/Json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 5;
// The click-mask crossfade a punch leaves against the region it cuts into.
constexpr std::int64_t kPunchFade = 64;

AudioRegion regionAt (std::int64_t start, std::int64_t length, std::int64_t offset = 0)
{
    AudioRegion r;
    r.timelineStart = start;
    r.lengthInSamples = length;
    r.sourceOffset = offset;
    return r;
}

std::int64_t endOf (const AudioRegion& r) { return r.timelineStart + r.lengthInSamples; }

auto& regionsOf (ScenarioContext& ctx) { return ctx.session().track (kTrack).regions; }

void armTrack (ScenarioContext& ctx)
{
    // The track follows its own index for input, so it records from input
    // kTrack + 1. The arm is stored directly, past Session's input check, so
    // pin a capture width that offers that input: the armed state stays one
    // the session would accept whatever the default device's width is.
    auto& captureWidth = ctx.session().deviceCaptureChannels;
    ctx.keep (captureWidth);
    captureWidth.store (kTrack + 1);
    auto& track = ctx.session().track (kTrack);
    track.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    track.inputSource.store (-2, std::memory_order_relaxed);
    track.recordArmed.store (true, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();
}

// Records from `from` until the playhead passes `to`, then stops, which
// commits the take and puts it on the undo stack as one step.
bool recordSpan (ScenarioContext& ctx, std::int64_t from, std::int64_t to)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    transport.setPlayhead (from);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start with the track armed"))
        return false;
    for (int block = 0; transport.getPlayhead() < to && block < 10000; ++block)
        ctx.pump (1);
    engine.stop();
    return true;
}

const AudioRegion* regionStartingAt (ScenarioContext& ctx, std::int64_t start)
{
    for (const auto& r : regionsOf (ctx))
        if (r.timelineStart == start) return &r;
    return nullptr;
}

// A take on the track with nothing of it on the timeline.
TakeId addTake (ScenarioContext& ctx, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    auto& track = ctx.session().track (kTrack);
    AudioTake take;
    take.id = ctx.session().allocateTakeId();
    take.name = "Take " + std::to_string (track.takes.size() + 1);
    take.timelineStart = start;
    take.lengthInSamples = length;
    take.sourceOffset = offset;
    track.takes.push_back (take);
    return take.id;
}

// An existing region cut from a take of its own, the way a recording leaves one.
TakeId seedRegion (ScenarioContext& ctx, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    const auto id = addTake (ctx, start, length, offset);
    auto region = regionAt (start, length, offset);
    region.takeId = id;
    regionsOf (ctx).push_back (region);
    return id;
}

const AudioTake* takeWithId (ScenarioContext& ctx, TakeId id)
{
    for (const auto& take : ctx.session().track (kTrack).takes)
        if (take.id == id) return &take;
    return nullptr;
}

bool takeIsWhole (ScenarioContext& ctx, TakeId id, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    const auto* take = takeWithId (ctx, id);
    return take != nullptr && take->timelineStart == start && take->lengthInSamples == length
        && take->sourceOffset == offset;
}

// A recording that covers a whole region takes the region off the timeline and
// leaves its take on the track; Undo puts the region back and drops the new take.
ScenarioResult fullCoverKeepsTheCoveredTake (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto& takes = ctx.session().track (kTrack).takes;
    const auto old = seedRegion (ctx, 4800, 4800, 1000);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);

    if (! recordSpan (ctx, 2400, 12000))
        return ctx.verdict();

    TakeId fresh = 0;
    if (ctx.expect (regs.size() == 1, "the covered region is still on the timeline; "
                                          + std::to_string (regs.size()) + " regions on the track"))
    {
        const auto& live = regs[0];
        fresh = live.takeId;
        ctx.expect (live.timelineStart <= 4800 && endOf (live) >= 9600,
                    "the new take does not span the region it covered");
        ctx.expect (fresh != 0 && fresh != old && takeWithId (ctx, fresh) != nullptr,
                    "the new region does not name a take of its own");
        ctx.expect (live.previousTakes.empty(), "the recording built a take stack on the region");
    }
    ctx.expect (takes.size() == 2 && takeIsWhole (ctx, old, 4800, 4800, 1000),
                "the covered region's take did not stay on the track as it was");

    ctx.expect (undo.undo(), "undo after recording was refused");
    ctx.expect (regs.size() == 1 && regs[0].timelineStart == 4800 && regs[0].sourceOffset == 1000
                    && regs[0].lengthInSamples == 4800 && regs[0].takeId == old,
                "undo after recording did not bring the old region back as it was");
    ctx.expect (takes.size() == 1 && takes[0].id == old, "undo after recording left the new take behind");
    ctx.expect (undo.redo() && regs.size() == 1 && regs[0].takeId == fresh
                    && takes.size() == 2 && takes[1].id == fresh,
                "redo did not bring the take back under the same id");
    return ctx.verdict();
}

// A take over one end of an older region trims that region back to a
// crossfade; the older take stays whole and the audio still playing from it
// is exactly what played before.
ScenarioResult partialOverdubKeepsCoveredTakeWhole (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    const auto old = seedRegion (ctx, 4800, 4800, 1000);
    const AudioRegion before = regs[0];
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);

    if (! recordSpan (ctx, 7200, 12000))
        return ctx.verdict();

    const auto* kept = regionStartingAt (ctx, 4800);
    const AudioRegion* fresh = nullptr;
    for (const auto& r : regs)
        if (&r != kept) fresh = &r;

    if (ctx.expect (regs.size() == 2 && kept != nullptr && fresh != nullptr,
                    "an edge overdub should leave the old region and the new take side by side; "
                        + std::to_string (regs.size()) + " regions on the track"))
    {
        ctx.expect (endOf (*kept) > fresh->timelineStart && endOf (*kept) <= fresh->timelineStart + kPunchFade,
                    "the old region was not trimmed back to a crossfade under the new take");
        ctx.expect (kept->file == before.file && kept->sourceOffset == before.sourceOffset
                        && kept->takeId == old && kept->fadeInSamples == before.fadeInSamples
                        && kept->gainDb == before.gainDb,
                    "the uncovered part of the old region no longer plays the same audio");
        ctx.expect (fresh->takeId != 0 && fresh->takeId != old, "the new region does not name its own take");
    }
    ctx.expect (takeIsWhole (ctx, old, 4800, 4800, 1000), "the covered take was trimmed with its region");

    ctx.expect (undo.undo() && regs.size() == 1 && regs[0].timelineStart == 4800
                    && regs[0].lengthInSamples == 4800,
                "undo did not restore the whole old region");
    return ctx.verdict();
}

// A punch inside an older region splits it around the new take. The two
// fragments still name the older take, which stays whole; the punch names its own.
ScenarioResult punchInsideNamesEveryTake (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    const auto old = seedRegion (ctx, 4800, 19200, 0);
    armTrack (ctx);

    if (! recordSpan (ctx, 9600, 14400))
        return ctx.verdict();

    const auto* left = regionStartingAt (ctx, 4800);
    const AudioRegion* fresh = nullptr;
    const AudioRegion* right = nullptr;
    for (const auto& r : regs)
    {
        if (&r == left) continue;
        if (endOf (r) == 24000) right = &r;
        else fresh = &r;
    }

    if (! ctx.expect (regs.size() == 3 && left != nullptr && fresh != nullptr && right != nullptr,
                      "the punch should leave the old region on both sides of the new take; "
                          + std::to_string (regs.size()) + " regions on the track"))
        return ctx.verdict();

    ctx.expect (endOf (*left) <= fresh->timelineStart + kPunchFade
                    && right->timelineStart >= endOf (*fresh) - kPunchFade,
                "the old region still runs under the new take");
    ctx.expect (right->sourceOffset == right->timelineStart - 4800,
                "the right-hand fragment does not continue the old audio where it resumes");
    ctx.expect (left->takeId == old && right->takeId == old,
                "a fragment of the old region lost the take it came from");
    ctx.expect (fresh->takeId != 0 && fresh->takeId != old && takeWithId (ctx, fresh->takeId) != nullptr,
                "the punch does not name a take of its own");
    ctx.expect (takeIsWhole (ctx, old, 4800, 19200, 0), "the punched-into take was cut down");
    for (const auto& r : regs)
        ctx.expect (r.previousTakes.empty(), "the punch built a take stack on a region");
    return ctx.verdict();
}

// Deleting a recorded region leaves its take on the track, and so does
// splitting one: both halves name the take and neither carries a stack.
ScenarioResult editsLeaveRecordedTakes (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto& takes = ctx.session().track (kTrack).takes;
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);

    if (! recordSpan (ctx, 4800, 14400) || ! recordSpan (ctx, 4800, 14400))
        return ctx.verdict();
    if (! ctx.expect (regs.size() == 1 && takes.size() == 2, "two takes over one span did not leave one region and two takes"))
        return ctx.verdict();
    const auto taken = takes;
    const auto newest = regs[0].takeId;
    ctx.expect (newest == taken[1].id, "the region does not name the newest take");

    const auto mid = regs[0].timelineStart + regs[0].lengthInSamples / 2;
    undo.beginNewTransaction();
    if (ctx.expect (undo.perform (new SplitRegionAction (ctx.session(), ctx.engine(), kTrack, 0, mid)),
                    "the split was refused")
        && ctx.expect (regs.size() == 2, "the split did not leave two regions"))
    {
        ctx.expect (regs[0].takeId == newest && regs[1].takeId == newest,
                    "the split halves do not both name the take they came from");
        ctx.expect (regs[0].previousTakes.empty() && regs[1].previousTakes.empty(),
                    "the split gave a half a take stack");
    }
    ctx.expect (takes.size() == 2, "the split changed the track's takes");

    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteRegionAction (ctx.session(), ctx.engine(), kTrack, 1)),
                "deleting a half was refused");
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteRegionAction (ctx.session(), ctx.engine(), kTrack, 0)),
                "deleting the other half was refused");
    ctx.expect (regs.empty(), "a region is still on the track");
    ctx.expect (takes.size() == 2 && takes[0].id == taken[0].id && takes[1].id == taken[1].id
                    && takeIsWhole (ctx, taken[0].id, taken[0].timelineStart, taken[0].lengthInSamples, taken[0].sourceOffset)
                    && takeIsWhole (ctx, taken[1].id, taken[1].timelineStart, taken[1].lengthInSamples, taken[1].sourceOffset),
                "deleting the recorded region removed or changed a take");
    ctx.expect (undo.undo() && regs.size() == 1 && regs[0].takeId == newest,
                "undo did not bring the deleted half back naming its take");
    return ctx.verdict();
}

// Undoing a recording removes the take it added and only that one; redo
// brings it back under the same id.
ScenarioResult undoRecordRemovesItsTake (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto& takes = ctx.session().track (kTrack).takes;
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);

    if (! recordSpan (ctx, 4800, 9600))
        return ctx.verdict();
    if (! ctx.expect (takes.size() == 1 && regs.size() == 1, "the first recording did not add one take"))
        return ctx.verdict();
    const auto first = takes[0];

    if (! recordSpan (ctx, 7200, 12000))
        return ctx.verdict();
    if (! ctx.expect (takes.size() == 2, "the second recording did not add a take"))
        return ctx.verdict();
    const auto second = takes[1];
    ctx.expect (second.id != first.id && second.name == "Take 2", "the second take has no id or name of its own");

    ctx.expect (undo.undo(), "undo after recording was refused");
    ctx.expect (takes.size() == 1 && takes[0].id == first.id, "undo did not remove exactly the take it recorded");
    ctx.expect (regs.size() == 1 && regs[0].takeId == first.id && regs[0].lengthInSamples == first.lengthInSamples,
                "undo did not put the first take's region back whole");
    ctx.expect (undo.redo() && takes.size() == 2 && takes[1].id == second.id && takes[1].file == second.file,
                "redo did not bring the take back under the same id");
    ctx.expect (undo.undo() && undo.undo() && takes.empty() && regs.empty(),
                "undoing both recordings left a take behind");
    return ctx.verdict();
}

ScenarioResult eightInputsRecordSeparately (ScenarioContext& ctx)
{
    constexpr int channels = 8;
    constexpr int frames = ScenarioContext::kBlockSize;
    constexpr int blocks = 64;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    ctx.keep (session.deviceCaptureChannels);
    ctx.keep (session.countInEnabled);
    session.deviceCaptureChannels.store (channels);
    session.countInEnabled.store (false);
    for (int index = 0; index < channels; ++index)
    {
        auto& track = session.track (index);
        track.mode.store ((int) Track::Mode::Mono);
        track.inputSource.store (channels - 1 - index);
        track.printEffects.store (false);
        session.setTrackArmed (index, true);
    }
    std::array<std::array<float, frames>, channels> input {};
    std::array<const float*, channels> pointers {};
    std::array<float, frames> left {}, right {};
    float* outputs[] { left.data(), right.data() };
    for (std::size_t channel = 0; channel < input.size(); ++channel)
    {
        pointers[channel] = input[channel].data();
        for (std::size_t frame = 0; frame < input[channel].size(); ++frame)
            input[channel][frame] = static_cast<float> (channel + 1) * 0.05f
                                  * (frame % 32 < 16 ? 1.0f : -1.0f);
    }
    engine.getTransport().setPlayhead (0);
    engine.record();
    if (! ctx.expect (engine.getTransport().isRecording(), "eight armed tracks did not start recording"))
        return ctx.verdict();
    for (int block = 0; block < blocks; ++block)
        engine.audioDeviceIOCallback (pointers.data(), channels, outputs, 2, frames, {});
    engine.stop();
    for (int index = 0; index < Session::kNumTracks; ++index)
    {
        const auto& regions = session.track (index).regions;
        if (index >= channels)
        {
            ctx.expect (regions.empty(), "an unarmed track acquired a recording");
            continue;
        }
        if (! ctx.expect (regions.size() == 1, "an armed track did not commit exactly one take")) continue;
        const auto& region = regions.front();
        ctx.expect (region.timelineStart == 0 && region.lengthInSamples == frames * blocks,
                    "simultaneous takes have different starts or lengths");
        auto reader = dusk::audio::FileReader::open (
            std::filesystem::u8path (region.file.getFullPathName().toStdString()));
        if (! ctx.expect (reader != nullptr, "a committed take has no readable WAV")) continue;
        ctx.expect (reader->info().numChannels == 1 && reader->info().numFrames == frames * blocks,
                    "the recorded WAV has the wrong channel count or duration");
        std::array<float, frames> recorded {};
        float* destination[] { recorded.data() };
        ctx.expect (reader->read (destination, 1, frames * 8, frames) == frames, "the WAV was truncated");
        for (std::size_t frame = 0; frame < recorded.size(); ++frame)
            if (! ctx.expect (std::abs (recorded[frame] - input[static_cast<std::size_t> (channels - 1 - index)][frame]) < 1.0e-5f,
                              "a take contains another input's signal")) break;
    }
    return ctx.verdict();
}

ScenarioResult midiRecordingLivesInJson (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (0);
    const int input = engine.getVirtualKeyboardInputIndex();
    if (! ctx.expect (input >= 0, "the virtual MIDI input is missing")) return ctx.verdict();
    ctx.keep (session.countInEnabled);
    session.countInEnabled.store (false);
    track.mode.store ((int) Track::Mode::Midi);
    track.midiInputIndex.store (input);
    track.midiChannel.store (0);
    track.inputMonitor.store (true);
    session.setTrackArmed (0, true);
    engine.record();
    if (! ctx.expect (engine.getTransport().isRecording(), "MIDI recording did not start")) return ctx.verdict();
    ctx.pump (4);
    dusk::MidiBuffer start;
    const std::uint8_t noteOn[] { 0x92, 60, 101 };
    const std::uint8_t controller[] { 0xb2, 74, 87 };
    start.addEvent (noteOn, 3, 0);
    start.addEvent (controller, 3, 64);
    ctx.pumpWithMidi (input, std::move (start));
    ctx.pump (16);
    dusk::MidiBuffer finish;
    const std::uint8_t noteOff[] { 0x82, 60, 0 };
    finish.addEvent (noteOff, 3, 0);
    ctx.pumpWithMidi (input, std::move (finish));
    ctx.pump (4);
    engine.stop();
    const auto& regions = track.midiRegions.current();
    if (! ctx.expect (regions.size() == 1 && regions[0].notes.size() == 1 && regions[0].ccs.size() == 1,
                      "MIDI recording did not commit the note and controller")) return ctx.verdict();
    const auto recorded = regions[0];
    ctx.expect (recorded.notes[0].channel == 3 && recorded.notes[0].noteNumber == 60
                && recorded.notes[0].velocity == 101 && recorded.notes[0].lengthInTicks > 0,
                "the recorded note changed channel, pitch, velocity or duration");
    ctx.expect (recorded.ccs[0].channel == 3 && recorded.ccs[0].controller == 74 && recorded.ccs[0].value == 87,
                "the recorded controller changed its identity or value");
    const auto path = ctx.sessionDir() / "session.json";
    if (! ctx.expect (SessionSerializer::save (session, path), "could not save the MIDI take")) return ctx.verdict();
    std::ifstream stream (path);
    const auto json = dusk::json::Json::parse (stream, nullptr, false);
    if (! ctx.expect (! json.is_discarded(), "the saved session is not valid JSON")) return ctx.verdict();
    const auto& embedded = json.at ("tracks").at (0).at ("midi_regions").at (0);
    ctx.expect (embedded.at ("notes").at (0).at ("note") == 60
                && embedded.at ("ccs").at (0).at ("ctrl") == 74,
                "the MIDI events were not embedded in session.json");
    Session loaded;
    if (! ctx.expect (SessionSerializer::load (loaded, path), "could not reload the MIDI session")) return ctx.verdict();
    const auto& restored = loaded.track (0).midiRegions.current();
    ctx.expect (restored.size() == 1 && restored[0].notes == recorded.notes && restored[0].ccs == recorded.ccs
                && restored[0].timelineStart == recorded.timelineStart
                && restored[0].lengthInTicks == recorded.lengthInTicks,
                "saving and reloading changed the recorded MIDI data");
    for (const auto& entry : std::filesystem::recursive_directory_iterator (ctx.sessionDir()))
        ctx.expect (entry.path().extension() != ".mid" && entry.path().extension() != ".wav",
                    "MIDI recording created an external media file");
    return ctx.verdict();
}

using Span = std::pair<std::int64_t, std::int64_t>;

std::string spansText (const std::vector<Span>& spans)
{
    std::string text;
    for (const auto& span : spans)
        text += "[" + std::to_string (span.first) + ", " + std::to_string (span.second) + ") ";
    return text.empty() ? "none" : text;
}

// Promoting part of a take puts exactly that span of it on the timeline and
// carves the region under it; undo and redo swap the whole layout back and forth.
ScenarioResult promoteRangeReplacesThatSpan (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto& track = ctx.session().track (kTrack);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();

    const auto first = seedRegion (ctx, 0, 48000, 0);
    const auto second = addTake (ctx, 0, 48000, 500);

    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new PromoteTakeRangeAction (ctx.session(), ctx.engine(), kTrack,
                                                               second, 12000, 24000)),
                      "promoting a range of the second take was refused"))
        return ctx.verdict();

    const auto* promoted = regionStartingAt (ctx, 12000);
    if (ctx.expect (promoted != nullptr, "no region starts where the promoted range does"))
        ctx.expect (promoted->takeId == second && promoted->lengthInSamples == 12000
                        && promoted->sourceOffset == 500 + 12000,
                    "the promoted region is not the second take's audio over the range");

    const auto secondSpans = takeCoverage (track, second);
    const auto firstSpans = takeCoverage (track, first);
    ctx.note ("first take plays " + spansText (firstSpans) + "; second plays " + spansText (secondSpans));
    ctx.expect (secondSpans == std::vector<Span> { { 12000, 24000 } },
                "the second take does not play exactly the promoted range");
    ctx.expect (firstSpans == std::vector<Span> { { 0, 12000 + kPunchFade }, { 24000 - kPunchFade, 48000 } },
                "the first take does not play everything outside the range up to the seams");
    ctx.expect (track.takes.size() == 2 && takeIsWhole (ctx, first, 0, 48000, 0)
                    && takeIsWhole (ctx, second, 0, 48000, 500),
                "promoting a range changed a take");

    ctx.expect (undo.undo() && regs.size() == 1 && regs[0].takeId == first
                    && regs[0].timelineStart == 0 && regs[0].lengthInSamples == 48000,
                "undo did not put the first take's region back whole");
    ctx.expect (takeCoverage (track, second).empty(), "undo left the second take on the timeline");
    ctx.expect (undo.redo() && takeCoverage (track, second) == secondSpans
                    && takeCoverage (track, first) == firstSpans,
                "redo did not bring the promoted layout back");

    auto& frozen = track.frozen;
    ctx.keep (frozen);
    frozen.store (true);
    const auto regionCount = regs.size();
    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new PromoteTakeRangeAction (ctx.session(), ctx.engine(), kTrack,
                                                           first, 30000, 36000)),
                "a promote on a frozen track was carried out");
    ctx.expect (regs.size() == regionCount, "a refused promote changed the regions");
    frozen.store (false);

    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new PromoteTakeRangeAction (ctx.session(), ctx.engine(), kTrack,
                                                           second, 60000, 70000)),
                "a promote outside the take was carried out");

    for (auto& r : regs)
        if (r.timelineStart == 0) r.locked = true;
    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new PromoteTakeRangeAction (ctx.session(), ctx.engine(), kTrack,
                                                           second, 6000, 9000)),
                "a promote that would carve a locked region was carried out");
    ctx.expect (regs.size() == regionCount && takeCoverage (track, first) == firstSpans,
                "a promote refused over a locked region changed the regions");
    return ctx.verdict();
}

// Deleting a take takes every region cut from it off the timeline; undo puts the
// take back at its place in recording order and the regions back as they were.
ScenarioResult deleteTakeRemovesItsRegions (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto& track = ctx.session().track (kTrack);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();

    const auto first = seedRegion (ctx, 0, 24000, 0);
    const auto second = addTake (ctx, 0, 48000, 0);
    const auto third = seedRegion (ctx, 30000, 9600, 0);
    promoteTakeRange (track, *findTake (track, second), 6000, 12000);
    promoteTakeRange (track, *findTake (track, second), 26000, 28000);
    const auto takesBefore = track.takes;
    const auto regionsBefore = regs;
    if (! ctx.expect (takeCoverage (track, second).size() == 2, "the setup did not put the second take on the timeline twice"))
        return ctx.verdict();

    const auto firstOfSecond = std::find_if (regs.begin(), regs.end(),
                                             [second] (const AudioRegion& r) { return r.takeId == second; });
    firstOfSecond->locked = true;
    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new DeleteTakeAction (ctx.session(), ctx.engine(), kTrack, second)),
                "deleting a take with a locked region on the timeline was carried out");
    ctx.expect (findTake (track, second) != nullptr && regs.size() == regionsBefore.size(),
                "a refused take delete changed the track");
    firstOfSecond->locked = false;

    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new DeleteTakeAction (ctx.session(), ctx.engine(), kTrack, second)),
                      "deleting the second take was refused"))
        return ctx.verdict();
    ctx.expect (findTake (track, second) == nullptr && track.takes.size() == 2
                    && track.takes[0].id == first && track.takes[1].id == third,
                "the delete did not remove exactly the second take");
    ctx.expect (std::none_of (regs.begin(), regs.end(), [second] (const AudioRegion& r) { return r.takeId == second; }),
                "a region cut from the deleted take is still on the timeline");
    ctx.expect (regs.size() == regionsBefore.size() - 2, "the delete removed regions of other takes");

    ctx.expect (undo.undo(), "undo of the take delete was refused");
    bool sameTakes = track.takes.size() == takesBefore.size();
    for (std::size_t i = 0; sameTakes && i < takesBefore.size(); ++i)
        sameTakes = track.takes[i].id == takesBefore[i].id && track.takes[i].name == takesBefore[i].name
                 && track.takes[i].timelineStart == takesBefore[i].timelineStart
                 && track.takes[i].lengthInSamples == takesBefore[i].lengthInSamples;
    ctx.expect (sameTakes, "undo did not put the take back with its id at its place in recording order");
    bool sameRegions = regs.size() == regionsBefore.size();
    for (std::size_t i = 0; sameRegions && i < regionsBefore.size(); ++i)
        sameRegions = regs[i].takeId == regionsBefore[i].takeId
                   && regs[i].timelineStart == regionsBefore[i].timelineStart
                   && regs[i].lengthInSamples == regionsBefore[i].lengthInSamples
                   && regs[i].sourceOffset == regionsBefore[i].sourceOffset;
    ctx.expect (sameRegions, "undo did not put the take's regions back as they were");

    ctx.expect (undo.redo() && findTake (track, second) == nullptr && track.takes.size() == 2,
                "redo did not delete the take again");
    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new DeleteTakeAction (ctx.session(), ctx.engine(), kTrack, second)),
                "deleting a take that is already gone was carried out");
    return ctx.verdict();
}

// Deleting the auditioned take, or cloning another track over its track, ends
// the audition; undo brings the take back without auditioning it again.
ScenarioResult auditionEndsWhenItsTakeGoes (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (kTrack);
    auto& undo = engine.getUndoManager();
    undo.clearUndoHistory();
    const auto first = seedRegion (ctx, 0, 24000, 0);
    const auto second = addTake (ctx, 0, 24000, 0);
    auto auditioning = [&session] (TakeId id)
    {
        return session.takeAudition.trackIdx == kTrack && session.takeAudition.takeId == id;
    };
    auto nothingAuditioned = [&session]
    {
        return session.takeAudition.trackIdx == -1 && session.takeAudition.takeId == 0;
    };

    engine.setTakeAudition (kTrack, first);
    engine.setTakeAudition (kTrack, second + 1000);
    ctx.expect (nothingAuditioned(), "an audition of a take the track lacks was kept");

    engine.setTakeAudition (kTrack, first);
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteTakeAction (session, engine, kTrack, second)),
                "deleting a take that is not auditioned was refused");
    ctx.expect (auditioning (first), "deleting another take ended the audition");
    ctx.expect (undo.undo() && findTake (track, second) != nullptr, "undo did not bring the take back");

    engine.setTakeAudition (kTrack, second);
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteTakeAction (session, engine, kTrack, second)),
                "deleting the auditioned take was refused");
    ctx.expect (nothingAuditioned(), "deleting the auditioned take left the audition set");
    ctx.expect (undo.undo() && findTake (track, second) != nullptr,
                "undo did not bring the auditioned take back");
    ctx.expect (nothingAuditioned(), "undo brought the audition back with the take");

    engine.setTakeAudition (kTrack, second);
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new CloneTrackAction (session, engine, kTrack + 1, kTrack)),
                "cloning a track over the auditioned track was refused");
    ctx.expect (nothingAuditioned(), "a clone over the auditioned track left the audition set");
    ctx.expect (undo.undo() && findTake (track, second) != nullptr && nothingAuditioned(),
                "undoing the clone brought the audition back");
    return ctx.verdict();
}

// A renamed take keeps its new name through save and load, and an undone
// rename saves the old one.
ScenarioResult renameTakeRoundTrips (ScenarioContext& ctx)
{
    auto& track = ctx.session().track (kTrack);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);
    if (! recordSpan (ctx, 4800, 9600)) return ctx.verdict();
    if (! ctx.expect (track.takes.size() == 1, "the recording did not add a take")) return ctx.verdict();
    const auto id = track.takes[0].id;
    const auto original = track.takes[0].name;

    const std::string renamed = "Lead vocal \xE2\x80\x93 keeper";
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new RenameTakeAction (ctx.session(), kTrack, id, renamed)),
                "the rename was refused");
    ctx.expect (track.takes[0].name == renamed, "the rename did not change the take's name");

    auto nameAfterReload = [&ctx, id] (const char* file) -> std::string
    {
        const auto path = ctx.sessionDir() / file;
        if (! ctx.expect (SessionSerializer::save (ctx.session(), path), "could not save the session"))
            return {};
        auto loaded = std::make_unique<Session>();
        if (! ctx.expect (SessionSerializer::load (*loaded, path), "could not reload the session"))
            return {};
        const auto* take = findTake (loaded->track (kTrack), id);
        return take != nullptr ? take->name : std::string ("<no take with that id>");
    };

    const auto reloaded = nameAfterReload ("renamed.json");
    ctx.note ("reloaded name: " + reloaded);
    ctx.expect (reloaded == renamed, "the renamed take did not keep its name through save and load");

    ctx.expect (undo.undo() && track.takes[0].name == original, "undo did not restore the take's name");
    ctx.expect (nameAfterReload ("undone.json") == original, "the undone rename did not save the old name");

    undo.beginNewTransaction();
    ctx.expect (! undo.perform (new RenameTakeAction (ctx.session(), kTrack, id + 1000, "nobody")),
                "renaming a take the track lacks was carried out");
    return ctx.verdict();
}

// The render API takes the framework's file type; named through Session's own
// getter so this file stays free of the framework header.
using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

bool writeMono (const std::filesystem::path& path, const std::vector<float>& samples)
{
    dusk::audio::WriteSpec spec;
    spec.sampleRate = ScenarioContext::kSampleRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return writer != nullptr && writer->write (channels, 1, (std::int64_t) samples.size())
        && writer->flush();
}

float peakOf (const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (const float s : samples) peak = std::max (peak, std::abs (s));
    return peak;
}

struct AuditionBounce
{
    std::unique_ptr<BounceEngine> bounce;
    std::atomic<bool> finished { false };
    bool ok = false;
    std::string error;
};

// The track plays a tone from its region and has a silent take over the same
// span. Auditioning the silent take silences the track's playback, and a
// master bounce taken during the audition still prints the tone.
std::optional<ScenarioResult> auditionNeverReachesBounce (ScenarioContext& ctx)
{
    constexpr int kTrackIdx = 0;
    constexpr std::int64_t kStart = 12000;
    constexpr int kLength = 24000;
    constexpr int kProbe = ScenarioContext::kBlockSize;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& track = session.track (kTrackIdx);

    std::vector<float> tone ((std::size_t) kLength);
    for (int i = 0; i < kLength; ++i)
        tone[(std::size_t) i] = 0.25f * std::sin (2.0f * 3.14159265f * 440.0f * (float) i
                                                  / (float) ScenarioContext::kSampleRate);
    const auto toneFile = ctx.tempDir() / "tone.wav";
    const auto silentFile = ctx.tempDir() / "silent.wav";
    if (! writeMono (toneFile, tone) || ! writeMono (silentFile, std::vector<float> ((std::size_t) kLength, 0.0f)))
        return ScenarioResult::fail ("could not write the source takes");

    AudioTake toneTake;
    toneTake.id = session.allocateTakeId();
    toneTake.file = SessionFile (toneFile.u8string().c_str());
    toneTake.timelineStart = kStart;
    toneTake.lengthInSamples = kLength;
    AudioTake silentTake = toneTake;
    silentTake.id = session.allocateTakeId();
    silentTake.file = SessionFile (silentFile.u8string().c_str());
    track.takes = { toneTake, silentTake };
    track.regions.push_back (*regionFromTake (toneTake, kStart, kStart + kLength));

    // Loop over the region so preparePlayback fills the loop-start cache on
    // this thread and a read right after it sees real samples.
    auto& transport = engine.getTransport();
    transport.setPlayhead (kStart);
    transport.setLoopRange (kStart, kStart + kLength);
    transport.setLoopEnabled (true);
    std::vector<float> probe ((std::size_t) kProbe);
    auto trackPeak = [&]
    {
        engine.getPlaybackEngine().readForTrack (kTrackIdx, kStart + 1000, probe.data(), nullptr, kProbe,
                                                 kStart, kStart + kLength);
        return peakOf (probe);
    };

    engine.clearTakeAudition();
    const float regionPeak = trackPeak();
    engine.setTakeAudition (kTrackIdx, silentTake.id);
    const float auditionPeak = trackPeak();
    ctx.note ("track peak from the region " + std::to_string (regionPeak) + ", auditioning the silent take "
              + std::to_string (auditionPeak));
    if (! ctx.expect (regionPeak > 0.1f, "the region does not play before the audition")
        || ! ctx.expect (auditionPeak < 1.0e-6f, "the audition did not replace the region with the silent take"))
        return ctx.verdict();
    transport.setLoopEnabled (false);
    transport.setLoopRange (0, 0);
    transport.setPlayhead (0);

    auto run = std::make_shared<AuditionBounce>();
    run->bounce = std::make_unique<BounceEngine> (engine, session);
    ctx.cleanup ([run] { run->bounce.reset(); });
    run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
    {
        raw->ok = ok;
        raw->error = std::move (error);
        raw->finished.store (true, std::memory_order_release);
    };
    const auto out = ctx.sessionDir() / "bounce.wav";
    if (! run->bounce->start (SessionFile (out.u8string().c_str()), ScenarioContext::kSampleRate, 1024, 1.0,
                              BounceEngine::Mode::MasterMix, BounceEngine::Format::Wav, 320, 24))
        return ScenarioResult::fail ("the bounce refused to start: " + run->bounce->getLastError());

    ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
                   90000,
                   [&ctx, run, out, silent = silentTake.id]
                   {
                       auto reader = dusk::audio::FileReader::open (out);
                       if (ctx.expect (run->ok, "the bounce failed: " + run->error)
                           && ctx.expect (reader != nullptr, "the bounce wrote no readable file"))
                       {
                           std::vector<float> left ((std::size_t) kLength - 512);
                           float* dest[] = { left.data() };
                           reader->read (dest, 1, kStart + 256, (std::int64_t) left.size());
                           const float printed = peakOf (left);
                           ctx.note ("bounce peak over the region " + std::to_string (printed));
                           ctx.expect (printed > 0.05f, "the bounce printed the auditioned silent take, not the region");
                       }
                       const auto& audition = ctx.session().takeAudition;
                       ctx.expect (audition.trackIdx == kTrackIdx && audition.takeId == silent,
                                   "the bounce changed the audition the user had set");
                       ctx.complete (ctx.verdict());
                   },
                   "the bounce never finished");
    return std::nullopt;
}

std::optional<ScenarioResult> run (ScenarioResult (*body) (ScenarioContext&), ScenarioContext& ctx)
{
    return body (ctx);
}

ScenarioResult secondTrackOverdub (ScenarioContext& ctx)
{
    constexpr int frames = ScenarioContext::kBlockSize;
    constexpr int blocks = 64;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    // Track 2 records from input 2, which a one-input default device does not
    // offer, and Session refuses to arm a track past the capture width.
    ctx.keep (session.deviceCaptureChannels);
    session.deviceCaptureChannels.store (2);
    session.countInEnabled.store (false);
    session.master().eqEnabled.store (false);
    session.master().compEnabled.store (false);
    session.master().tapeEnabled.store (false);
    for (int index = 0; index < 2; ++index)
    {
        auto& track = session.track (index);
        track.mode.store ((int) Track::Mode::Mono);
        track.inputSource.store (index);
        track.printEffects.store (false);
        track.inputMonitor.store (false);
    }
    std::array<float, frames> inputLeft {}, inputRight {}, outputLeft {}, outputRight {};
    const float* inputs[] { inputLeft.data(), inputRight.data() };
    float* outputs[] { outputLeft.data(), outputRight.data() };
    for (std::size_t frame = 0; frame < inputLeft.size(); ++frame)
        inputLeft[frame] = frame % 32 < 16 ? 0.1f : -0.1f;
    session.setTrackArmed (0, true);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "the first track did not start recording")) return ctx.verdict();
    for (int block = 0; block < blocks; ++block)
        engine.audioDeviceIOCallback (inputs, 2, outputs, 2, frames, {});
    engine.stop();
    if (! ctx.expect (session.track (0).regions.size() == 1, "the first take was not committed")) return ctx.verdict();
    const auto first = session.track (0).regions.front();
    session.setTrackArmed (0, false);
    session.setTrackArmed (1, true);
    inputLeft.fill (0.0f);
    transport.setPlayhead (0);
    transport.setLoopRange (0, frames * (blocks + 8));
    transport.setLoopEnabled (true);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "the overdub did not start recording")) return ctx.verdict();
    double playbackEnergy = 0.0;
    for (int block = 0; block < blocks; ++block)
    {
        for (std::size_t frame = 0; frame < inputRight.size(); ++frame)
            inputRight[frame] = block < blocks / 2 ? 0.0f : frame % 16 < 8 ? 0.2f : -0.2f;
        engine.audioDeviceIOCallback (inputs, 2, outputs, 2, frames, {});
        if (block >= 16 && block < blocks / 2)
            for (const float sample : outputLeft) playbackEnergy += static_cast<double> (sample) * sample;
    }
    engine.stop();
    ctx.expect (playbackEnergy > 1.0, "track 1 was not audible during the silent-input half of the overdub");
    ctx.expect (session.track (0).regions.size() == 1 && session.track (0).regions.front().file == first.file
                && session.track (0).regions.front().lengthInSamples == first.lengthInSamples,
                "overdubbing track 2 changed track 1's region");
    if (! ctx.expect (session.track (1).regions.size() == 1, "the overdub did not commit one take to track 2"))
        return ctx.verdict();
    ctx.expect (session.track (1).regions.front().file != first.file, "the overdub reused the original take's file");
    for (int index = 0; index < 2; ++index)
    {
        const auto& take = session.track (index).regions.front();
        ctx.expect (take.timelineStart == 0 && take.lengthInSamples == frames * blocks,
                    "the two tracks did not retain aligned take boundaries");
        auto reader = dusk::audio::FileReader::open (take.file.getFullPathName().toStdString());
        if (! ctx.expect (reader != nullptr, "a take has no readable WAV")) continue;
        std::array<float, frames> recorded {};
        float* destination[] { recorded.data() };
        for (const int block : { 16, 48 })
        {
            ctx.expect (reader->read (destination, 1, block * frames, frames) == frames, "the take was truncated");
            for (std::size_t frame = 0; frame < recorded.size(); ++frame)
            {
                const float expected = index == 0 ? (frame % 32 < 16 ? 0.1f : -0.1f)
                                     : block < blocks / 2 ? 0.0f : (frame % 16 < 8 ? 0.2f : -0.2f);
                if (! ctx.expect (std::abs (recorded[frame] - expected) < 1.0e-5f,
                                  "a take changed source or captured the other track's playback")) break;
            }
        }
    }
    return ctx.verdict();
}

const ScenarioRegistrar overdubRegistrar { Scenario {
    "record.second_track_overdub", { "record", "playback" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (secondTrackOverdub, ctx); } } };

const ScenarioRegistrar eightInputsRegistrar { Scenario {
    "record.eight_inputs_separate_takes", { "record" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (eightInputsRecordSeparately, ctx); } } };
const ScenarioRegistrar midiJsonRegistrar { Scenario {
    "record.midi_embedded_in_json", { "record", "midi", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (midiRecordingLivesInJson, ctx); } } };
const ScenarioRegistrar fullCoverRegistrar { Scenario {
    "take.full_cover_keeps_the_covered_take", { "take", "record", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (fullCoverKeepsTheCoveredTake, ctx); } } };
const ScenarioRegistrar edgeRegistrar { Scenario {
    "take.partial_overdub_keeps_covered_take_whole", { "take", "record", "punch" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (partialOverdubKeepsCoveredTakeWhole, ctx); } } };
const ScenarioRegistrar punchRegistrar { Scenario {
    "take.punch_inside_names_every_take", { "take", "record", "punch" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (punchInsideNamesEveryTake, ctx); } } };
const ScenarioRegistrar editsRegistrar { Scenario {
    "take.split_and_delete_keep_the_take", { "take", "record", "region", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (editsLeaveRecordedTakes, ctx); } } };
const ScenarioRegistrar undoRegistrar { Scenario {
    "take.undo_record_removes_its_take", { "take", "record", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (undoRecordRemovesItsTake, ctx); } } };
const ScenarioRegistrar promoteRegistrar { Scenario {
    "take.promote_range_replaces_that_span", { "take", "comp", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (promoteRangeReplacesThatSpan, ctx); } } };
const ScenarioRegistrar deleteTakeRegistrar { Scenario {
    "take.delete_take_removes_its_regions_and_undo_restores_ids", { "take", "comp", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (deleteTakeRemovesItsRegions, ctx); } } };
const ScenarioRegistrar auditionEndsRegistrar { Scenario {
    "take.audition_ends_when_its_take_goes", { "take", "comp", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (auditionEndsWhenItsTakeGoes, ctx); } } };
const ScenarioRegistrar renameTakeRegistrar { Scenario {
    "take.rename_take_round_trips_through_save", { "take", "session", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (renameTakeRoundTrips, ctx); } } };
const ScenarioRegistrar auditionRegistrar { Scenario {
    "take.audition_never_reaches_bounce", { "take", "bounce", "playback" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return auditionNeverReachesBounce (ctx); }, 120000 } };
} // namespace
} // namespace duskstudio::scenario
