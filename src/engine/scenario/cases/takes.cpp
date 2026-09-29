#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../audiofile/FileReader.h"
#include "../../../session/RegionEditActions.h"
#include "../../../session/Session.h"
#include "../../../session/SessionSerializer.h"
#include "../../../foundation/Json.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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

// An existing region cut from a take of its own, the way a recording leaves one.
TakeId seedRegion (ScenarioContext& ctx, std::int64_t start, std::int64_t length, std::int64_t offset)
{
    auto& track = ctx.session().track (kTrack);
    AudioTake take;
    take.id = ctx.session().allocateTakeId();
    take.name = "Take " + std::to_string (track.takes.size() + 1);
    take.timelineStart = start;
    take.lengthInSamples = length;
    take.sourceOffset = offset;
    track.takes.push_back (take);
    auto region = regionAt (start, length, offset);
    region.takeId = take.id;
    track.regions.push_back (region);
    return take.id;
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
} // namespace
} // namespace duskstudio::scenario
