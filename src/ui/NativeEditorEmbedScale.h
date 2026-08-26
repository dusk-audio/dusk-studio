#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PlatformWindowing.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

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

// Where an embedded native child sits, and the scale the framework should draw it
// at. macOS reports the backing scale off the view rather than the peer, which is
// why the factor is assembled from three sources rather than read from factor().
struct NativeChildGeometry
{
    int x = 0;
    int y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double scale = 1.0;
};

// The logical rectangle a child of this size takes when centred in the window,
// shrunk rather than allowed to run off the edges. Used twice per panel: once for
// the child's own geometry, once for the dim overlay's click-through region.
inline juce::Rectangle<int> centredChildBounds (const juce::Component& topLevel,
                                                int logicalWidth, int logicalHeight)
{
    const auto bounds = topLevel.getLocalBounds();
    return bounds.withSizeKeepingCentre (
        std::clamp (logicalWidth, 0, std::max (0, bounds.getWidth() - 16)),
        std::clamp (logicalHeight, 0, std::max (0, bounds.getHeight() - 16)));
}

inline NativeChildGeometry childGeometryFor (const juce::Component& topLevel,
                                             juce::Rectangle<int> logical)
{
    auto* const peer = topLevel.getPeer();
    const double peerScale = peer != nullptr ? peer->getPlatformScaleFactor() : 1.0;
    const double backingScale = peer != nullptr
                              ? platform::nativeViewBackingScale (peer->getNativeHandle())
                              : 1.0;
   #if JUCE_MAC
    constexpr bool useNativeViewScale = true;
   #else
    constexpr bool useNativeViewScale = false;
   #endif
    const double scale = factorFromSources (globalScale(), peerScale, backingScale,
                                            useNativeViewScale);
    const auto bounds = toPhysicalBounds (logical.getX(), logical.getY(),
                                          logical.getWidth(), logical.getHeight(), scale);
    return { bounds.x, bounds.y,
             static_cast<std::uint32_t> (std::max (2, bounds.width)),
             static_cast<std::uint32_t> (std::max (2, bounds.height)),
             scale };
}

// A child pinned to the physical size it had when it opened, so an app-wide UI-scale
// change rescales the DAW behind it and leaves the panel alone. The audio settings
// panel is the control surface for that change: a panel that rescaled with everything
// else would move the slider out from under the pointer mid-drag.
//
// The logical rectangle grows as the app zooms out and shrinks as it zooms in, so the
// physical one it converts to stays put; the draw scale is taken back to the reference
// for the same reason.
struct PinnedChild
{
    juce::Rectangle<int> logical;     // for the dim overlay's click-through region
    NativeChildGeometry geometry;
};

inline PinnedChild pinnedChildGeometry (const juce::Component& topLevel,
                                        int designWidth, int designHeight,
                                        double referenceGlobalScale)
{
    const double live = std::max (0.01, globalScale());
    const double inverseZoom = std::max (0.01, referenceGlobalScale) / live;
    const auto bounds = centredChildBounds (
        topLevel, static_cast<int> (std::floor (designWidth * inverseZoom + 0.5)),
        static_cast<int> (std::floor (designHeight * inverseZoom + 0.5)));
    auto geometry = childGeometryFor (topLevel, bounds);
    geometry.scale *= inverseZoom;
    return { bounds, geometry };
}

// The handle a native child embeds into, or 0 when the window is not realised yet.
inline std::uintptr_t nativeParentHandle (const juce::Component& topLevel)
{
    auto* const peer = topLevel.getPeer();
    if (peer == nullptr || peer->getNativeHandle() == nullptr)
        return 0;
    return reinterpret_cast<std::uintptr_t> (peer->getNativeHandle());
}
} // namespace duskstudio::embedscale
