#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>

namespace duskstudio::embedscale
{
constexpr double factorFromSources (double globalScale,
                                    double peerPlatformScale,
                                    double nativeViewScale,
                                    bool useNativeViewScale) noexcept
{
    return globalScale * (useNativeViewScale ? nativeViewScale
                                             : peerPlatformScale);
}

struct PhysicalBounds
{
    int x, y, width, height;
};

inline PhysicalBounds toPhysicalBounds (int logicalX, int logicalY,
                                        int logicalWidth, int logicalHeight,
                                        double scale) noexcept
{
    // Round the corners, not the dimensions - independently rounded x and
    // width can land the right/bottom edge 1px off the painted edge.
    const auto round = [] (double v) { return static_cast<int> (std::floor (v + 0.5)); };
    const int x0 = round (logicalX * scale);
    const int y0 = round (logicalY * scale);
    const int x1 = round ((logicalX + logicalWidth) * scale);
    const int y1 = round ((logicalY + logicalHeight) * scale);
    return { x0, y0, x1 - x0, y1 - y0 };
}

// The app-set UI zoom (appconfig ui_scale -> Desktop::setGlobalScaleFactor).
inline double globalScale()
{
    return juce::Desktop::getInstance().getGlobalScaleFactor();
}

// JUCE component coordinates are logical; raw X11 windows live in physical
// pixels. The full logical->physical factor is the peer's platform scale TIMES
// the Desktop global scale - the UI zoom is NOT part of
// getPlatformScaleFactor(), and missing it drifts every native editor window
// down-right by zoom% of its position. Plugin-reported sizes (resizeView /
// ui:resize / clap gui) are physical.
inline double factor (const juce::Component& c)
{
    if (auto* peer = c.getPeer())
        return globalScale() * peer->getPlatformScaleFactor();
    return globalScale();
}

inline juce::Rectangle<int> toPhysical (const juce::Component& c,
                                        juce::Rectangle<int> logical)
{
    const auto bounds = toPhysicalBounds (logical.getX(), logical.getY(),
                                          logical.getWidth(), logical.getHeight(),
                                          factor (c));
    return { bounds.x, bounds.y, bounds.width, bounds.height };
}

inline int fromPhysical (const juce::Component& c, int physical)
{
    return std::max (1, static_cast<int> (std::floor (physical / factor (c) + 0.5)));
}
} // namespace duskstudio::embedscale
