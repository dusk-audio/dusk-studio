#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Translucent black overlay used to "dim" the rest of the UI behind a modal
// surface (CallOutBox popups in TIMELINE mode, the TapeMachine gear modal,
// etc.). Sits as a sibling of the modal in the top-level component, sized
// to the parent's local bounds.
//
// onClick fires when the user clicks anywhere on the overlay — owners use
// this to dismiss whatever modal is being shadowed.
class DimOverlay final : public juce::Component
{
public:
    // alpha is the fill darkness, 0..1. Default 0.55 matches the
    // CallOutBox / startup / tuner usage; modal editors (audio region,
    // piano roll) pass a heavier value (~0.80) so the DAW behind reads
    // as background rather than co-equal context.
    explicit DimOverlay (float alpha = 0.55f);

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void parentSizeChanged() override;

    std::function<void()> onClick;

private:
    float fillAlpha;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DimOverlay)
};
} // namespace duskstudio
