#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Patreon credits live in the plugins repo (plugins/shared/PatreonBackers.h,
// already on the include path when the donor DSP is discovered) so the DAW
// and every plugin show the same list from one source of truth. Builds
// without the plugins repo simply have no supporters panel.
#if __has_include("PatreonBackers.h")
 #include "PatreonBackers.h"
 #define DUSKSTUDIO_HAS_PATREON_CREDITS 1
#else
 #define DUSKSTUDIO_HAS_PATREON_CREDITS 0
#endif

#if DUSKSTUDIO_HAS_PATREON_CREDITS

namespace duskstudio
{
// Dusk-styled mirror of the plugins' SupportersOverlay: tier headings with
// accent colours over a centred name list. Hosted in an EmbeddedModal (the
// in-window modal convention) with a scrolling viewport so the panel keeps
// working as the list grows.
class SupportersPanel final : public juce::Component
{
public:
    std::function<void()> onCloseRequested;

    SupportersPanel()
    {
        setOpaque (true);
        viewport.setViewedComponent (&tierList, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        closeBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff305a82));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible (closeBtn);

        // Natural height up to a cap; past that the viewport scrolls.
        setSize (440, juce::jmin (620, kHeaderH + tierList.naturalHeight() + kFooterH));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);

        auto r = getLocalBounds().reduced (18);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        g.drawText ("Special Thanks", r.removeFromTop (26), juce::Justification::centred, false);
        g.setColour (juce::Colour (0xff909094));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("To the supporters who make Dusk Studio possible",
                     r.removeFromTop (18), juce::Justification::centred, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18);
        r.removeFromTop (kHeaderH - 18);
        auto footer = r.removeFromBottom (kFooterH - 18);
        closeBtn.setBounds (footer.removeFromBottom (28)
                                  .withSizeKeepingCentre (100, 28));
        viewport.setBounds (r);
        tierList.setSize (r.getWidth() - viewport.getScrollBarThickness(),
                           tierList.naturalHeight());
    }

private:
    static constexpr int kHeaderH = 70;
    static constexpr int kFooterH = 54;

    struct TierList final : public juce::Component
    {
        struct Tier { const char* heading; const std::vector<juce::String>* names;
                      juce::Colour accent; bool past; };

        std::vector<Tier> tiers() const
        {
            return {
                { "CHAMPIONS",       &PatreonCredits::champions,      juce::Colour (0xffffd700), false },
                { "PATRONS",         &PatreonCredits::patrons,        juce::Colour (0xff00aaff), false },
                { "SUPPORTERS",      &PatreonCredits::supporters,     juce::Colour (0xff6ac47e), false },
                { "HUGS",            &PatreonCredits::hugs,           juce::Colour (0xffc08ad0), false },
                { "PAST SUPPORTERS", &PatreonCredits::pastSupporters, juce::Colour (0xff606060), true  },
            };
        }

        int naturalHeight() const
        {
            int h = 6;
            for (const auto& t : tiers())
                if (! t.names->empty())
                    h += 28 + (int) t.names->size() * 18 + 14;
            return juce::jmax (h, 60);
        }

        void paint (juce::Graphics& g) override
        {
            const int cx = getWidth() / 2;
            int y = 6;

            bool any = false;
            for (const auto& t : tiers())
            {
                if (t.names->empty()) continue;
                any = true;

                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.setColour (t.accent);
                g.drawText (t.heading, 0, y, getWidth(), 16, juce::Justification::centred, false);
                g.setColour (t.accent.withAlpha (0.3f));
                g.fillRect (cx - 15, y + 20, 30, 1);
                y += 28;

                g.setFont (juce::Font (juce::FontOptions (t.past ? 12.0f : 13.0f)));
                g.setColour (t.past ? juce::Colour (0xff707078) : juce::Colour (0xffd0d0d4));
                for (const auto& name : *t.names)
                {
                    g.drawText (name, 0, y, getWidth(), 18, juce::Justification::centred, true);
                    y += 18;
                }
                y += 14;
            }

            if (! any)
            {
                g.setFont (juce::Font (juce::FontOptions (13.0f)));
                g.setColour (juce::Colour (0xff909094));
                g.drawText ("Be the first to support development on Patreon!",
                             getLocalBounds(), juce::Justification::centred, false);
            }
        }
    };

    // tierList BEFORE viewport: the viewport holds a non-owning pointer to it
    // (setViewedComponent(&tierList, false)), so tierList must outlive the
    // viewport - members destruct in reverse declaration order.
    TierList        tierList;
    juce::Viewport  viewport;
    juce::TextButton closeBtn { "Close" };
};
} // namespace duskstudio

#endif // DUSKSTUDIO_HAS_PATREON_CREDITS
