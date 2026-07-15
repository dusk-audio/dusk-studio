#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <functional>

namespace duskstudio
{
// Small round LED used as the bypass-toggle for every compressor in the
// app (channel strip, bus, master). Green when engaged, dim grey when
// bypassed. Click to toggle. Lives in the strip header alongside the
// "COMP" section label so the comp section reads with the same
// grammar as the EQ section ([LED] LABEL ... [pill]).
//
// State is supplied by callbacks rather than a static atom so the same
// class can drive ChannelStripParams::compEnabled, BusParams::compEnabled,
// and MasterBusParams::compEnabled without templating.
class CompBypassLed final : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    CompBypassLed (std::function<bool()> getEnabled,
                    std::function<void()> onToggle)
        : isEnabledFn (std::move (getEnabled)),
          toggleFn (std::move (onToggle))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Compressor bypass - green when engaged, click to toggle");
    }

    void paint (juce::Graphics& g) override
    {
        // Match CompHeaderButton's LED grammar: tight 1 px dark ring,
        // LED face occupies most of the bounds. Previous double-reduce
        // (1 px outer + 2 px inner) shrank the visible green to ~50 %
        // of the bounds and made the EQ bypass LED read much smaller
        // than the COMP header button's LED at the same widget size.
        //
        // The visual caps at 10 px and centres in the bounds, so callers
        // can give the widget a larger hit area than the dot - an 8 px
        // click target is unusable.
        auto r = getLocalBounds().toFloat();
        const float d = std::min ({r.getWidth(), r.getHeight(), 10.0f});
        r = r.withSizeKeepingCentre (d, d);
        const bool on = isEnabledFn ? isEnabledFn() : false;

        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillEllipse (r);

        const auto inner = r.reduced (1.0f);
        g.setColour (on ? juce::Colour (0xff60d060) : juce::Colour (0xff2a3028));
        g.fillEllipse (inner);
        if (on)
        {
            g.setColour (juce::Colour (0xffa0f0a0).withAlpha (0.55f));
            g.fillEllipse (inner.reduced (inner.getWidth() * 0.35f,
                                            inner.getHeight() * 0.35f)
                                  .translated (-inner.getWidth() * 0.10f,
                                                 -inner.getHeight() * 0.10f));
        }
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (toggleFn) toggleFn();
        repaint();
    }

private:
    std::function<bool()> isEnabledFn;
    std::function<void()> toggleFn;
};
} // namespace duskstudio
