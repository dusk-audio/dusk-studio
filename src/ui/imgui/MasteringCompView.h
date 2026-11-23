#pragma once

#include "DuskPanelWindow.h"

#include <memory>

namespace duskstudio
{
struct MasteringParams;
class MultiCompStage;
} // namespace duskstudio

namespace duskstudio::imgui
{
// The mastering chain's multiband compressor, Multi-Comp 2's Multiband mode driven
// through the session: the MULTIBAND COMP header that engages it and the preset
// picker, the band display with its crossover handles and per-band gain reduction,
// the four bands' switches and dials below it, and MIX, OUTPUT and AUTO MAKEUP along
// the bottom. Every control is on screen at once.
//
// `stage` is the chain's compressor, which prints and reads the values and publishes
// the band meters. `presetChoice` is the preset last picked (0 for none), kept by the
// caller so the name outlives the panel's window. An inline view draws its own panel
// surface and takes no plate.
std::unique_ptr<DuskPanelView> makeMasteringCompView (MasteringParams& params, const MultiCompStage& stage,
                                                     int& presetChoice);
} // namespace duskstudio::imgui
