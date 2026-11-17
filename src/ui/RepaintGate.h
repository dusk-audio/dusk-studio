#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace duskstudio
{
// The value a widget last asked to be painted with. changed() measures against
// that rather than the previous tick, so a decay that moves less than eps per
// tick still repaints once its steps add up. A NaN eps never holds a repaint
// back.
struct PaintedValue
{
    float last = std::numeric_limits<float>::quiet_NaN();
    bool painted = false;

    bool changed (float now, float eps) noexcept
    {
        if (painted)
        {
            const bool nowNaN = std::isnan (now), lastNaN = std::isnan (last);
            if (nowNaN || lastNaN)
            {
                if (nowNaN && lastNaN) return false;
            }
            else if (! (now < last || now > last) || std::abs (now - last) <= eps)
            {
                return false;
            }
        }
        last = now;
        painted = true;
        return true;
    }

    void forget() noexcept { painted = false; }
};

// A quarter of a logical pixel. Once a gate's key carries every threshold its
// paint() applies, a move smaller than this changes only the antialiased
// coverage of an edge, never whether something is drawn.
inline constexpr float kSubPixelRepaint = 0.25f;

// One piece of a meter as paint() draws it: whether it is drawn at all and, if
// so, where, in pixels. key() is far below any drawn position when nothing is
// drawn, so crossing a draw threshold always moves a PaintedValue.
struct DrawnExtent
{
    bool shown = false;
    float px = 0.0f;

    float key() const noexcept { return shown ? px : -1.0e6f; }
};

inline DrawnExtent drawnAbove (float value, float threshold, float px) noexcept
{
    DrawnExtent drawn;
    drawn.shown = value > threshold;
    drawn.px = px;
    return drawn;
}

// Whether `component` and every component above it are set visible, so a paint
// can reach it. Unlike isShowing() a minimised window still counts: restoring one
// calls no visibilityChanged, so a component that stopped refreshing while
// minimised would come back with what it showed then.
template <typename ComponentType>
bool shownInLayout (const ComponentType& component) noexcept
{
    if (! component.isVisible())
        return false;
    const auto* above = component.getParentComponent();
    if (above == nullptr)
        return component.isOnDesktop();
    for (; above->getParentComponent() != nullptr; above = above->getParentComponent())
        if (! above->isVisible())
            return false;
    return above->isVisible() && above->isOnDesktop();
}

// Repaints the gated meters and strips have asked for, process-wide, so the GUI
// scenarios can tell an idle console from a moving one.
inline std::atomic<std::uint64_t> gatedRepaintRequests { 0 };

inline void countGatedRepaint() noexcept
{
    gatedRepaintRequests.fetch_add (1, std::memory_order_relaxed);
}
} // namespace duskstudio
