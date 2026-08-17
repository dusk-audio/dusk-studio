#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <functional>
#include <memory>

namespace duskstudio
{
using SplitModuleTextButtonBase = juce::TextButton;

class SplitModuleButton final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    using StateCallback = std::function<bool()>;
    using ActionCallback = std::function<void()>;

    explicit SplitModuleButton (juce::String moduleLabel = {})
        : indicatorButton (*this, true, false),
          editorButton (*this, false, true),
          label (std::move (moduleLabel))
    {
        addAndMakeVisible (indicatorButton);
        addAndMakeVisible (editorButton);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        Component::setTitle (label + " module");
        setAccessibleLabels();

        indicatorButton.onClick = [this]
        {
            const auto callback = toggleBypassFn;
            if (callback) callback();
        };
        editorButton.onClick = [this]
        {
            const auto callback = openEditorFn;
            if (callback) callback();
        };
    }

    void setCallbacks (StateCallback getBypassed,
                       ActionCallback toggleBypass,
                       ActionCallback openEditor,
                       ActionCallback openContextMenu)
    {
        isBypassedFn = std::move (getBypassed);
        toggleBypassFn = std::move (toggleBypass);
        openEditorFn = std::move (openEditor);
        openContextMenuFn = std::move (openContextMenu);
        refresh();
    }

    void setLabelText (juce::String newLabel)
    {
        if (label == newLabel) return;
        label = std::move (newLabel);
        if (! hasCustomAccessibilityTitle)
            Component::setTitle (label + " module");
        setAccessibleLabels();
        repaint();
    }

    const juce::String& getLabelText() const noexcept { return label; }

    void setAccessibilityTitle (const juce::String& newTitle)
    {
        hasCustomAccessibilityTitle = true;
        Component::setTitle (newTitle);
    }

    // Pull externally-mutated model state into the accessibility-facing
    // toggle before scheduling the visual update. Owners call this from
    // their existing UI refresh timers.
    void refresh()
    {
        syncIndicatorState();
        repaint();
    }

    void setAccentColour (juce::Colour newAccent)
    {
        if (accent == newAccent) return;
        accent = newAccent;
        repaint();
    }

    void setTooltip (const juce::String& newTooltip) override
    {
        juce::SettableTooltipClient::setTooltip (newTooltip);
        indicatorButton.setTooltip (newTooltip);
        editorButton.setTooltip (newTooltip);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.75f);
        if (bounds.isEmpty()) return;

        const bool bypassed = isBypassed();
        const auto indicatorArea = indicatorButton.getBounds().toFloat();
        const auto editorArea = editorButton.getBounds().toFloat();

        g.setColour (juce::Colour (0xff202024).withAlpha (0.92f));
        g.fillRoundedRectangle (bounds, 4.0f);

        paintHitboxState (g, bounds, indicatorArea, indicatorButton);
        paintHitboxState (g, bounds, editorArea, editorButton);

        g.setColour (juce::Colour (0xff4a4a50).withAlpha (0.55f));
        g.drawVerticalLine (indicatorButton.getRight(),
                            bounds.getY() + 3.0f,
                            bounds.getBottom() - 3.0f);

        const float indicatorSize = std::clamp (
            std::min (indicatorArea.getWidth() - 8.0f,
                      indicatorArea.getHeight() - 6.0f),
            5.0f, 9.0f);
        const auto indicator = juce::Rectangle<float> (
            indicatorSize, indicatorSize).withCentre (indicatorArea.getCentre());

        g.setColour (juce::Colour (0xff09090b));
        g.fillEllipse (indicator.expanded (1.0f));
        if (bypassed)
        {
            g.setColour (juce::Colour (0xff29292e));
            g.fillEllipse (indicator);
            g.setColour (juce::Colour (0xff66666e));
            g.drawEllipse (indicator.reduced (0.5f), 1.0f);
        }
        else
        {
            g.setColour (accent);
            g.fillEllipse (indicator);
            g.setColour (accent.brighter (0.8f).withAlpha (0.55f));
            g.fillEllipse (indicator.reduced (indicatorSize * 0.32f)
                                    .translated (-indicatorSize * 0.10f,
                                                 -indicatorSize * 0.10f));
        }

        g.setColour (bypassed ? juce::Colour (0xff77777f)
                              : juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (label, editorArea.reduced (3.0f, 0.0f),
                    juce::Justification::centred, false);

        g.setColour (juce::Colour (0xff55555c));
        g.drawRoundedRectangle (bounds, 4.0f, 0.9f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const int indicatorWidth = std::max (
            1, juce::roundToInt (static_cast<float> (area.getWidth()) * 0.20f));
        indicatorButton.setBounds (area.removeFromLeft (indicatorWidth));
        editorButton.setBounds (area);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (
            *this, juce::AccessibilityRole::group);
    }

private:
    class HitboxButton final : public SplitModuleTextButtonBase
    {
    public:
        HitboxButton (SplitModuleButton& ownerIn, bool isToggle, bool suppressRepeatedClicksIn)
            : owner (ownerIn), suppressRepeatedClicks (suppressRepeatedClicksIn)
        {
            setClickingTogglesState (isToggle);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            onStateChange = [this] { owner.repaint(); };
        }

        void paintButton (juce::Graphics&, bool, bool) override {}

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu())
            {
                owner.openContextMenu();
                return;
            }
            if (suppressRepeatedClicks && e.getNumberOfClicks() != 1)
                return;
            SplitModuleTextButtonBase::mouseDown (e);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()
                || (suppressRepeatedClicks && e.getNumberOfClicks() != 1))
            {
                setState (isMouseOver() ? buttonOver : buttonNormal);
                return;
            }
            SplitModuleTextButtonBase::mouseUp (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()
                || (suppressRepeatedClicks && e.getNumberOfClicks() != 1))
                return;
            SplitModuleTextButtonBase::mouseDrag (e);
        }

    private:
        SplitModuleButton& owner;
        bool suppressRepeatedClicks = false;
    };

    bool isBypassed() const
    {
        return isBypassedFn ? isBypassedFn() : true;
    }

    void syncIndicatorState()
    {
        const bool engaged = ! isBypassed();
        if (indicatorButton.getToggleState() != engaged)
            indicatorButton.setToggleState (engaged, juce::dontSendNotification);
    }

    void openContextMenu()
    {
        const auto callback = openContextMenuFn;
        if (callback) callback();
    }

    void setAccessibleLabels()
    {
        indicatorButton.setButtonText (label + " enabled");
        indicatorButton.setTitle (label + " enabled");
        indicatorButton.setHelpText ("Toggle " + label + " bypass");
        editorButton.setButtonText ("Open " + label + " editor");
        editorButton.setTitle ("Open " + label + " editor");
        editorButton.setHelpText ("Open the " + label + " editor");
    }

    static void paintHitboxState (juce::Graphics& g,
                                  juce::Rectangle<float> shell,
                                  juce::Rectangle<float> area,
                                  const HitboxButton& button)
    {
        float alpha = 0.0f;
        if (button.isDown()) alpha = 0.18f;
        else if (button.isOver()) alpha = 0.09f;
        if (alpha <= 0.0f) return;

        g.saveState();
        g.reduceClipRegion (area.getSmallestIntegerContainer());
        g.setColour (juce::Colours::white.withAlpha (alpha));
        g.fillRoundedRectangle (shell, 4.0f);
        g.restoreState();
    }

    HitboxButton indicatorButton;
    HitboxButton editorButton;
    StateCallback isBypassedFn;
    ActionCallback toggleBypassFn;
    ActionCallback openEditorFn;
    ActionCallback openContextMenuFn;
    juce::String label;
    juce::Colour accent { 0xff60d060 };
    bool hasCustomAccessibilityTitle = false;
};
} // namespace duskstudio
