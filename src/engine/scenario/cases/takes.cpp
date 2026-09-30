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
#include <functional>
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
constexpr int kTrack = 5;
// The click-mask crossfade a punch leaves against the region it cuts into.
constexpr std::int64_t kPunchFade = 64;

// The render API takes the framework's file type; named through Session's own
// getter so this file stays free of the framework header.
using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

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
                        && std::abs (kept->gainDb - before.gainDb) < 1.0e-6f,
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
    return ctx.verdict();
}

// Deleting a recorded region leaves its take on the track, and so does
// splitting one: both halves name the take.
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

// A region that names no take (an imported file, a reversed region, a 0.14
// recording) becomes a take of its own before a promote or a recording cuts
// into it, so no part of its audio is left out of every take. Undo removes that
// take again and ends an audition of it; redo brings it back under its id.
ScenarioResult carveAdoptsARegionNamingNoTake (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    auto& regs = track.regions;
    auto& undo = engine.getUndoManager();
    undo.clearUndoHistory();

    auto plain = regionAt (0, 48000, 200);
    plain.file = SessionFile ((ctx.tempDir() / "imported.wav").u8string().c_str());
    regs.push_back (plain);
    const auto promoted = addTake (ctx, 0, 48000, 500);

    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new PromoteTakeRangeAction (session, engine, kTrack, promoted, 12000, 24000)),
                      "promoting over a region naming no take was refused"))
        return ctx.verdict();
    if (! ctx.expect (track.takes.size() == 2 && track.takes[1].id == promoted,
                      "the region did not become a take ahead of the promoted one; "
                          + std::to_string (track.takes.size()) + " takes on the track"))
        return ctx.verdict();
    const auto adopted = track.takes[0];
    ctx.expect (adopted.id != 0 && adopted.file == plain.file && adopted.name == "Take 2",
                "the adopted take is not the region's file under a new take name");
    ctx.expect (takeIsWhole (ctx, adopted.id, 0, 48000, 200), "the adopted take is not the region as it stood");
    const std::vector<Span> adoptedSpans { { 0, 12000 + kPunchFade }, { 24000 - kPunchFade, 48000 } };
    ctx.note ("the adopted take plays " + spansText (takeCoverage (track, adopted.id)));
    ctx.expect (takeCoverage (track, adopted.id) == adoptedSpans,
                "the carved pieces of the region do not name the adopted take");

    engine.setTakeAudition (kTrack, adopted.id);
    ctx.expect (undo.undo(), "undo of the promote was refused");
    ctx.expect (track.takes.size() == 1 && track.takes[0].id == promoted, "undo left the adopted take on the track");
    ctx.expect (regs.size() == 1 && regs[0].takeId == 0 && regs[0].file == plain.file
                    && regs[0].timelineStart == 0 && regs[0].lengthInSamples == 48000,
                "undo did not put the region back whole, naming no take");
    ctx.expect (session.takeAudition.trackIdx == -1 && session.takeAudition.takeId == 0,
                "undo left the audition on the adopted take it removed");
    ctx.expect (undo.redo() && track.takes.size() == 2 && track.takes[0].id == adopted.id
                    && takeCoverage (track, adopted.id) == adoptedSpans,
                "redo did not bring the adopted take back under its id");
    ctx.expect (session.takeAudition.takeId == 0, "redo brought the audition back");

    undo.clearUndoHistory();
    regs.clear();
    track.takes.clear();
    auto older = regionAt (4800, 4800, 1000);
    older.file = SessionFile ((ctx.tempDir() / "older.wav").u8string().c_str());
    regs.push_back (older);
    armTrack (ctx);
    if (! recordSpan (ctx, 2400, 12000)) return ctx.verdict();
    if (! ctx.expect (track.takes.size() == 2, "recording over a region naming no take did not keep it as a take; "
                                                   + std::to_string (track.takes.size()) + " takes on the track"))
        return ctx.verdict();
    const auto kept = track.takes[0];
    ctx.expect (kept.file == older.file && takeIsWhole (ctx, kept.id, 4800, 4800, 1000) && kept.name == "Take 1",
                "the covered region's audio is not the first take, whole");
    ctx.expect (track.takes[1].name == "Take 2" && regs.size() == 1 && regs[0].takeId == track.takes[1].id,
                "the recording is not the second take on the timeline");
    ctx.expect (undo.undo() && track.takes.empty() && regs.size() == 1 && regs[0].takeId == 0
                    && regs[0].file == older.file,
                "undo of the recording did not remove both takes and put the region back naming none");
    ctx.expect (undo.redo() && track.takes.size() == 2 && track.takes[0].id == kept.id,
                "redo did not bring the kept take back under its id");
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
    promoteTakeRange (ctx.session(), track, *findTake (track, second), 6000, 12000);
    promoteTakeRange (ctx.session(), track, *findTake (track, second), 26000, 28000);
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
// Undoing the recording that added the auditioned take ends it too, and redo
// brings the take back without the audition.
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

    auto& source = session.track (kTrack + 1);
    AudioTake sourceTake;
    sourceTake.id = session.allocateTakeId();
    sourceTake.name = "Take 1";
    sourceTake.lengthInSamples = 24000;
    source.takes.push_back (sourceTake);
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new CloneTrackAction (session, engine, kTrack + 1, kTrack)),
                "cloning a track with a take was refused");
    const auto cloned = track.takes.size() == 1 ? track.takes[0].id : TakeId { 0 };
    engine.setTakeAudition (kTrack, cloned);
    ctx.expect (cloned != 0 && auditioning (cloned), "the cloned take could not be auditioned");
    ctx.expect (undo.undo() && findTake (track, cloned) == nullptr, "undoing the clone left the cloned take");
    ctx.expect (nothingAuditioned(), "undoing the clone left the audition on the cloned take it removed");
    source.takes.clear();

    armTrack (ctx);
    if (! recordSpan (ctx, 30000, 36000)) return ctx.verdict();
    if (! ctx.expect (! track.takes.empty(), "the recording left no take")) return ctx.verdict();
    const auto recorded = track.takes.back().id;
    engine.setTakeAudition (kTrack, recorded);
    ctx.expect (auditioning (recorded), "the recorded take could not be auditioned");
    ctx.expect (undo.undo() && findTake (track, recorded) == nullptr, "undo did not remove the recorded take");
    ctx.expect (nothingAuditioned(), "undoing the recording left the audition on its take");
    ctx.expect (undo.redo() && findTake (track, recorded) != nullptr, "redo did not bring the recorded take back");
    ctx.expect (nothingAuditioned(), "redo brought the audition back with the recorded take");
    return ctx.verdict();
}

std::vector<TakeId> takeIds (const Track& track)
{
    std::vector<TakeId> ids;
    for (const auto& take : track.takes)
        ids.push_back (take.id);
    return ids;
}

// A take can join the track with no undo step: a recording stopped by a stage
// switch or a lost device. Undoing an earlier promote, recording or clone swaps
// only the takes that step changed, and redo puts them back where they were.
ScenarioResult undoKeepsTakesAddedOutsideHistory (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& track = session.track (kTrack);
    auto& undo = engine.getUndoManager();
    undo.clearUndoHistory();

    auto plain = regionAt (0, 48000, 200);
    plain.file = SessionFile ((ctx.tempDir() / "imported.wav").u8string().c_str());
    track.regions.push_back (plain);
    const auto promoted = addTake (ctx, 0, 48000, 500);
    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new PromoteTakeRangeAction (session, engine, kTrack, promoted, 12000, 24000)),
                      "the promote was refused"))
        return ctx.verdict();
    if (! ctx.expect (track.takes.size() == 2, "the promote did not make the region a take"))
        return ctx.verdict();
    const auto adopted = track.takes[0].id;
    const auto outside = addTake (ctx, 0, 48000, 0);

    ctx.expect (undo.undo() && takeIds (track) == std::vector<TakeId> { promoted, outside },
                "undoing the promote touched a take added after it with no undo step");
    ctx.expect (undo.redo() && takeIds (track) == std::vector<TakeId> { adopted, promoted, outside },
                "redoing the promote did not put its take back where it was");

    undo.clearUndoHistory();
    track.regions.clear();
    track.takes.clear();
    armTrack (ctx);
    if (! recordSpan (ctx, 4800, 9600)) return ctx.verdict();
    if (! ctx.expect (track.takes.size() == 1, "the recording did not add one take"))
        return ctx.verdict();
    const auto recorded = track.takes[0].id;
    const auto later = addTake (ctx, 0, 48000, 0);
    ctx.expect (undo.undo() && takeIds (track) == std::vector<TakeId> { later },
                "undoing the recording touched a take added after it with no undo step");
    ctx.expect (undo.redo() && takeIds (track) == std::vector<TakeId> { recorded, later },
                "redoing the recording did not put its take back where it was");

    undo.clearUndoHistory();
    track.regions.clear();
    track.takes.clear();
    const auto replaced = addTake (ctx, 0, 48000, 0);
    auto& source = session.track (kTrack + 1);
    AudioTake sourceTake;
    sourceTake.id = session.allocateTakeId();
    sourceTake.name = "Take 1";
    sourceTake.lengthInSamples = 24000;
    source.takes.push_back (sourceTake);
    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new CloneTrackAction (session, engine, kTrack + 1, kTrack))
                          && track.takes.size() == 1 && track.takes[0].id != replaced,
                      "the clone did not replace the destination's take with a copy"))
        return ctx.verdict();
    const auto copy = track.takes[0].id;
    const auto afterClone = addTake (ctx, 0, 48000, 0);
    ctx.expect (undo.undo() && takeIds (track) == std::vector<TakeId> { replaced, afterClone },
                "undoing the clone touched a take added after it with no undo step");
    ctx.expect (undo.redo() && takeIds (track) == std::vector<TakeId> { copy, afterClone },
                "redoing the clone touched a take added after it with no undo step");
    source.takes.clear();
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

struct AuditionRender
{
    std::unique_ptr<BounceEngine> bounce;
    std::atomic<bool> finished { false };
    std::atomic<bool> ok { false };
    std::string error;
    std::filesystem::path printed;
};

enum class AuditionLeg { RealtimeMix, Mix, Stems, Freeze };

std::string legName (AuditionLeg leg)
{
    switch (leg)
    {
        case AuditionLeg::RealtimeMix: return "the realtime mixdown";
        case AuditionLeg::Mix:         return "the mixdown";
        case AuditionLeg::Stems:       return "the stems bounce";
        case AuditionLeg::Freeze:      return "the freeze";
    }
    return {};
}

std::filesystem::path pathOf (const SessionFile& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

// The track plays a tone from its region and has a silent take over the same
// span. Auditioning the silent take silences the track's playback, and a
// realtime mixdown, an offline mixdown, a stems bounce and a freeze taken during
// the audition all still print the tone. The realtime pass is clocked by
// pumped blocks, as a device would clock it.
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

    const std::vector<AuditionLeg> legs { AuditionLeg::RealtimeMix, AuditionLeg::Mix,
                                          AuditionLeg::Stems, AuditionLeg::Freeze };
    auto next = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakNext = next;
    *next = [&ctx, &engine, &session, legs, weakNext, silent = silentTake.id] (std::size_t index)
    {
        if (index >= legs.size())
        {
            ctx.complete (ctx.verdict());
            return;
        }
        const auto leg = legs[index];
        const auto label = legName (leg);
        auto run = std::make_shared<AuditionRender>();
        run->bounce = std::make_unique<BounceEngine> (engine, session);
        ctx.cleanup ([run] { run->bounce.reset(); });
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        {
            raw->error = std::move (error);
            raw->ok.store (ok);
            raw->finished.store (true, std::memory_order_release);
        };

        const auto out = ctx.sessionDir() / ("audition-" + std::to_string (index) + ".wav");
        const SessionFile outFile (out.u8string().c_str());
        const bool realtime = leg == AuditionLeg::RealtimeMix;
        bool started = false;
        switch (leg)
        {
            case AuditionLeg::RealtimeMix:
            case AuditionLeg::Mix:
                run->printed = out;
                started = run->bounce->start (outFile, ScenarioContext::kSampleRate, 1024, 1.0,
                                              BounceEngine::Mode::MasterMix, BounceEngine::Format::Wav,
                                              320, 24, realtime);
                break;
            case AuditionLeg::Stems:
                run->printed = pathOf (BounceEngine::stemOutputFile (
                    outFile, kTrackIdx, session.track (kTrackIdx).name.toStdString()));
                started = run->bounce->start (outFile, ScenarioContext::kSampleRate, 1024, 1.0,
                                              BounceEngine::Mode::Stems, BounceEngine::Format::Wav, 320, 24);
                break;
            case AuditionLeg::Freeze:
            {
                SessionFile frozen;
                std::int64_t length = 0;
                if (! ctx.expect (engine.freezePrepare (kTrackIdx, frozen, length),
                                  "freeze refused: " + engine.getLastFreezeError().toStdString()))
                {
                    ctx.complete (ctx.verdict());
                    return;
                }
                run->printed = pathOf (frozen);
                started = run->bounce->startFreeze (kTrackIdx, frozen, length, ScenarioContext::kSampleRate);
                break;
            }
        }
        if (! ctx.expect (started, label + " refused to start: " + run->bounce->getLastError()))
        {
            ctx.complete (ctx.verdict());
            return;
        }

        auto self = weakNext.lock();
        ctx.waitUntil ([&ctx, run, realtime]
                       {
                           if (realtime) ctx.pump (8);
                           return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering();
                       },
                       90000,
                       [&ctx, &engine, run, label, realtime, silent, index, self]
                       {
                           auto reader = dusk::audio::FileReader::open (run->printed);
                           if (ctx.expect (run->ok.load(), label + " failed: " + run->error)
                               && ctx.expect (reader != nullptr, label + " wrote no readable file"))
                           {
                               std::vector<float> left ((std::size_t) kLength - 512);
                               float* dest[] = { left.data() };
                               reader->read (dest, 1, kStart + 256, (std::int64_t) left.size());
                               const float printed = peakOf (left);
                               ctx.note (label + " peak over the region " + std::to_string (printed));
                               ctx.expect (printed > 0.05f, label + " printed the auditioned silent take, not the region");
                           }
                           const auto& audition = ctx.session().takeAudition;
                           ctx.expect (audition.trackIdx == kTrackIdx && audition.takeId == silent,
                                       label + " changed the audition the user had set");
                           // An offline render hands the engine back to the device,
                           // whose restart re-prepares it; this world has no device.
                           if (! realtime)
                               engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
                           (*self) (index + 1);
                       },
                       label + " never finished");
    };
    (*next) (0);
    return std::nullopt;
}

// A take on the track whose file really exists, a quiet ramp, so an edit that
// renders can read it.
std::optional<AudioTake> writtenTake (ScenarioContext& ctx, const std::string& name, std::int64_t start, int length)
{
    std::vector<float> ramp ((std::size_t) length);
    for (int i = 0; i < length; ++i)
        ramp[(std::size_t) i] = 0.5f * (float) i / (float) length;
    const auto path = ctx.tempDir() / (name + ".wav");
    if (! writeMono (path, ramp)) return std::nullopt;
    AudioTake take;
    take.id = ctx.session().allocateTakeId();
    take.name = name;
    take.file = SessionFile (path.u8string().c_str());
    take.timelineStart = start;
    take.lengthInSamples = length;
    ctx.session().track (kTrack).takes.push_back (take);
    return take;
}

// Reverse renders the region into a file of its own, which is no take's: the
// result names no take, the take's lane stops showing it, and deleting the take
// leaves it. Undo brings the region back naming its take.
ScenarioResult reverseNamesNoTake (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (kTrack);
    auto& regs = track.regions;
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    const auto take = writtenTake (ctx, "reverse-take", 4800, 9600);
    if (! take) return ScenarioResult::fail ("could not write the take");
    regs.push_back (*regionFromTake (*take, 4800, 14400));

    undo.beginNewTransaction();
    if (! ctx.expect (undo.perform (new ReverseRegionAction (session, ctx.engine(), kTrack, 0)), "the reverse was refused")
        || ! ctx.expect (regs.size() == 1, "the reverse changed the number of regions"))
        return ctx.verdict();
    ctx.expect (regs[0].file != take->file, "the reverse did not render a file of its own");
    ctx.expect (regs[0].takeId == 0, "the reversed region still names the take it was rendered from");
    ctx.expect (takeCoverage (track, take->id).empty(), "the take's lane still shows the reversed region");

    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteTakeAction (session, ctx.engine(), kTrack, take->id)),
                "deleting the take was refused");
    ctx.expect (regs.size() == 1 && findTake (track, take->id) == nullptr,
                "deleting the take took the reversed region with it");

    ctx.expect (undo.undo() && undo.undo(), "undo of the delete and the reverse was refused");
    ctx.expect (regs.size() == 1 && regs[0].file == take->file && regs[0].takeId == take->id,
                "undoing the reverse did not bring the region back naming its take");
    return ctx.verdict();
}

// Join keeps a take only while the result still reads that take alone: two
// halves of one take rejoin naming it, two loop passes that share a file and
// abut join naming neither, and regions of two takes render into a new file
// that names none.
ScenarioResult joinNamesATakeOnlyWhileItReadsIt (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& regs = regionsOf (ctx);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    const auto first = writtenTake (ctx, "join-first", 0, 9600);
    const auto second = writtenTake (ctx, "join-second", 0, 9600);
    if (! first || ! second) return ScenarioResult::fail ("could not write the takes");

    const auto join = [&] (const std::string& what)
    {
        undo.beginNewTransaction();
        return ctx.expect (undo.perform (new JoinRegionsAction (session, ctx.engine(), kTrack, { 0, 1 })),
                           "joining " + what + " was refused")
            && ctx.expect (regs.size() == 1, "joining " + what + " did not leave one region");
    };

    regs = { *regionFromTake (*first, 0, 4800), *regionFromTake (*first, 4800, 9600) };
    if (join ("two halves of one take"))
        ctx.expect (regs[0].file == first->file && regs[0].takeId == first->id,
                    "two halves of one take did not rejoin naming it");

    AudioTake passOne = *first;
    passOne.id = session.allocateTakeId();
    passOne.timelineStart = 20000;
    passOne.lengthInSamples = 4800;
    AudioTake passTwo = passOne;
    passTwo.id = session.allocateTakeId();
    passTwo.sourceOffset = 4800;
    session.track (kTrack).takes.push_back (passOne);
    session.track (kTrack).takes.push_back (passTwo);
    auto moved = *regionFromTake (passTwo, 20000, 24800);
    moved.timelineStart = 24800;
    regs = { *regionFromTake (passOne, 20000, 24800), moved };
    if (join ("two loop passes of one file"))
        ctx.expect (regs[0].file == first->file && regs[0].takeId == 0,
                    "a join across two loop passes named one of them");

    regs = { *regionFromTake (*first, 0, 4800), *regionFromTake (*second, 4800, 9600) };
    if (join ("regions of two takes"))
        ctx.expect (regs[0].file != first->file && regs[0].file != second->file && regs[0].takeId == 0,
                    "a join rendered from two takes names one of them");
    return ctx.verdict();
}

// A pasted copy names its take only while the take is on the track and the copy
// reads its file: a copy on the take's own track keeps it, and a copy on another
// track, a copy reading another file and a copy pasted after its take was
// deleted do not.
ScenarioResult pasteKeepsTheTakeWhileItReadsIt (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& regs = regionsOf (ctx);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    const auto take = writtenTake (ctx, "paste-take", 0, 9600);
    if (! take) return ScenarioResult::fail ("could not write the take");
    regs.push_back (*regionFromTake (*take, 0, 9600));
    auto copy = regs[0];
    copy.timelineStart = 20000;

    const auto paste = [&] (int track, const AudioRegion& region, const std::string& what) -> const AudioRegion*
    {
        undo.beginNewTransaction();
        auto& target = session.track (track).regions;
        const auto before = target.size();
        if (! ctx.expect (undo.perform (new PasteRegionAction (session, ctx.engine(), track, region)),
                          "pasting " + what + " was refused")
            || ! ctx.expect (target.size() == before + 1, "pasting " + what + " added no region"))
            return nullptr;
        return &target.back();
    };

    if (const auto* pasted = paste (kTrack, copy, "a copy on its own track"))
        ctx.expect (pasted->takeId == take->id, "a copy pasted on its take's track lost the take");

    if (const auto* pasted = paste (kTrack + 1, copy, "a copy on another track"))
        ctx.expect (pasted->takeId == 0, "a copy pasted on a track without the take still names it");

    auto elsewhere = copy;
    elsewhere.file = SessionFile ((ctx.tempDir() / "elsewhere.wav").u8string().c_str());
    if (const auto* pasted = paste (kTrack, elsewhere, "a copy reading another file"))
        ctx.expect (pasted->takeId == 0, "a copy reading another file names the take");

    undo.beginNewTransaction();
    ctx.expect (undo.perform (new DeleteTakeAction (session, ctx.engine(), kTrack, take->id)),
                "deleting the take was refused");
    ctx.expect (regs.size() == 1 && regs[0].file == elsewhere.file,
                "deleting the take did not remove exactly the original and the copy that names it");
    if (const auto* pasted = paste (kTrack, copy, "a copy after its take was deleted"))
        ctx.expect (pasted->takeId == 0, "a copy pasted after its take was deleted still names it");
    return ctx.verdict();
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
const ScenarioRegistrar adoptRegistrar { Scenario {
    "take.carve_adopts_a_region_naming_no_take", { "take", "comp", "record", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (carveAdoptsARegionNamingNoTake, ctx); } } };
const ScenarioRegistrar deleteTakeRegistrar { Scenario {
    "take.delete_take_removes_its_regions_and_undo_restores_ids", { "take", "comp", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (deleteTakeRemovesItsRegions, ctx); } } };
const ScenarioRegistrar auditionEndsRegistrar { Scenario {
    "take.audition_ends_when_its_take_goes", { "take", "comp", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (auditionEndsWhenItsTakeGoes, ctx); } } };
const ScenarioRegistrar undoKeepsOutsideTakesRegistrar { Scenario {
    "take.undo_keeps_takes_added_outside_history", { "take", "comp", "record", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (undoKeepsTakesAddedOutsideHistory, ctx); } } };
const ScenarioRegistrar renameTakeRegistrar { Scenario {
    "take.rename_take_round_trips_through_save", { "take", "session", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (renameTakeRoundTrips, ctx); } } };
const ScenarioRegistrar reverseRegistrar { Scenario {
    "take.reverse_names_no_take", { "take", "region", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (reverseNamesNoTake, ctx); } } };
const ScenarioRegistrar joinRegistrar { Scenario {
    "take.join_names_a_take_only_while_it_reads_it", { "take", "region", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (joinNamesATakeOnlyWhileItReadsIt, ctx); } } };
const ScenarioRegistrar pasteRegistrar { Scenario {
    "take.paste_keeps_the_take_while_it_reads_it", { "take", "region", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (pasteKeepsTheTakeWhileItReadsIt, ctx); } } };
const ScenarioRegistrar auditionRegistrar { Scenario {
    "take.audition_never_reaches_bounce", { "take", "bounce", "freeze", "playback" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return auditionNeverReachesBounce (ctx); }, 180000 } };
} // namespace
} // namespace duskstudio::scenario
