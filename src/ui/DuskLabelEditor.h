#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
// A double-click-editable juce::Label spawns an internal TextEditor when the
// edit begins, and that editor keeps the default built-in right-click menu -
// a native PopupMenu window, i.e. the XWayland popup flash every explicit
// TextEditor in the app already disables (see DuskComboBox). Call once after
// setEditable so the spawned editor gets the same treatment.
inline void disableLabelEditorPopup (juce::Label& label)
{
    label.onEditorShow = [&label]
    {
        if (auto* ed = label.getCurrentTextEditor())
            ed->setPopupMenuEnabled (false);
    };
}
} // namespace duskstudio
