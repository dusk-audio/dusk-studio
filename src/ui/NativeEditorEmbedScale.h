#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio::embedscale
{
// JUCE component coordinates are logical; raw X11 windows live in physical
// pixels. The full logical→physical factor is the peer's platform scale TIMES
// the Desktop global scale — the app-set UI zoom (appconfig ui_scale →
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
