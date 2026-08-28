#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Processing-editor dim: lighter than the 0.55 decision-modal default so the
// strip level / GR meters and neighbouring strips stay readable while you tweak
// a comp / EQ / plugin editor and audition on a loop. Decision modals
// (save-before-quit, missing-plugins) keep the heavier default.
inline constexpr float kEditorDimAlpha = 0.28f;

// Translucent black overlay used to "dim" the rest of the UI behind a modal
// surface (CallOutBox popups in TIMELINE mode, the TapeMachine gear modal,
// etc.). Sits as a sibling of the modal in the top-level component, sized
// to the parent's local bounds.
//
// onClick fires when the user clicks anywhere on the overlay - owners use
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

    // Region covered by an embedded native child window, in this overlay's
    // coordinates. JUCE selects XInput2 on the top-level while a raw X11
    // child (DAF/pugl, plugin editors) takes core events, so a press over the
    // child is delivered to both: the child acts on it AND the overlay sees an
    // XI2 press it must not read as a click-outside dismissal.
    void setNativeChildArea (juce::Rectangle<int> area);

    std::function<void()> onClick;

private:
    float fillAlpha;
    juce::Rectangle<int> nativeChildArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DimOverlay)
};
} // namespace duskstudio
