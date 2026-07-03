#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio::embedscale
{
// JUCE component coordinates are logical; raw X11 windows live in physical
// pixels (physical = logical × peer scale, see LinuxComponentPeer::setBounds).
// The native editor components must convert on both sides of the boundary or
// the embedded plugin window drifts from its JUCE body by the scale factor.
// Plugin-reported sizes (resizeView / ui:resize / clap gui) are physical.

inline double factor (const juce::Component& c)
{
    if (auto* peer = c.getPeer())
        return peer->getPlatformScaleFactor();
    return 1.0;
}

inline juce::Rectangle<int> toPhysical (const juce::Component& c,
                                        juce::Rectangle<int> logical)
{
    const double s = factor (c);
    return { juce::roundToInt (logical.getX()      * s),
             juce::roundToInt (logical.getY()      * s),
             juce::roundToInt (logical.getWidth()  * s),
             juce::roundToInt (logical.getHeight() * s) };
}

inline int fromPhysical (const juce::Component& c, int physical)
{
    return juce::jmax (1, juce::roundToInt (physical / factor (c)));
}
} // namespace duskstudio::embedscale
