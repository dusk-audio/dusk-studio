#pragma once

#include "DuskPanelWindow.h"

#include <memory>

namespace duskstudio
{
struct MasteringParams;
class MasteringChain;
} // namespace duskstudio

namespace duskstudio::imgui
{
// The mastering chain's 5-band digital EQ: the frequency-response curve with the live
// post-EQ spectrum behind it, and a row of Freq / Gain / Q knobs beneath.
//
// An inline view rather than a modal - it is one of the mastering stage's three panels,
// so it draws its own panel surface and takes no plate.
std::unique_ptr<DuskPanelView> makeMasteringEqView (MasteringParams& params,
                                                    MasteringChain* chain);
} // namespace duskstudio::imgui
