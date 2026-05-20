#include "AuxView.h"
#include "AuxLaneComponent.h"
#include "../engine/AudioEngine.h"

namespace duskstudio
{
namespace
{
void styleSelectorButton (juce::TextButton& b, juce::Colour onColour)
{
    b.setClickingTogglesState (true);
    b.setRadioGroupId (2001);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    b.setColour (juce::TextButton::buttonOnColourId, onColour);
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff909094));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
}
} // namespace

AuxView::AuxView (Session& session, AudioEngine& engine)
{
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        // Selector button. Use full-saturation accent (matches stage-tab
        // saturation; the earlier 0.7f multiplier made these read as
        // washed-out next to the stage tabs they sit underneath).
        auto& btn = selectorButtons[(size_t) i];
        btn.setButtonText (session.auxLane (i).name);
        styleSelectorButton (btn, session.auxLane (i).colour);
        if (i == 0)
            btn.setConnectedEdges (juce::Button::ConnectedOnRight);
        else if (i == Session::kNumAuxLanes - 1)
            btn.setConnectedEdges (juce::Button::ConnectedOnLeft);
        else
            btn.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        btn.onClick = [this, i] { setActiveLane (i); };
        addAndMakeVisible (btn);

        // Lane content. All four are constructed up front so plugin
        // editors and timer callbacks survive across selector switches;
        // only the active one is visible.
        lanes[(size_t) i] = std::make_unique<AuxLaneComponent> (
            session.auxLane (i), engine.getAuxLaneStrip (i), i, engine);
        addChildComponent (lanes[(size_t) i].get());
    }

    setActiveLane (0);
}

AuxView::~AuxView() = default;

void AuxView::closeAllAuxPopouts()
{
    for (auto& lane : lanes)
        if (lane != nullptr)
            lane->closeAllPopoutsForShutdown();
}

void AuxView::setActiveLane (int index)
{
    index = juce::jlimit (0, Session::kNumAuxLanes - 1, index);
    activeLaneIndex = index;

    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        selectorButtons[(size_t) i].setToggleState (i == index, juce::dontSendNotification);
        if (lanes[(size_t) i] != nullptr)
        {
            const bool active = (i == index);
            lanes[(size_t) i]->setVisible (active);
            // Editor hosts are X11 toplevels separate from the JUCE
            // visibility tree - hide them explicitly when their lane
            // isn't active, else they keep floating over the screen.
            lanes[(size_t) i]->setEditorHostsHidden (! active || ! isVisible());
        }
    }
    resized();
    repositionAllHosts();
}

void AuxView::repositionAllHosts()
{
    if (activeLaneIndex < 0 || activeLaneIndex >= Session::kNumAuxLanes) return;
    if (auto* lane = lanes[(size_t) activeLaneIndex].get())
        lane->repositionEditorHosts();
}

void AuxView::setAllHostsHidden (bool hidden)
{
    for (auto& lane : lanes)
        if (lane != nullptr)
            lane->setEditorHostsHidden (hidden);
}

void AuxView::visibilityChanged()
{
    // Whole AUX view swapped out (user picked MIXING / RECORDING /
    // MASTERING) or back in. Hide / re-show only the active lane's host
    // since inactive lanes' hosts are already hidden.
    const bool nowVisible = isVisible();
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
        if (lanes[(size_t) i] != nullptr)
            lanes[(size_t) i]->setEditorHostsHidden (! nowVisible || i != activeLaneIndex);
    if (nowVisible)
        repositionAllHosts();
}

void AuxView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff121214));
}

void AuxView::resized()
{
    auto area = getLocalBounds().reduced (4);

    // Selector row across the top - same height as the main stage buttons
    // for visual consistency.
    constexpr int kSelectorH = 28;
    auto row = area.removeFromTop (kSelectorH);
    const int btnW = juce::jmax (1, row.getWidth() / Session::kNumAuxLanes);
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        // Last button absorbs any rounding remainder so the row exactly
        // fills the available width.
        const int w = (i == Session::kNumAuxLanes - 1) ? row.getWidth() : btnW;
        selectorButtons[(size_t) i].setBounds (row.removeFromLeft (w));
    }
    area.removeFromTop (6);

    // Active lane fills the rest.
    if (activeLaneIndex >= 0 && activeLaneIndex < Session::kNumAuxLanes
        && lanes[(size_t) activeLaneIndex] != nullptr)
    {
        lanes[(size_t) activeLaneIndex]->setBounds (area);
    }
}
} // namespace duskstudio
