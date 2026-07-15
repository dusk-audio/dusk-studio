#include "DimOverlay.h"

#include <algorithm>

namespace duskstudio
{
DimOverlay::DimOverlay (float alpha)
    : fillAlpha (std::clamp (alpha, 0.0f, 1.0f))
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
    // Local copy BEFORE invoking - the handler may destroy this overlay
    // (and with it the member std::function) mid-call. Same idiom as
    // EmbeddedModal's dim onClick.
    if (auto cb = onClick) cb();
}

void DimOverlay::parentSizeChanged()
{
    if (auto* p = getParentComponent())
        setBounds (p->getLocalBounds());
}
} // namespace duskstudio
