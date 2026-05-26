#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
// Drop-in replacement for juce::ComboBox that opens a Dusk-native
// in-window picker panel (EmbeddedModal-hosted vertical list) instead
// of the platform PopupMenu. Inherits the full ComboBox API so call
// sites keep their existing addItem / setSelectedId / getSelectedId /
// onChange wiring with no changes.
//
// Why: the platform PopupMenu on XWayland intermittently flashes and
// dismisses (sticky-X11 peer-routing fights the Wayland focus tracker),
// which makes dropdowns unreliable on Linux. The Dusk-native panel
// sidesteps the issue by rendering inside the main app window.
class DuskComboBox final : public juce::ComboBox
{
public:
    DuskComboBox() = default;
    explicit DuskComboBox (const juce::String& componentName) : juce::ComboBox (componentName) {}

    void showPopup() override;
};
} // namespace duskstudio
