#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Compact-mode section pill. Left-click → onClick (toggle the section's enable),
// right-click → onRightClick (open the section editor) — the same grammar as the
// expanded section headers. Shared by the master, channel, and bus strips.
struct SectionPillButton final : public juce::TextButton
{
    using juce::TextButton::TextButton;
    std::function<void (const juce::MouseEvent&)> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onRightClick)
        {
            onRightClick (e);
            return;
        }
        juce::TextButton::mouseDown (e);
    }
};
} // namespace duskstudio
