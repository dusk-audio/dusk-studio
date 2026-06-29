#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"
#include "EmbeddedModal.h"

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
class AuxView final : public juce::Component,
                       private juce::Timer
{
public:
    AuxView (Session& session, AudioEngine& engine);
    ~AuxView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDoubleClick (const juce::MouseEvent&) override;

    int  getActiveLane() const noexcept { return activeLaneIndex; }
    void setActiveLane (int index);

    // Shutdown: close every lane's native CLAP editor (X11 Display + host window)
    // while the main peer + message loop are still alive. See
    // MainComponent::beginSafeShutdown phase 4.
    void dropAllClapEditors();

private:
    void timerCallback() override;
    void promptRenameLane (int index);

    int activeLaneIndex = 0;
    Session* sessionPtr = nullptr;

    std::array<juce::TextButton, Session::kNumAuxLanes>            selectorButtons;
    std::array<std::unique_ptr<AuxLaneComponent>, Session::kNumAuxLanes> lanes;
    EmbeddedModal renameModal;
};
} // namespace duskstudio
