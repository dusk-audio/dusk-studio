#pragma once

#include "DuskPanelWindow.h"

#include <functional>
#include <memory>

namespace duskstudio
{
struct MasteringPhaseParams;
class PhaseAnalyzer;
}

namespace duskstudio::imgui
{
std::unique_ptr<DuskPanelView> makeMasteringPhaseView (
    MasteringPhaseParams& params, PhaseAnalyzer& analyzer,
    std::function<bool()> getMono, std::function<void (bool)> setMono);
}
