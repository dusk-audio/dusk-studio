#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../session/MidiBindings.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace duskstudio::scenario
{
namespace
{
constexpr int kTrack = 5;
constexpr std::int64_t kSecond = 48000;
// RecordManager's click-mask crossfade at a punch edge.
constexpr std::int64_t kPunchFade = 64;

void armTrack (ScenarioContext& ctx)
{
    auto& track = ctx.session().track (kTrack);
    track.mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    track.inputSource.store (-2, std::memory_order_relaxed);
    track.recordArmed.store (true, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();
}

AudioRegion regionAt (std::int64_t start, std::int64_t length)
{
    AudioRegion r;
    r.timelineStart = start;
    r.lengthInSamples = length;
    return r;
}

std::int64_t endOf (const AudioRegion& r) { return r.timelineStart + r.lengthInSamples; }

// Play with the loop on starts inside it: from outside, the playhead snaps to
// the loop start first.
ScenarioResult playSnapsToLoopStart (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    transport.setLoopRange (kSecond, 2 * kSecond);
    transport.setLoopEnabled (true);

    for (const std::int64_t from : { std::int64_t (0), 3 * kSecond })
    {
        transport.setPlayhead (from);
        engine.play();
        ctx.expect (transport.getPlayhead() == kSecond,
                    "Play from " + std::to_string (from) + " did not snap to the loop start");
        engine.stop();
    }

    transport.setPlayhead (kSecond + kSecond / 2);
    engine.play();
    ctx.expect (transport.getPlayhead() == kSecond + kSecond / 2, "Play inside the loop moved the playhead");
    engine.stop();

    transport.setLoopEnabled (false);
    transport.setPlayhead (0);
    engine.play();
    ctx.expect (transport.getPlayhead() == 0, "Play snapped to a loop that was switched off");
    engine.stop();
    return ctx.verdict();
}

ScenarioResult clickFollowsBusAlignment (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const int factorWas = session.oversamplingFactor.load();
    const bool enabledWas = session.metronomeEnabled.load();
    const bool playingWas = session.metronomeClickWhilePlaying.load();
    const float bpmWas = session.tempoBpm.load();
    const float volumeWas = session.metronomeVolDb.load();
    ctx.cleanup ([&session, &engine, factorWas, enabledWas, playingWas, bpmWas, volumeWas]
    {
        engine.stop();
        session.oversamplingFactor.store (factorWas);
        engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
        session.metronomeEnabled.store (enabledWas);
        session.metronomeClickWhilePlaying.store (playingWas);
        session.tempoBpm.store (bpmWas);
        session.metronomeVolDb.store (volumeWas);
    });
    session.metronomeEnabled.store (true);
    session.metronomeClickWhilePlaying.store (true);
    session.tempoBpm.store (120.0f);
    session.metronomeVolDb.store (0.0f);

    for (int factor : { 1, 2, 4 })
    {
        engine.stop();
        session.oversamplingFactor.store (factor);
        engine.prepareForSelfTest (ScenarioContext::kSampleRate, ScenarioContext::kBlockSize);
        engine.applyMasterPdcTargetsNow();
        constexpr int kLeadIn = 100;
        transport.setPlayhead (kSecond / 2 - kLeadIn);
        engine.play();
        ctx.pump (1);
        // The sine click is zero at its first sample and audible at the next.
        const int expected = kLeadIn + 1 + engine.getMixLatencySamples();
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto& block = ctx.lastBlock (channel);
            const auto first = std::find_if (block.begin(), block.end(),
                                            [] (float v) { return std::abs (v) > 1.0e-5f; });
            const int onset = first == block.end() ? -1 : (int) (first - block.begin());
            ctx.expect (onset == expected,
                        std::to_string (factor) + "x click on channel " + std::to_string (channel)
                            + " starts at " + std::to_string (onset)
                            + ", expected " + std::to_string (expected));
        }
    }
    return ctx.verdict();
}

// Count-in rolls the playhead back one bar and clicks through it, metronome
// off or not; the take itself starts where Record was pressed.
ScenarioResult countInRollsBackABar (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const bool countInWas = session.countInEnabled.load (std::memory_order_relaxed);
    const bool metronomeWas = session.metronomeEnabled.load (std::memory_order_relaxed);
    ctx.cleanup ([&session, countInWas, metronomeWas]
    {
        session.countInEnabled.store (countInWas, std::memory_order_relaxed);
        session.metronomeEnabled.store (metronomeWas, std::memory_order_relaxed);
    });
    session.countInEnabled.store (true, std::memory_order_relaxed);
    session.metronomeEnabled.store (false, std::memory_order_relaxed);

    const double beats = (double) session.beatsPerBar.load (std::memory_order_relaxed);
    const double bpm = (double) session.tempoBpm.load (std::memory_order_relaxed);
    const auto bar = (std::int64_t) ((double) kSecond * 60.0 / bpm * beats);
    const std::int64_t takeStart = 4 * kSecond;

    armTrack (ctx);
    transport.setPlayhead (takeStart);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start"))
        return ctx.verdict();
    ctx.expect (transport.getPlayhead() == takeStart - bar,
                "count-in rolled back to " + std::to_string (transport.getPlayhead())
                    + ", not one bar (" + std::to_string (bar) + " samples) before the take");

    float click = 0.0f;
    for (int block = 0; transport.getPlayhead() < takeStart && block < 10000; ++block)
        click = std::max (click, ctx.pump (1));
    ctx.note ("peak during the count-in " + std::to_string (click));
    ctx.expect (click > 0.01f, "the count-in bar was silent");

    ctx.pump (40);
    engine.stop();
    const auto& regs = session.track (kTrack).regions;
    ctx.expect (regs.size() == 1 && regs[0].timelineStart == takeStart,
                "the take did not start where Record was pressed");
    return ctx.verdict();
}

// Punch: pre-roll plays from before the punch-in, the take covers exactly the
// punch range with raised-cosine crossfades against the audio it cuts into,
// and post-roll rolls past the punch-out, then stops on its own.
ScenarioResult punchPreAndPostRoll (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const bool preWas = session.preRollEnabled.load (std::memory_order_relaxed);
    const float preSecondsWas = session.preRollSeconds.load (std::memory_order_relaxed);
    const bool postWas = session.postRollEnabled.load (std::memory_order_relaxed);
    const float postSecondsWas = session.postRollSeconds.load (std::memory_order_relaxed);
    ctx.cleanup ([&session, preWas, preSecondsWas, postWas, postSecondsWas]
    {
        session.preRollEnabled.store (preWas, std::memory_order_relaxed);
        session.preRollSeconds.store (preSecondsWas, std::memory_order_relaxed);
        session.postRollEnabled.store (postWas, std::memory_order_relaxed);
        session.postRollSeconds.store (postSecondsWas, std::memory_order_relaxed);
    });
    session.preRollEnabled.store (true, std::memory_order_relaxed);
    session.preRollSeconds.store (1.0f, std::memory_order_relaxed);
    session.postRollEnabled.store (true, std::memory_order_relaxed);
    session.postRollSeconds.store (0.5f, std::memory_order_relaxed);

    const std::int64_t punchIn = 2 * kSecond, punchOut = 3 * kSecond;
    auto& regs = session.track (kTrack).regions;
    regs.push_back (regionAt (kSecond, 3 * kSecond));
    transport.setPunchRange (punchIn, punchOut);
    transport.setPunchEnabled (true);
    armTrack (ctx);

    // Pre-roll only ever rolls back: from further back than it reaches,
    // playback starts where the playhead already is.
    transport.setPlayhead (0);
    engine.record();
    ctx.expect (transport.isRecording() && transport.getPlayhead() == 0,
                "Record from before the pre-roll point moved the playhead forward");
    engine.stop();

    transport.setPlayhead (punchIn);
    engine.record();
    if (! ctx.expect (transport.isRecording(), "Record did not start"))
        return ctx.verdict();
    ctx.expect (transport.getPlayhead() == punchIn - kSecond,
                "pre-roll did not start playback one second before the punch-in");

    std::int64_t stoppedAt = -1;
    for (int block = 0; block < 10000; ++block)
    {
        ctx.pump (1);
        const auto playhead = transport.getPlayhead();
        if (engine.serviceTransportRequests().stopped)
        {
            stoppedAt = playhead;
            break;
        }
    }
    const auto postRollEnd = punchOut + kSecond / 2;
    ctx.note ("auto-stopped with the playhead at " + std::to_string (stoppedAt));
    ctx.expect (transport.isStopped(), "post-roll never stopped the transport");
    ctx.expect (stoppedAt >= postRollEnd && stoppedAt < postRollEnd + ScenarioContext::kBlockSize,
                "post-roll did not stop in the block that passed punch-out plus half a second");

    const AudioRegion* take = nullptr;
    const AudioRegion* before = nullptr;
    const AudioRegion* after = nullptr;
    for (const auto& r : regs)
    {
        if (r.timelineStart == punchIn) take = &r;
        else if (r.timelineStart == kSecond) before = &r;
        else after = &r;
    }
    if (! ctx.expect (take != nullptr && before != nullptr && after != nullptr,
                      "the punch did not leave the old region on both sides of the take ("
                          + std::to_string (regs.size()) + " regions)"))
        return ctx.verdict();

    ctx.expect (endOf (*take) == punchOut, "the take does not end at the punch-out");
    ctx.expect (take->fadeInSamples == kPunchFade && take->fadeInShape == FadeShape::RaisedCosine
                    && take->fadeOutSamples == kPunchFade && take->fadeOutShape == FadeShape::RaisedCosine,
                "the take lacks its raised-cosine punch fades");
    ctx.expect (endOf (*before) == punchIn + kPunchFade && before->fadeOutSamples == kPunchFade
                    && before->fadeOutShape == FadeShape::RaisedCosine,
                "the audio before the punch does not crossfade into it");
    ctx.expect (after->timelineStart == punchOut - kPunchFade && after->fadeInSamples == kPunchFade
                    && after->fadeInShape == FadeShape::RaisedCosine && endOf (*after) == 4 * kSecond,
                "the audio after the punch does not crossfade out of it");
    return ctx.verdict();
}

// Loop recording needs a loop of at least 128 samples; a shorter one refuses
// Record and says why.
ScenarioResult shortLoopRefused (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    std::string refusal;
    engine.setRecordBlockedSink ([&refusal] (auto message) { refusal = message.toStdString(); });
    ctx.cleanup ([&engine] { engine.setRecordBlockedSink ({}); });
    armTrack (ctx);

    transport.setLoopRange (kSecond, kSecond + 127);
    transport.setLoopEnabled (true);
    transport.setPlayhead (kSecond);
    engine.record();
    ctx.expect (! transport.isRecording(), "a 127-sample loop recorded");
    ctx.expect (refusal.find ("128") != std::string::npos,
                "the refusal did not give the 128-sample minimum: '" + refusal + "'");

    refusal.clear();
    transport.setLoopRange (kSecond, kSecond + 128);
    engine.record();
    ctx.expect (transport.isRecording() && refusal.empty(), "a 128-sample loop was refused");
    engine.stop();
    engine.setRecordBlockedSink ({});
    return ctx.verdict();
}

// A stop from the chased master leaves the playhead where the master stopped;
// the Stop control returns it to where playback began.
ScenarioResult chaseStopLeavesPlayhead (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    const auto rollThenStop = [&] (PendingTransportAction action)
    {
        transport.setPlayhead (0);
        session.pendingTransportPlayhead.store (kSecond / 10, std::memory_order_relaxed);
        session.pendingTransportAction.store ((int) PendingTransportAction::Play,
                                              std::memory_order_release);
        engine.serviceTransportRequests();
        ctx.expect (transport.isPlaying() && transport.getPlayhead() == kSecond / 10
                        && transport.getRollStart() == kSecond / 10,
                    "the queued play did not start at its published playhead");
        ctx.pump (50);
        const auto rolledTo = transport.getPlayhead();
        session.pendingTransportAction.store ((int) action, std::memory_order_release);
        const bool stopped = engine.serviceTransportRequests().stopped;
        ctx.expect (stopped && transport.isStopped(), "the queued stop did not stop the transport");
        return std::make_pair (rolledTo, transport.getPlayhead());
    };

    const auto chase = rollThenStop (PendingTransportAction::SyncStop);
    ctx.expect (chase.second == chase.first,
                "a chase stop moved the playhead from " + std::to_string (chase.first)
                    + " to " + std::to_string (chase.second));

    const auto local = rollThenStop (PendingTransportAction::Stop);
    ctx.expect (local.second == kSecond / 10,
                "the Stop control did not return to where playback began");
    return ctx.verdict();
}

std::optional<ScenarioResult> run (ScenarioResult (*body) (ScenarioContext&), ScenarioContext& ctx)
{
    return body (ctx);
}

const ScenarioRegistrar snapRegistrar { Scenario {
    "transport.play_snaps_to_loop_start", { "transport", "loop" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (playSnapsToLoopStart, ctx); } } };
const ScenarioRegistrar countInRegistrar { Scenario {
    "transport.count_in_rolls_back_a_bar", { "transport", "record", "metronome" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (countInRollsBackABar, ctx); } } };
const ScenarioRegistrar clickAlignmentRegistrar { Scenario {
    "transport.click_follows_bus_alignment", { "transport", "metronome", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (clickFollowsBusAlignment, ctx); } } };
const ScenarioRegistrar punchRegistrar { Scenario {
    "transport.punch_pre_and_post_roll", { "transport", "record", "punch" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (punchPreAndPostRoll, ctx); } } };
const ScenarioRegistrar shortLoopRegistrar { Scenario {
    "transport.short_loop_refused", { "transport", "record", "loop" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (shortLoopRefused, ctx); } } };
const ScenarioRegistrar chaseRegistrar { Scenario {
    "transport.chase_stop_leaves_playhead", { "transport", "sync" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (chaseStopLeavesPlayhead, ctx); } } };
} // namespace
} // namespace duskstudio::scenario
