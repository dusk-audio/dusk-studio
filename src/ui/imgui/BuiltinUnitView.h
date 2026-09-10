#pragma once

#include "DuskPanelWindow.h"

#include <functional>
#include <memory>
#include <string>

namespace duskstudio::builtin { class NativeBuiltinSlot; }

namespace duskstudio::imgui
{
// The editor for every built-in unit. There is one view rather than five
// because a unit already declares the shape of each control in its ParamInfo
// table: a name, a section, a suffix, a range, and whether the row is a slider,
// a tick box or a choice. A new unit gets an editor by existing.
//
// `title` is what the panel's header shows. `onParameterTouched` receives the
// index of the last control the user moved, which is what MIDI Learn binds to.
//
// The slot outlives the view: the caller closes the panel before it unloads.
std::unique_ptr<DuskPanelView> makeBuiltinUnitView (
    builtin::NativeBuiltinSlot& slot,
    std::string title,
    std::function<void (int paramIndex)> onParameterTouched,
    bool inlineInStage);
} // namespace duskstudio::imgui
