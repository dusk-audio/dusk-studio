#pragma once

#include "DuskPanelWindow.h"

#include <memory>

namespace duskstudio
{
struct MasteringParams;
class BrickwallLimiter;
} // namespace duskstudio

namespace duskstudio::imgui
{
// The mastering chain's brickwall limiter: the Threshold and Ceiling meter columns with
// their draggable level lines, the gain-reduction column, the loudness readout, and the
// mode / release / lookahead / stereo-link controls down the right.
//
// An inline view rather than a modal - it is one of the mastering stage's three panels,
// so it draws its own panel surface and takes no plate.
std::unique_ptr<DuskPanelView> makeMasteringLimiterView (MasteringParams& params,
                                                         BrickwallLimiter& limiter);
} // namespace duskstudio::imgui
