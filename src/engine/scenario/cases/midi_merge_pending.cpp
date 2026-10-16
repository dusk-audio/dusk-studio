#include "../Scenario.h"
#include "MidiProbeHarness.h"
#include "../../Transport.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
#if DUSKSTUDIO_HAS_NATIVE_CLAP
ScenarioResult runPending (ScenarioContext& ctx, bool latency)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& track = session.track (0);
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::skip ("the MIDI input bank has no injectable input");
    const auto fixture = ctx.fixture ("panic_probe.clap");
    if (! fixture) return ScenarioResult::skip ("panic_probe.clap did not resolve");

    ctx.cleanup ([&engine, &session, tempoMap = session.tempoMap]
    {
        session.tempoMap = tempoMap;
        engine.publishTempoMap();
        session.recomputeRtCounters();
    });
    ctx.keep (session.tempoBpm);
    ctx.keep (session.midiMergeEnabled);
    ctx.keep (session.countInEnabled);
    ctx.keep (session.lastRecordPointSamples);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.keep (track.inputMonitor);
    ctx.keep (track.midiInputIndex);
    ctx.keep (track.midiChannel);
    for (int t = 0; t < Session::kNumTracks; ++t) ctx.keep (session.track (t).recordArmed);
    ctx.cleanup ([&engine, &track, regions = track.midiRegions.current(),
                  stage = engine.getStage(), offset = engine.getRecordingLatencyOffsetSamples(),
                  position = transport.getPlayhead(), loop = transport.isLoopEnabled(),
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  punch = transport.isPunchEnabled(), punchIn = transport.getPunchIn(), punchOut = transport.getPunchOut()]
    {
        engine.stop();
        engine.getUndoManager().clearUndoHistory();
        engine.getChannelStrip (0).unloadNativeClap();
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
        engine.setRecordingLatencyOffsetSamples (offset);
        engine.setStage (stage);
        auto& restored = engine.getTransport();
        restored.setPlayhead (position);
        restored.setLoopRange (loopStart, loopEnd);
        restored.setLoopEnabled (loop);
        restored.setPunchRange (punchIn, punchOut);
        restored.setPunchEnabled (punch);
    });

    engine.stop();
    engine.setStage (AudioEngine::Stage::Recording);
    engine.setRecordingLatencyOffsetSamples (0);
    session.tempoBpm.store (120.0f, std::memory_order_release);
    session.tempoMap.clear();
    engine.publishTempoMap();
    session.midiMergeEnabled.store (true);
    session.countInEnabled.store (false);
    for (int t = 0; t < Session::kNumTracks; ++t) session.setTrackArmed (t, false);
    track.frozen.store (false);
    track.mode.store ((int) Track::Mode::Midi);
    track.inputMonitor.store (false);
    track.midiInputIndex.store (input);
    track.midiChannel.store (0);
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
    session.setTrackArmed (0, true);
    session.recomputeRtCounters();
    std::string error;
    auto& slot = engine.getChannelStrip (0).getNativeClapSlot();
    const bool loaded = slot.load (*fixture, ScenarioContext::kSampleRate, ScenarioContext::kBlockSize,
                                   error, latency ? "studio.dusk.test.panic-probe-latency-400" : midiprobe::kPanicProbeClapId);
    if (! ctx.expect (loaded, "the MIDI timing probe could not load: " + error))
        return ctx.verdict();
    ctx.expect (slot.getLatencySamples() == (latency ? 400 : 0), "the probe reported the wrong latency");
    ctx.pump (2);
    transport.setPlayhead (0);
    transport.setPunchEnabled (false);
    transport.setLoopRange (0, latency ? 512 : 384);
    transport.setLoopEnabled (true);
    engine.getUndoManager().clearUndoHistory();
    const auto generation = track.midiRegions.generation();
    const auto capture = track.midiRegions.read()->capture;
    const auto ons = midiprobe::counter (ctx, 0, "noteOnsSeen");
    const auto offs = midiprobe::counter (ctx, 0, "noteOffsSeen");
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start")) return ctx.verdict();

    dusk::MidiBuffer notes;
    midiprobe::addMessage (notes, 0x90, 60, 100, 0);
    midiprobe::addMessage (notes, 0x80, 60, 0, 100);
    ctx.pumpWithMidi (input, std::move (notes));
    if (! latency) ctx.pump (1);
    ctx.expect ((int) midiprobe::counter (ctx, 0, "noteOnsSeen") == (int) ons + 2,
                "the next pass did not replay the captured loop-start note exactly once");
    ctx.expect ((int) midiprobe::counter (ctx, 0, "noteOffsSeen") == (int) offs + 2,
                "the replayed note did not receive exactly one release");
    ctx.expect ((int) midiprobe::counter (ctx, 0, "lastNoteOnOffset") == (latency ? 112 : 128),
                "the replay onset missed its next-pass callback position");
    ctx.expect ((int) midiprobe::counter (ctx, 0, "lastNoteOffOffset") == (latency ? 212 : 228),
                "the replay release missed its next-pass callback position");
    ctx.expect ((int) midiprobe::counter (ctx, 0, "voicesHeld") == 0, "the short replay left a voice sounding");
    ctx.expect (transport.getCommittedPlayhead() == (latency ? 256 : 128), "the loop advanced to the wrong position");
    ctx.expect (track.midiRegions.current().empty() && track.midiRegions.generation() == generation
                && track.midiRegions.read()->capture.gestureId == capture.gestureId
                && track.midiRegions.read()->capture.consumedThroughPass == capture.consumedThroughPass
                && ! engine.getUndoManager().canUndo(), "the callback check unexpectedly serviced a merge commit");
    return ctx.verdict();
}
#else
ScenarioResult runPending (ScenarioContext&, bool)
{
    return ScenarioResult::skip ("native CLAP hosting is not enabled");
}
#endif

const ScenarioRegistrar intraBlock { Scenario {
    "midi.merge_pending_intrablock", { "midi", "recording", "merge", "loop", "clap" }, Needs::Engine,
    { "panic_probe.clap" }, [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runPending (ctx, false); }
} };
const ScenarioRegistrar lookahead { Scenario {
    "midi.merge_pending_latency", { "midi", "recording", "merge", "loop", "clap", "latency" }, Needs::Engine,
    { "panic_probe.clap" }, [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return runPending (ctx, true); }
} };
} // namespace
} // namespace duskstudio::scenario
