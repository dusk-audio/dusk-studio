#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../../session/AutomationRecorder.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio::scenario
{
namespace
{
constexpr auto kFader = AutomationParam::FaderDb;

// An earlier ride: -30 dB at 0 rising to 0 dB at 96000.
void seedRide (AutomationLane& lane)
{
    lane.publishPoints ({ { 0, normalizeAutomationValue (kFader, -30.0f), 120.0f },
                          { 96000, normalizeAutomationValue (kFader, 0.0f), 120.0f } });
}

bool nearDb (float a, float b) { return std::abs (a - b) < 0.05f; }

// A pass holding `db` from `from` to `to`, one point every UI timer tick.
void ride (AutomationPassRecorder& recorder, AutomationLane& lane,
           std::int64_t from, std::int64_t to, float db)
{
    for (std::int64_t t = from; t <= to; t += 1600)
        recorder.record (lane, t, db, 120.0f);
}

// WRITE replaces what it played over and nothing else: before and after the
// span the earlier ride plays on.
ScenarioResult writeSplicesItsSpan (ScenarioContext& ctx)
{
    AutomationLane lane;
    seedRide (lane);
    const auto earlier = lane.pointsConst();
    AutomationPassRecorder recorder (kFader);

    ride (recorder, lane, 24000, 48000, -10.0f);
    ctx.expect (lane.passOpen.load(), "a recording pass did not raise passOpen");
    ctx.expect (lane.pointsConst() == earlier, "the lane changed before the pass finished");
    recorder.finish (lane, 0);
    ctx.expect (! lane.passOpen.load(), "the finished pass left passOpen up");

    const auto& now = lane.pointsConst();
    for (const std::int64_t t : { std::int64_t (12000), std::int64_t (72000), std::int64_t (96000) })
        ctx.expect (nearDb (evaluateLane (now, t, kFader), evaluateLane (earlier, t, kFader)),
                    "outside the pass the ride changed at " + std::to_string (t));
    ctx.expect (nearDb (evaluateLane (now, 36000, kFader), -10.0f), "the pass did not take its span");
    ctx.expect (nearDb (evaluateLane (now, 48000, kFader), -10.0f), "the pass does not hold to its end");
    return ctx.verdict();
}

// TOUCH glides from the released value back to the earlier ride.
ScenarioResult touchGlidesBack (ScenarioContext& ctx)
{
    constexpr std::int64_t kGlide = 4800;
    AutomationLane lane;
    seedRide (lane);
    const auto earlier = lane.pointsConst();
    AutomationPassRecorder recorder (kFader);

    ride (recorder, lane, 24000, 48000, -10.0f);
    recorder.finish (lane, kGlide);
    const auto& now = lane.pointsConst();

    const float midway = evaluateLane (now, 48000 + kGlide / 2, kFader);
    const float target = evaluateLane (earlier, 48000 + kGlide, kFader);
    ctx.note ("halfway through the glide " + std::to_string (midway) + " dB, heading for "
              + std::to_string (target) + " dB");
    ctx.expect (std::min (-10.0f, target) < midway && midway < std::max (-10.0f, target),
                "the release did not glide");
    ctx.expect (nearDb (evaluateLane (now, 48000 + kGlide, kFader), target),
                "the glide did not land on the earlier ride");
    ctx.expect (nearDb (evaluateLane (now, 80000, kFader), evaluateLane (earlier, 80000, kFader)),
                "after the glide the earlier ride did not play on");
    return ctx.verdict();
}

// A loop wrap closes the pass it interrupts and opens the next, so each lap
// overwrites the same stretch and the lane stays in time order.
ScenarioResult loopWrapClosesThePass (ScenarioContext& ctx)
{
    AutomationLane lane;
    seedRide (lane);
    AutomationPassRecorder recorder (kFader);

    ride (recorder, lane, 24000, 48000, -10.0f);
    ride (recorder, lane, 24000, 48000, -20.0f);
    recorder.finish (lane, 0);

    const auto& now = lane.pointsConst();
    bool ascending = true;
    for (std::size_t i = 1; i < now.size(); ++i)
        ascending = ascending && now[i].timeSamples > now[i - 1].timeSamples;
    ctx.expect (ascending, "the lane is out of time order after a loop wrap");
    ctx.expect (nearDb (evaluateLane (now, 36000, kFader), -20.0f), "the second lap did not win");
    ctx.expect (nearDb (evaluateLane (now, 96000, kFader), 0.0f), "the laps erased the ride after them");
    return ctx.verdict();
}

// A discrete lane keeps only the changes, and holds the last state to the
// end of the pass.
ScenarioResult discreteKeepsChanges (ScenarioContext& ctx)
{
    AutomationLane lane;
    AutomationPassRecorder recorder (AutomationParam::Mute);
    const float states[] = { 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f };
    std::int64_t t = 0;
    for (const float state : states)
    {
        recorder.record (lane, t, state, 120.0f);
        t += 1600;
    }
    recorder.finish (lane, 0);
    const auto& now = lane.pointsConst();
    ctx.note ("mute points: " + std::to_string (now.size()));
    ctx.expect (now.size() == 4, "expected on-off-on-off changes plus the pass end");
    ctx.expect (evaluateLane (now, 4000, AutomationParam::Mute) > 0.5f
                    && evaluateLane (now, 9000, AutomationParam::Mute) < 0.5f,
                "the mute changes are not where they were made");
    return ctx.verdict();
}

// A lane something else replaced during the pass, as a session load does,
// keeps what it was given; the pass is dropped.
ScenarioResult replacedLaneDropsThePass (ScenarioContext& ctx)
{
    AutomationLane lane;
    seedRide (lane);
    AutomationPassRecorder recorder (kFader);
    ride (recorder, lane, 24000, 48000, -10.0f);
    lane.publishPoints ({ { 0, normalizeAutomationValue (kFader, -6.0f), 120.0f } });
    const auto loaded = lane.pointsConst();

    recorder.finish (lane, 0);
    ctx.expect (lane.pointsConst() == loaded, "the pass landed in a lane that was replaced under it");
    ctx.expect (! lane.passOpen.load(), "the dropped pass left passOpen up");
    return ctx.verdict();
}

// While a pass is open the engine plays the control, not the lane: a TOUCH
// release does not play the old points before the splice lands.
ScenarioResult enginePlaysControlWhilePassOpen (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& track = session.track (0);
    auto& lane = track.automationLanes[(std::size_t) kFader];
    ctx.cleanup ([&track, &lane]
    {
        track.automationMode.store ((int) AutomationMode::Off, std::memory_order_release);
        lane.passOpen.store (false, std::memory_order_release);
        lane.publishPoints ({});
        track.strip.faderDb.store (0.0f, std::memory_order_relaxed);
    });
    lane.publishPoints ({ { 0, normalizeAutomationValue (kFader, -20.0f), 120.0f } });
    track.strip.faderDb.store (-3.0f, std::memory_order_relaxed);
    track.automationMode.store ((int) AutomationMode::Touch, std::memory_order_release);

    AutomationPassRecorder recorder (kFader);
    recorder.record (lane, 0, -3.0f, 120.0f);
    ctx.pump (1);
    ctx.expect (nearDb (track.strip.liveFaderDb.load(), -3.0f),
                "with a pass open the engine played the lane");

    recorder.finish (lane, 0);
    ctx.pump (1);
    ctx.expect (nearDb (track.strip.liveFaderDb.load(), -3.0f),
                "after the splice the engine did not play the recorded value");
    return ctx.verdict();
}

std::optional<ScenarioResult> run (ScenarioResult (*body) (ScenarioContext&), ScenarioContext& ctx)
{
    return body (ctx);
}

const ScenarioRegistrar writeRegistrar { Scenario {
    "automation.write_splices_its_span", { "automation" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (writeSplicesItsSpan, ctx); } } };
const ScenarioRegistrar touchRegistrar { Scenario {
    "automation.touch_glides_back", { "automation" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (touchGlidesBack, ctx); } } };
const ScenarioRegistrar wrapRegistrar { Scenario {
    "automation.loop_wrap_closes_the_pass", { "automation", "loop" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (loopWrapClosesThePass, ctx); } } };
const ScenarioRegistrar discreteRegistrar { Scenario {
    "automation.discrete_keeps_changes", { "automation" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (discreteKeepsChanges, ctx); } } };
const ScenarioRegistrar replacedRegistrar { Scenario {
    "automation.replaced_lane_drops_the_pass", { "automation", "session" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (replacedLaneDropsThePass, ctx); } } };
const ScenarioRegistrar gateRegistrar { Scenario {
    "automation.engine_plays_control_while_pass_open", { "automation" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return run (enginePlaysControlWhilePassOpen, ctx); } } };
} // namespace
} // namespace duskstudio::scenario
