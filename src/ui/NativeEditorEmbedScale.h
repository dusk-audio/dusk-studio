#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio::embedscale
{
// JUCE component coordinates are logical; raw X11 windows live in physical
// pixels. The full logical->physical factor is the peer's platform scale TIMES
// the Desktop global scale - the app-set UI zoom (appconfig ui_scale ->
// Desktop::setGlobalScaleFactor) is NOT part of getPlatformScaleFactor(), and
// missing it drifts every native editor window down-right by zoom% of its
// position. Plugin-reported sizes (resizeView / ui:resize / clap gui) are
// physical.

inline double factor (const juce::Component& c)
{
    const double global = juce::Desktop::getInstance().getGlobalScaleFactor();
    if (auto* peer = c.getPeer())
        return global * peer->getPlatformScaleFactor();
    return global;
}

inline juce::Rectangle<int> toPhysical (const juce::Component& c,
                                        juce::Rectangle<int> logical)
{
    // Round the corners, not the dimensions - independently rounded x and
    // width can land the right/bottom edge 1px off the painted edge.
    const double s = factor (c);
    const int x0 = juce::roundToInt (logical.getX()      * s);
    const int y0 = juce::roundToInt (logical.getY()      * s);
    const int x1 = juce::roundToInt (logical.getRight()  * s);
    const int y1 = juce::roundToInt (logical.getBottom() * s);
    return { x0, y0, x1 - x0, y1 - y0 };
}

inline int fromPhysical (const juce::Component& c, int physical)
{
    return juce::jmax (1, juce::roundToInt (physical / factor (c)));
}
} // namespace duskstudio::embedscale
