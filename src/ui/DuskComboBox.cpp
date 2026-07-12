#include "DuskComboBox.h"
#include "EmbeddedModal.h"

namespace duskstudio
{
namespace
{
// One static modal app-wide for combo dropdowns. Picker / chooser
// / alert each have their own static modal so a combo opened over
// any of those stacks cleanly.
EmbeddedModal& sharedComboModal()
{
    static EmbeddedModal m;
    return m;
}

// Vertical list panel. One row per non-separator menu item; section
// headers render as bold labels; separators as thin dividers.
// Selection fires onPick(itemId) and closes the modal.
class DuskComboPanel final : public juce::Component
{
public:
    struct Row
    {
        juce::String text;
        int          itemId    = 0;       // 0 = non-clickable header / separator
        bool         isHeader  = false;
        bool         isSep     = false;
        bool         isTicked  = false;
        bool         isEnabled = true;
    };

    DuskComboPanel (juce::String headerTitle,
                     std::vector<Row> rows,
                     std::function<void (int)> onPick)
        : title (std::move (headerTitle)),
          rowsData (std::move (rows)),
          onPickFn (std::move (onPick))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        // Compute width from the widest item text so short-item combos
        // (freq pickers showing "30 Hz" / "8 kHz") don't render a
        // ridiculous 260-px-wide list. Add fixed padding for the tick
        // dot column (24 px text X-offset) + trailing space (18 px) +
        // a min/max clamp so very short or pathologically long items
        // still render readably.
        constexpr int kRowH      = 26;
        constexpr int kSepH      = 8;
        constexpr int kBottomPad = 8;
        constexpr int kTextXPad  = 24;  // tick column on the left of each row
        constexpr int kTrailPad  = 18;
        constexpr int kMinW      = 100;
        constexpr int kMaxW      = 320;

        const juce::Font rowFont   { juce::FontOptions (14.0f) };
        const juce::Font headerFont{ juce::FontOptions (14.5f, juce::Font::bold) };
        int maxTextW = 0;
        for (const auto& r : rowsData)
        {
            if (r.isSep) continue;
            const auto& f = r.isHeader ? headerFont : rowFont;
            maxTextW = juce::jmax (maxTextW,
                (int) std::ceil (f.getStringWidthFloat (r.text)));
        }
        if (title.isNotEmpty())
            maxTextW = juce::jmax (maxTextW,
                (int) std::ceil (headerFont.getStringWidthFloat (title)));

        const int w = juce::jlimit (kMinW, kMaxW,
                                       maxTextW + kTextXPad + kTrailPad);

        const int headerH = title.isEmpty() ? 0 : 28;
        int total = headerH + kBottomPad;
        for (const auto& r : rowsData)
            total += r.isSep ? kSepH : kRowH;
        setSize (w, juce::jmax (60, total));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);

        auto area = getLocalBounds().reduced (4);
        if (title.isNotEmpty())
        {
            auto header = area.removeFromTop (24);
            g.setColour (juce::Colour (0xffe8e8ec));
            g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
            g.drawText (title, header.reduced (8, 0),
                         juce::Justification::centredLeft, false);
            g.setColour (juce::Colour (0xff2a2a30));
            g.drawHorizontalLine (header.getBottom() - 1,
                                    (float) header.getX() + 4.0f,
                                    (float) header.getRight() - 4.0f);
        }

        // Body rows
        constexpr int kRowH = 26;
        constexpr int kSepH = 8;
        int y = area.getY();
        for (int i = 0; i < (int) rowsData.size(); ++i)
        {
            const auto& r = rowsData[(size_t) i];
            if (r.isSep)
            {
                g.setColour (juce::Colour (0xff2a2a30));
                g.drawHorizontalLine (y + kSepH / 2,
                                        (float) area.getX() + 4.0f,
                                        (float) area.getRight() - 4.0f);
                y += kSepH;
                continue;
            }

            const auto rowR = juce::Rectangle<int> (area.getX(), y, area.getWidth(), kRowH);
            if (r.isHeader)
            {
                g.setColour (juce::Colour (0xff8090a0));
                g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
                g.drawText (r.text.toUpperCase(), rowR.reduced (10, 0),
                             juce::Justification::centredLeft, false);
            }
            else
            {
                const bool hovered = (i == hoveredRow);
                if (hovered)
                {
                    g.setColour (juce::Colour (0xff2a3a50));
                    g.fillRect (rowR.reduced (2, 0));
                }
                g.setColour (r.isEnabled ? juce::Colours::white
                                          : juce::Colour (0xff707080));
                g.setFont (juce::Font (juce::FontOptions (14.0f)));
                const int textX = 24;
                g.drawText (r.text,
                             rowR.withX (rowR.getX() + textX)
                                  .withWidth (rowR.getWidth() - textX - 8),
                             juce::Justification::centredLeft, false);
                if (r.isTicked)
                {
                    g.setColour (juce::Colour (0xff60d060));
                    g.fillEllipse ((float) rowR.getX() + 8.0f,
                                    (float) rowR.getCentreY() - 3.5f, 7.0f, 7.0f);
                }
            }
            y += kRowH;
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int newHover = rowAtY (e.y);
        if (newHover != hoveredRow)
        {
            hoveredRow = newHover;
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoveredRow != -1) { hoveredRow = -1; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int row = rowAtY (e.y);
        if (row < 0 || row >= (int) rowsData.size()) return;
        const auto& r = rowsData[(size_t) row];
        if (r.isSep || r.isHeader || ! r.isEnabled || r.itemId == 0) return;
        if (onPickFn) onPickFn (r.itemId);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            sharedComboModal().close();
            return true;
        }
        // Up/Down hover navigation; Enter selects.
        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            const int dir = (k == juce::KeyPress::upKey) ? -1 : 1;
            int next = hoveredRow;
            for (int step = 0; step < (int) rowsData.size(); ++step)
            {
                next = (next < 0)
                        ? (dir > 0 ? 0 : (int) rowsData.size() - 1)
                        : juce::jlimit (0, (int) rowsData.size() - 1, next + dir);
                const auto& r = rowsData[(size_t) next];
                if (! r.isSep && ! r.isHeader && r.isEnabled && r.itemId != 0)
                {
                    hoveredRow = next;
                    repaint();
                    return true;
                }
            }
            return true;
        }
        if (k == juce::KeyPress::returnKey
            && hoveredRow >= 0 && hoveredRow < (int) rowsData.size())
        {
            const auto& r = rowsData[(size_t) hoveredRow];
            if (! r.isSep && ! r.isHeader && r.isEnabled && r.itemId != 0
                && onPickFn)
            {
                onPickFn (r.itemId);
                return true;
            }
        }
        return false;
    }

private:
    int rowAtY (int y) const
    {
        constexpr int kRowH = 26;
        constexpr int kSepH = 8;
        const int top = (title.isEmpty() ? 4 : 4 + 24);
        int yy = top;
        for (int i = 0; i < (int) rowsData.size(); ++i)
        {
            const int h = rowsData[(size_t) i].isSep ? kSepH : kRowH;
            if (y >= yy && y < yy + h) return i;
            yy += h;
        }
        return -1;
    }

    juce::String title;
    std::vector<Row> rowsData;
    std::function<void (int)> onPickFn;
    int hoveredRow = -1;
};
} // namespace

void DuskComboBox::showPopup()
{
    // Walk the ComboBox's PopupMenu and translate items into Dusk rows.
    auto* root = getRootMenu();
    if (root == nullptr)
    {
        // No backing menu - fall through to JUCE's default popup so we
        // don't silently drop the click.
        juce::ComboBox::showPopup();
        return;
    }

    std::vector<DuskComboPanel::Row> rows;
    rows.reserve ((size_t) getNumItems() + 4);
    juce::PopupMenu::MenuItemIterator it (*root, /*recursive*/ false);
    while (it.next())
    {
        const auto& item = it.getItem();
        DuskComboPanel::Row r;
        r.isSep    = item.isSeparator;
        r.isHeader = item.isSectionHeader;
        r.text     = item.text;
        r.itemId   = item.itemID;
        r.isEnabled = item.isEnabled;
        r.isTicked = (item.itemID == getSelectedId());
        rows.push_back (std::move (r));
    }

    auto* parent = getTopLevelComponent();
    if (parent == nullptr) parent = this;

    juce::Component::SafePointer<DuskComboBox> safeSelf (this);
    auto panel = std::make_unique<DuskComboPanel> (
        getName().isNotEmpty() ? getName() : juce::String(),
        std::move (rows),
        [safeSelf] (int chosenId)
        {
            // Close FIRST so the modal disappears before the change
            // notification fires (callers' onChange handlers may open
            // another modal, e.g. plugin pickers).
            sharedComboModal().close();
            if (auto* s = safeSelf.getComponent())
            {
                // Reset juce::ComboBox's internal `menuActive` flag so
                // the next click reopens the popup. Without this the
                // base class still thinks our (overridden) popup is up
                // and showPopupIfNotActive short-circuits -> combo
                // appears frozen after one selection.
                s->hidePopup();
                // Apply selection silently, then fire onChange by hand.
                // sendNotificationSync ends up calling juce::ComboBox's
                // internal post-action path (accessibility handler focus
                // grab + dynamic_cast into the JUCE peer) which has been
                // observed to abort with __dynamic_cast on Linux/XWayland
                // when called from inside our modal teardown. Manually
                // firing onChange keeps the user's handler running while
                // skipping the dangerous post-action path.
                s->setSelectedId (chosenId, juce::dontSendNotification);
                if (s->onChange) s->onChange();
            }
        });

    // Anchor the popup BELOW the combo's screen bounds (or above when it
    // would clip the bottom edge), instead of centring in the parent
    // window. Without this the popup floats in the middle of the screen
    // far from the click - confusing and easy to miss.
    const int panelW = panel->getWidth();
    const int panelH = panel->getHeight();
    const auto comboScreen = getScreenBounds();
    const auto parentScreen = parent->getScreenBounds();

    int sx = comboScreen.getX();
    int sy = comboScreen.getBottom() + 2;
    if (sy + panelH > parentScreen.getBottom() - 8)
        sy = juce::jmax (parentScreen.getY() + 8, comboScreen.getY() - panelH - 2);
    if (sx + panelW > parentScreen.getRight() - 8)
        sx = juce::jmax (parentScreen.getX() + 8, parentScreen.getRight() - panelW - 8);
    sx = juce::jmax (parentScreen.getX() + 8, sx);

    sharedComboModal().show (*parent, std::move (panel),
                                /*onDismiss*/ [safeSelf]
                                {
                                    sharedComboModal().close();
                                    if (auto* s = safeSelf.getComponent())
                                        s->hidePopup();   // same reason as the pick path
                                });

    // EmbeddedModal centred the body during show(); now slam it (and
    // its backdrop) to the anchor position. repositionBody moves both
    // together - moving the body alone leaves the centred backdrop
    // rendering as a stray blank panel where the body used to be.
    const auto localTopLeft = parent->getLocalPoint (nullptr,
                                                        juce::Point<int> (sx, sy));
    sharedComboModal().repositionBody (localTopLeft);
}
} // namespace duskstudio
