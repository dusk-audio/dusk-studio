#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Shared compressor-section header button. Pill with a green LED on the
// left and white text. Left-click toggles enable (toggleFn); optional
// right-click invokes pickFn (section context menu). Pass an empty pickFn
// to disable the right-click action. Optional onDoubleClick opens the
// section's full editor.
class CompHeaderButton final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    CompHeaderButton (std::function<bool()> getEnabled,
                       std::function<void()> onToggleEnable,
                       std::function<void()> onPickMode = {},
                       std::function<void()> onDoubleClickEditor = {})
        : isEnabledFn (std::move (getEnabled)),
          toggleFn    (std::move (onToggleEnable)),
          pickFn      (std::move (onPickMode)),
          doubleClickFn (std::move (onDoubleClickEditor))
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip (pickFn
                       ? "Left-click to enable / disable. Right-click to choose mode."
                       : "Left-click to enable / disable.");
    }

    void setLabelText (juce::String t)
    {
        if (text != t) { text = std::move (t); repaint(); }
    }

    // Optional accent stripe painted along the bottom edge of the
    // pill. Used by the EQ header to surface the active type (Brown
    // vs Black) without taking real estate from the centred label.
    // Pass a transparent colour (default) to disable.
    void setAccentColour (juce::Colour c)
    {
        if (accent != c) { accent = c; repaint(); }
    }

    // Optional short chip text painted on the right side of the pill
    // (e.g. "E" / "G" for EQ type). Empty string disables it.
    void setChipText (juce::String t)
    {
        if (chip != t) { chip = std::move (t); repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = isEnabledFn ? isEnabledFn() : false;

        // Mostly transparent pill - only a faint outline + LED + label
        // are drawn. Lets the underlying section band tint show through.
        g.setColour (juce::Colour (0xff242428).withAlpha (0.18f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (juce::Colour (0xff3a3a40).withAlpha (0.55f));
        g.drawRoundedRectangle (r, 4.0f, 0.8f);

        const float ledSize = juce::jmin (r.getHeight() - 4.0f, 8.0f);
        const float ledX    = r.getX() + 4.0f;
        const float ledY    = r.getCentreY() - ledSize / 2.0f;
        const auto ledRect  = juce::Rectangle<float> (ledX, ledY, ledSize, ledSize);
        g.setColour (juce::Colour (0xff0a0a0c));
        g.fillEllipse (ledRect.expanded (1.0f));
        g.setColour (on ? juce::Colour (0xff60d060) : juce::Colour (0xff2a3028));
        g.fillEllipse (ledRect);
        if (on)
        {
            g.setColour (juce::Colour (0xffa0f0a0).withAlpha (0.55f));
            g.fillEllipse (ledRect.reduced (ledSize * 0.35f, ledSize * 0.35f)
                                    .translated (-ledSize * 0.10f, -ledSize * 0.10f));
        }

        // Right-side chip (e.g. "E" / "G" for EQ type). Painted first
        // so its bounds reservation is reflected when carving the text
        // area below. Background tints from the accent so the chip
        // doubles as the colour cue.
        const float chipReserve = chip.isNotEmpty() ? 18.0f : 0.0f;
        if (chip.isNotEmpty())
        {
            const auto chipR = juce::Rectangle<float> (
                r.getRight() - chipReserve - 2.0f,
                r.getY() + 3.0f,
                chipReserve, r.getHeight() - 6.0f);
            const auto chipFill = accent.isTransparent()
                                       ? juce::Colour (0xff2a2a32)
                                       : accent;
            g.setColour (chipFill);
            g.fillRoundedRectangle (chipR, 2.5f);
            g.setColour (juce::Colour (0xff0a0a0c));
            g.drawRoundedRectangle (chipR, 2.5f, 0.6f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
            g.drawText (chip, chipR, juce::Justification::centred, false);
        }

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        auto textBounds = r;
        textBounds.removeFromLeft (ledSize + 8.0f);
        textBounds.removeFromRight (4.0f + chipReserve);
        g.drawText (text, textBounds, juce::Justification::centred, false);

        // Accent stripe along the bottom edge (only when a non-
        // transparent accent was set AND no chip is drawn - when a
        // chip is present, the chip itself carries the colour).
        if (chip.isEmpty() && ! accent.isTransparent())
        {
            const auto stripe = juce::Rectangle<float> (
                r.getX() + 4.0f, r.getBottom() - 3.0f,
                r.getWidth() - 8.0f, 2.0f);
            g.setColour (accent);
            g.fillRoundedRectangle (stripe, 1.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (pickFn) pickFn();
            return;
        }
        // Double-click grammar: the first click (numClicks == 1) toggles
        // immediately; the second click arrives as a fresh mouseDown with
        // numClicks == 2 which we skip here, then mouseDoubleClick toggles
        // BACK and opens the editor. Net effect of a double-click: no enable
        // change + editor opens. No timers, so single clicks stay lag-free.
        if (e.getNumberOfClicks() >= 2)
            return;
        if (toggleFn) toggleFn();
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (toggleFn) toggleFn();   // undo the first click's toggle -> no net change
        repaint();
        if (doubleClickFn) doubleClickFn();
    }

private:
    std::function<bool()> isEnabledFn;
    std::function<void()> toggleFn;
    std::function<void()> pickFn;
    std::function<void()> doubleClickFn;
    juce::String text { "COMP" };
    juce::String chip;                                 // empty = no chip
    juce::Colour accent { juce::Colours::transparentBlack };
};
} // namespace duskstudio
