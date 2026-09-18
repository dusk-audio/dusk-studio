#pragma once

#include "DuskPanelWindow.h"

#include <functional>
#include <memory>
#include <string>

namespace duskstudio::builtin { class NativeBuiltinSlot; }

namespace duskstudio::imgui
{
// The modal editor for every built-in unit on a channel insert. There is one view
// rather than five because a unit already declares the shape of each control in its
// ParamInfo table: a name, a section, a suffix, a range, and whether the row is a
// slider, a tick box or a choice. A new unit gets an editor by existing. On an aux
// lane the unit's controls are drawn inline by BuiltinLaneView instead.
//
// `title` is what the panel's header shows. `onParameterTouched` receives the
// index of the last control the user moved, which is what MIDI Learn binds to.
//
// The slot outlives the view: the caller closes the panel before it unloads.
std::unique_ptr<DuskPanelView> makeBuiltinUnitView (
    builtin::NativeBuiltinSlot& slot,
    std::string title,
    std::function<void (int paramIndex)> onParameterTouched);
} // namespace duskstudio::imgui
