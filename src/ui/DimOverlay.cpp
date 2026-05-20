#include "DimOverlay.h"

namespace duskstudio
{
DimOverlay::DimOverlay (float alpha)
    : fillAlpha (juce::jlimit (0.0f, 1.0f, alpha))
{
    setInterceptsMouseClicks (true, false);
    setWantsKeyboardFocus (false);
    setOpaque (false);
}

void DimOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (fillAlpha));
}

void DimOverlay::mouseDown (const juce::MouseEvent&)
{
    if (onClick) onClick();
}

void DimOverlay::parentSizeChanged()
{
    if (auto* p = getParentComponent())
        setBounds (p->getLocalBounds());
}
} // namespace duskstudio
