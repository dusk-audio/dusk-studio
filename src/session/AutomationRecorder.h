#pragma once

#include "Session.h"

#include <cstdint>
#include <vector>

namespace duskstudio
{
// Records one control's WRITE or TOUCH pass. The strip calls record() on each
// timer tick while the pass runs (and on a MUTE or SOLO click), and finish()
// once it stops: the transport stopped, the mode changed, or TOUCH let go.
// The pass collects its points on the side; finish() splices them over exactly
// the span the pass covered and publishes the lane in one step, so what the
// lane held outside that span stays. A WRITE pass picks the earlier ride up
// again straight after it; a TOUCH pass glides back to it.
//
// While a pass is open the lane's passOpen flag is up and the audio thread
// plays the control's own value rather than the lane, so nothing plays the
// old points over the span between a TOUCH release and the splice landing.
// Message thread only.
class AutomationPassRecorder
{
public:
    explicit AutomationPassRecorder (AutomationParam recorded) noexcept : param (recorded) {}

    // How long a TOUCH release takes to glide back to the earlier ride.
    static constexpr double kTouchReturnSeconds = 0.1;

    bool active() const noexcept { return ! pass.empty(); }

    // Adds value (in the control's own units: dB, pan, 0/1) at playhead. A
    // playhead behind the pass, as after a loop wrap, closes the pass first
    // and starts a new one.
    void record (AutomationLane& lane, std::int64_t playhead, float value, float bpm);

    // Splices the pass into the lane and publishes it. returnSamples is how
    // long the lane takes to get back to what it held after the pass: 0 for
    // WRITE, the TOUCH glide otherwise. No-op when no pass is open.
    void finish (AutomationLane& lane, std::int64_t returnSamples);

private:
    AutomationParam param;
    std::vector<AutomationPoint> pass;
    std::int64_t spanEnd = 0;
    float lastValue = 0.0f;
    float lastBpm = 120.0f;
};
} // namespace duskstudio
