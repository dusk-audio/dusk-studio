#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace duskstudio
{
class AudioEngine;
class AuxLaneComponent;

// AUX stage view. A row of four selector buttons at the top (AUX 1..4)
// drives which AuxLaneComponent fills the body. Only one lane is visible
// at a time, so plugin editors get the full window width and most of its
// height to render at native size - the previous 2×2 grid squeezed
// editors into quarters and forced popouts for any plugin larger than a
// reverb.
//
// The selected lane persists for the life of the AuxView (set on
// construction, retained across hide/show cycles when the user toggles
// between stages).
class AuxView final : public juce::Component
{
public:
    AuxView (Session& session, AudioEngine& engine);
    ~AuxView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    int  getActiveLane() const noexcept { return activeLaneIndex; }
    void setActiveLane (int index);

    // Walk every aux lane and tear down its open plugin-editor popout
    // windows through the X-focus-safe path. Called from
    // MainComponent::beginSafeShutdown phase 4 alongside
    // ConsoleView::dropAllPluginEditors so EVERY top-level window the
    // host owns is hit BEFORE the unmap of the main window in phase 6.
    void closeAllAuxPopouts();

    // Re-push the slot screen-rect for every active lane's editor host -
    // called from MainComponent's main-window movement watcher so the
    // hosts follow when the user drags or resizes the main window.
    void repositionAllHosts();

    // Hide / show every active lane's editor host. Called when AUX view
    // visibility flips (user switches main view to MIXING / RECORDING /
    // MASTERING and back).
    void setAllHostsHidden (bool hidden);

    void visibilityChanged() override;

private:
    int activeLaneIndex = 0;

    std::array<juce::TextButton, Session::kNumAuxLanes>            selectorButtons;
    std::array<std::unique_ptr<AuxLaneComponent>, Session::kNumAuxLanes> lanes;
};
} // namespace duskstudio
