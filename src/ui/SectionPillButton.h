#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Compact-mode section pill. Left-click -> onClick (toggle the section's enable),
// right-click -> onRightClick (section context menu), double-click -> onDoubleClick
// (open the section editor) - the same grammar as the expanded section headers.
// Shared by the master, channel, and bus strips.
//
// A double-click on a TextButton fires onClick twice (one per mouseUp), so an
// enable-toggle onClick nets to no change; mouseDoubleClick then opens the
// editor. Net effect of a double-click: no enable change + editor opens.
struct SectionPillButton final : public juce::TextButton
{
    using juce::TextButton::TextButton;
    std::function<void (const juce::MouseEvent&)> onRightClick;
    std::function<void()> onDoubleClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onRightClick)
        {
            onRightClick (e);
            return;
        }
        juce::TextButton::mouseDown (e);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (onDoubleClick) onDoubleClick();
    }
};
} // namespace duskstudio
