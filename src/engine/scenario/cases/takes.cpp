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
// RecordManager's cap on a region's take history.
constexpr int kMaxTakes = 8;
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

// A new take whose range fully contains an existing region pushes that region
// onto the new one's take stack; Undo straight after puts the old one back.
ScenarioResult fullCoverPushesOntoStack (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (4800, 4800, 1000));
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);

    if (! recordSpan (ctx, 2400, 12000))
        return ctx.verdict();

    if (ctx.expect (regs.size() == 1, "the covered region was not absorbed; "
                                          + std::to_string (regs.size()) + " regions on the track"))
    {
        const auto& live = regs[0];
        ctx.expect (live.timelineStart <= 4800 && endOf (live) >= 9600,
                    "the new take does not span the region it covered");
        ctx.expect (live.previousTakes.size() == 1
                        && live.previousTakes[0].sourceOffset == 1000
                        && live.previousTakes[0].lengthInSamples == 4800,
                    "the covered region is not the first take on the stack");
    }

    ctx.expect (undo.undo(), "undo after recording was refused");
    ctx.expect (regs.size() == 1 && regs[0].timelineStart == 4800 && regs[0].sourceOffset == 1000
                    && regs[0].lengthInSamples == 4800 && regs[0].previousTakes.empty(),
                "undo after recording did not bring the old region back as it was");
    ctx.expect (undo.redo() && regs.size() == 1 && regs[0].previousTakes.size() == 1,
                "redo did not bring the take back with its stack");
    return ctx.verdict();
}

// Overdubbing the same span again and again keeps the newest eight takes,
// newest first, and lets the oldest fall off the bottom.
ScenarioResult stackKeepsNewestEight (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (4800, 4800, 1000));
    armTrack (ctx);

    std::vector<decltype (AudioRegion::file)> liveFiles;
    for (int take = 0; take <= kMaxTakes; ++take)
    {
        if (! recordSpan (ctx, 2400, 12000)) return ctx.verdict();
        if (! ctx.expect (regs.size() == 1, "take " + std::to_string (take + 1)
                                                + " left more than one region on the track"))
            return ctx.verdict();
        liveFiles.push_back (regs[0].file);
    }

    const auto& takes = regs[0].previousTakes;
    if (ctx.expect ((int) takes.size() == kMaxTakes,
                    "the stack holds " + std::to_string (takes.size()) + " takes, not "
                        + std::to_string (kMaxTakes)))
    {
        for (int i = 0; i < kMaxTakes; ++i)
            ctx.expect (takes[(std::size_t) i].file == liveFiles[liveFiles.size() - 2 - (std::size_t) i],
                        "stack position " + std::to_string (i) + " is not the take recorded "
                            + std::to_string (i + 1) + " before the live one");
        for (const auto& take : takes)
            ctx.expect (take.sourceOffset != 1000, "the original region survived past the cap");
    }
    return ctx.verdict();
}

// A take that covers only one end of an older region trims it back to a
// crossfade instead of absorbing it, and keeps none of the covered audio.
ScenarioResult edgeOverdubTrimsNeighbour (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (4800, 4800));
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();
    armTrack (ctx);

    if (! recordSpan (ctx, 7200, 12000))
        return ctx.verdict();

    const auto* old = regionStartingAt (ctx, 4800);
    const AudioRegion* fresh = nullptr;
    for (const auto& r : regs)
        if (&r != old) fresh = &r;

    if (ctx.expect (regs.size() == 2 && old != nullptr && fresh != nullptr,
                    "an edge overdub should leave the old region and the new take side by side; "
                        + std::to_string (regs.size()) + " regions on the track"))
    {
        ctx.expect (endOf (*old) > fresh->timelineStart && endOf (*old) <= fresh->timelineStart + kPunchFade,
                    "the old region was not trimmed back to a crossfade under the new take");
        ctx.expect (fresh->previousTakes.empty(), "the covered end of the old region went onto the stack");
    }

    ctx.expect (undo.undo() && regs.size() == 1 && regs[0].timelineStart == 4800
                    && regs[0].lengthInSamples == 4800,
                "undo did not restore the whole old region");
    return ctx.verdict();
}

// A punch inside an older region splits it around the new take and keeps the
// covered stretch on the take stack, so Alt+T brings the old audio back.
ScenarioResult punchInsideKeepsCoveredSlice (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    regs.push_back (regionAt (4800, 19200));
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
    if (ctx.expect (fresh->previousTakes.size() == 1, "the covered stretch did not go onto the stack"))
    {
        const auto& slice = fresh->previousTakes[0];
        ctx.expect (slice.sourceOffset == fresh->timelineStart - 4800
                        && slice.lengthInSamples == fresh->lengthInSamples,
                    "the stacked slice is not the audio the new take covers");
    }
    return ctx.verdict();
}

// What TapeStrip does for Alt+T, Alt+Shift+T and the take badge: one cycle
// step per undo step.
bool cycleAudio (ScenarioContext& ctx, bool forward)
{
    const AudioRegion before = regionsOf (ctx)[0];
    AudioRegion after = before;
    if (! cycleTake (after, forward)) return false;
    ctx.engine().getUndoManager().beginNewTransaction();
    return ctx.engine().getUndoManager().perform (
        new RegionEditAction (ctx.session(), ctx.engine(), kTrack, 0, before, after));
}

bool audioOrder (ScenarioContext& ctx, std::int64_t live, std::int64_t next, std::int64_t last)
{
    const auto& r = regionsOf (ctx)[0];
    return r.sourceOffset == live && r.previousTakes.size() == 2
        && r.previousTakes[0].sourceOffset == next && r.previousTakes[1].sourceOffset == last;
}

// Forward brings the next take live and sends the old one to the back;
// backward undoes that step exactly; three steps come all the way round.
ScenarioResult cycleStepsThroughTheStack (ScenarioContext& ctx)
{
    auto& regs = regionsOf (ctx);
    auto region = regionAt (0, 48000, 100);
    for (const std::int64_t offset : { 200, 300 })
    {
        TakeRef take;
        take.sourceOffset = offset;
        take.lengthInSamples = 48000;
        region.previousTakes.push_back (take);
    }
    regs.push_back (region);
    auto& undo = ctx.engine().getUndoManager();
    undo.clearUndoHistory();

    ctx.expect (cycleAudio (ctx, true) && audioOrder (ctx, 200, 300, 100),
                "Alt+T did not bring the next take live and send the old one to the back");
    ctx.expect (cycleAudio (ctx, false) && audioOrder (ctx, 100, 200, 300),
                "Alt+Shift+T did not step back to the take before");
    ctx.expect (cycleAudio (ctx, false) && audioOrder (ctx, 300, 100, 200),
                "Alt+Shift+T from the first take did not wrap to the last");
    ctx.expect (cycleAudio (ctx, true) && cycleAudio (ctx, true) && cycleAudio (ctx, true)
                    && audioOrder (ctx, 300, 100, 200),
                "three steps forward did not come back round to the same take");
    ctx.expect (undo.undo() && audioOrder (ctx, 200, 300, 100), "undo did not take back one cycle step");

    regs.push_back (regionAt (96000, 48000));
    AudioRegion single = regs[1];
    ctx.expect (! cycleTake (single, true), "a region with no history reported a take to cycle to");

    // MIDI regions cycle the same way, carrying their notes and length.
    auto& track = ctx.session().track (kTrack);
    MidiRegion midi;
    midi.lengthInTicks = 480;
    for (const std::int64_t ticks : { 960, 1440 })
    {
        MidiTakeRef take;
        take.lengthInTicks = ticks;
        midi.previousTakes.push_back (take);
    }
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (std::vector<MidiRegion> { midi }));
    const MidiRegion before = track.midiRegions.current()[0];
    MidiRegion after = before;
    ctx.expect (cycleTake (after, true), "a MIDI region with history had no take to cycle to");
    undo.beginNewTransaction();
    ctx.expect (undo.perform (new MidiRegionEditAction (ctx.session(), ctx.engine(), kTrack, 0, before, after)),
                "the MIDI cycle step was refused");
    const auto& cycled = track.midiRegions.current()[0];
    ctx.expect (cycled.lengthInTicks == 960 && cycled.previousTakes.size() == 2
                    && cycled.previousTakes[0].lengthInTicks == 1440
                    && cycled.previousTakes[1].lengthInTicks == 480,
                "the MIDI take stack did not step forward");
    ctx.expect (undo.undo() && track.midiRegions.current()[0].lengthInTicks == 480,
                "undo did not restore the MIDI take");
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

const ScenarioRegistrar fullCoverRegistrar { Scenario {
    "take.full_cover_pushes_onto_stack", { "take", "record", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (fullCoverPushesOntoStack, ctx); } } };
const ScenarioRegistrar eightInputsRegistrar { Scenario {
    "record.eight_inputs_separate_takes", { "record" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (eightInputsRecordSeparately, ctx); } } };
const ScenarioRegistrar midiJsonRegistrar { Scenario {
    "record.midi_embedded_in_json", { "record", "midi", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (midiRecordingLivesInJson, ctx); } } };
const ScenarioRegistrar capRegistrar { Scenario {
    "take.stack_keeps_newest_eight", { "take", "record" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (stackKeepsNewestEight, ctx); } } };
const ScenarioRegistrar edgeRegistrar { Scenario {
    "take.edge_overdub_trims_neighbour", { "take", "record", "punch" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (edgeOverdubTrimsNeighbour, ctx); } } };
const ScenarioRegistrar punchRegistrar { Scenario {
    "take.punch_inside_keeps_covered_slice", { "take", "record", "punch" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (punchInsideKeepsCoveredSlice, ctx); } } };
const ScenarioRegistrar cycleRegistrar { Scenario {
    "take.cycle_steps_through_the_stack", { "take", "undo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (cycleStepsThroughTheStack, ctx); } } };
} // namespace
} // namespace duskstudio::scenario
