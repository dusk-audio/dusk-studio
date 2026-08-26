#pragma once

#include "DuskPanelWindow.h"

namespace duskstudio
{
struct Track;

namespace imgui
{
// The channel compressor editor: ON, the three-way mode selector, the VCA knee and
// detector toggles, the input and gain-reduction meters with the threshold handle
// between them, and whichever of RATIO / ATTACK / RELEASE / MAKEUP the active mode
// has. Every value lives in the track's session atomics, so the view holds no state
// beyond the meter ballistics.
std::unique_ptr<DuskPanelView> makeChannelCompView (Track& track);
} // namespace imgui
} // namespace duskstudio
