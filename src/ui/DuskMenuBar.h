#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "DuskContextMenu.h"
#include <memory>
#include <vector>

namespace duskstudio
{
// Dusk-native menu bar - drop-in replacement for juce::MenuBarComponent
// that routes top-level menu pops through showContextMenu (EmbeddedModal
// + DuskContextMenuPanel) instead of juce::PopupMenu's native popup.
//
// Visually paints like a standard app menu bar (flat text per name,
// hover-highlight on the hovered name, no buttoned pills). Public API
// matches juce::MenuBarComponent for the parts MainComponent uses:
// setModel + addAndMakeVisible + setBounds.
class DuskMenuBar final : public juce::Component
{
public:
    DuskMenuBar() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    void setModel (juce::MenuBarModel* m)
    {
        model = m;
        rebuildNames();
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        for (int i = 0; i < (int) hits.size(); ++i)
        {
            const auto r = hits[(size_t) i];
            if (i == hoveredIdx || i == activeIdx)
            {
                g.setColour (juce::Colour (0xff2a2a32));
                g.fillRect (r);
            }
            g.setColour ((i == hoveredIdx || i == activeIdx)
                           ? juce::Colours::white
                           : juce::Colour (0xfff0f0f4));
            g.drawText (names[i], r.reduced (kHPad, 0),
                         juce::Justification::centredLeft, false);
        }
    }

    void resized() override { rebuildHits(); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int newHover = hitTest (e.getPosition());
        if (newHover != hoveredIdx) { hoveredIdx = newHover; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredIdx != -1) { hoveredIdx = -1; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int idx = hitTest (e.getPosition());
        if (idx >= 0) showMenuForIndex (idx);
    }

private:
    static constexpr int kHPad = 10;

    void rebuildNames()
    {
        names.clear();
        if (model != nullptr)
            for (const auto& n : model->getMenuBarNames())
                names.add (n);
        rebuildHits();
        repaint();
    }

    void rebuildHits()
    {
        hits.clear();
        const juce::Font f { juce::FontOptions (16.0f, juce::Font::bold) };
        int x = 0;
        for (const auto& n : names)
        {
            const int w = (int) std::ceil (f.getStringWidthFloat (n)) + kHPad * 2;
            hits.push_back (juce::Rectangle<int> (x, 0, w, getHeight()));
            x += w;
        }
    }

    int hitTest (juce::Point<int> p) const
    {
        for (int i = 0; i < (int) hits.size(); ++i)
            if (hits[(size_t) i].contains (p)) return i;
        return -1;
    }

    void showMenuForIndex (int idx)
    {
        if (model == nullptr || idx < 0 || idx >= (int) hits.size()) return;
        const juce::PopupMenu menu = model->getMenuForIndex (idx, names[idx]);

        activeIdx = idx;
        repaint();

        // Anchor below the clicked label with a comfortable gap so the
        // popup body doesn't visually butt up against the menu name.
        const auto hitR  = hits[(size_t) idx];
        const auto local = juce::Point<int> (hitR.getX(), hitR.getBottom() + 8);
        const auto anchor = localPointToGlobal (local);

        juce::Component::SafePointer<DuskMenuBar> safeSelf (this);
        auto* m = model;
        showContextMenu (menu, *this, anchor,
                          [safeSelf, m, idx] (int chosenId)
                          {
                              if (auto* self = safeSelf.getComponent())
                              {
                                  self->activeIdx = -1;
                                  self->repaint();
                              }
                              if (chosenId <= 0) return;
                              if (m != nullptr)
                                  m->menuItemSelected (chosenId, idx);
                          });
    }

    juce::MenuBarModel* model = nullptr;
    juce::StringArray names;
    std::vector<juce::Rectangle<int>> hits;
    int hoveredIdx = -1;
    int activeIdx  = -1;
};
} // namespace duskstudio
