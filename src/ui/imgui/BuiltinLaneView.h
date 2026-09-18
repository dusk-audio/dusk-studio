#pragma once

#include "DuskPanelWindow.h"

#include <functional>
#include <memory>
#include <vector>

namespace duskstudio::builtin { class NativeBuiltinSlot; }

namespace duskstudio::imgui
{
// A built-in unit's controls drawn inline in an aux lane, filling the rectangle the
// lane gives them: the unit's parameters in captioned groups of knobs, switch banks
// and toggles, sized to whatever room the lane has. Each unit on the effects list has
// its own grouping; one without falls back to a group per parameter section.
//
// The layout is resolved from the loaded unit's parameter table when the view is
// built, so the owner builds a new view when the slot loads a different unit. The
// slot outlives the view.
class BuiltinLaneView : public DuskPanelView
{
public:
    // Where one parameter's control landed in the last frame drawn, caption and
    // readout included, in screen pixels.
    struct Placement
    {
        int paramIndex = -1;
        ImVec2 tl {};
        ImVec2 br {};
    };

    virtual const std::vector<Placement>& placements() const = 0;
};

// `onParameterTouched` receives the index of the last control the user moved, which
// is what MIDI Learn binds to.
std::unique_ptr<BuiltinLaneView> makeBuiltinLaneView (
    builtin::NativeBuiltinSlot& slot,
    std::function<void (int paramIndex)> onParameterTouched);
} // namespace duskstudio::imgui
