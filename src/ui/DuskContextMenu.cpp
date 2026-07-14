#include "DuskContextMenu.h"
#include "EmbeddedModal.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
// One static modal per nesting level. Submenus open into a SECOND
// modal so the parent stays painted underneath while the submenu is
// visible - matches juce::PopupMenu's behaviour.
EmbeddedModal& sharedContextModal()
{
    static EmbeddedModal m;
    return m;
}
EmbeddedModal& sharedSubmenuModal()
{
    static EmbeddedModal m;
    return m;
}

struct Row
{
    juce::String text;
    int          itemId    = 0;
    bool         isHeader  = false;
    bool         isSep     = false;
    bool         isTicked  = false;
    bool         isEnabled = true;
    // Per-item callback fired on pick. juce::PopupMenu::Item stores
    // this in `action`; we copy the function object so the panel can
    // invoke it without keeping the source PopupMenu alive.
    std::function<void()> action;
    // Submenu rows pre-walked at walkMenu time. The original
    // juce::PopupMenu is typically a stack local (menu-bar idiom is
    // `auto menu = model->getMenuForIndex(); showContextMenu(menu);`)
    // and gets destroyed before the user expands the submenu, so we
    // capture submenu items by value here. shared_ptr keeps Row
    // cheaply copyable.
    std::shared_ptr<std::vector<Row>> subMenuRows;
};

std::vector<Row> walkMenu (const juce::PopupMenu& menu)
{
    std::vector<Row> rows;
    juce::PopupMenu::MenuItemIterator it (menu, /*recursive*/ false);
    while (it.next())
    {
        const auto& item = it.getItem();
        Row r;
        r.text     = item.text;
        r.itemId   = item.itemID;
        r.isHeader = item.isSectionHeader;
        r.isSep    = item.isSeparator;
        r.isTicked = item.isTicked;
        r.isEnabled = item.isEnabled;
        r.action   = item.action;
        if (item.subMenu != nullptr)
            r.subMenuRows = std::make_shared<std::vector<Row>> (walkMenu (*item.subMenu));
        rows.push_back (std::move (r));
    }
    return rows;
}

class DuskContextMenuPanel final : public juce::Component
{
public:
    DuskContextMenuPanel (std::vector<Row> rows,
                           std::function<void (int)> onPick,
                           bool isSubmenu)
        : rowsData (std::move (rows)),
          onPickFn (std::move (onPick)),
          submenuPanel (isSubmenu)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        constexpr int kRowH      = 32;   // sized for 16-pt body font
        constexpr int kSepH      = 10;
        constexpr int kHeaderH   = 26;
        constexpr int kVertPad   = 6;
        int total = kVertPad * 2;
        for (const auto& r : rowsData)
            total += r.isSep ? kSepH : (r.isHeader ? kHeaderH : kRowH);

        // Width: max of measured text widths + padding for tick + arrow.
        // Font size MUST match the row-paint font (16 pt, see paint())
        // - measuring at a smaller size undercounts and truncates the
        // last few characters (the "About Dusk Studio" -> "Studi" bug).
        const juce::Font font { juce::FontOptions (16.0f) };
        int maxText = 0;
        for (const auto& r : rowsData)
            maxText = std::max (maxText, (int) std::ceil (font.getStringWidthFloat (r.text)));
        const int w = std::clamp (maxText + 64, 160, 480);
        setSize (w, std::max (40, total));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);

        constexpr int kRowH      = 32;   // sized for 16-pt body font
        constexpr int kSepH      = 10;
        constexpr int kHeaderH   = 26;
        int y = 6;
        for (int i = 0; i < (int) rowsData.size(); ++i)
        {
            const auto& r = rowsData[(size_t) i];
            if (r.isSep)
            {
                g.setColour (juce::Colour (0xff2a2a30));
                g.drawHorizontalLine (y + kSepH / 2,
                                        (float) getX() + 4.0f,
                                        (float) getRight() - 4.0f);
                y += kSepH;
                continue;
            }
            if (r.isHeader)
            {
                g.setColour (juce::Colour (0xff8090a0));
                g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
                g.drawText (r.text.toUpperCase(),
                             juce::Rectangle<int> (10, y, getWidth() - 20, kHeaderH),
                             juce::Justification::centredLeft, false);
                y += kHeaderH;
                continue;
            }

            const auto rowR = juce::Rectangle<int> (0, y, getWidth(), kRowH);
            if (i == hoveredRow && r.isEnabled)
            {
                g.setColour (juce::Colour (0xff2a3a50));
                g.fillRect (rowR.reduced (2, 0));
            }
            g.setColour (r.isEnabled ? juce::Colours::white
                                      : juce::Colour (0xff707080));
            g.setFont (juce::Font (juce::FontOptions (16.0f)));
            const int textX = 28;
            g.drawText (r.text,
                         juce::Rectangle<int> (textX, y, getWidth() - textX - 22, kRowH),
                         juce::Justification::centredLeft, false);
            if (r.isTicked)
            {
                g.setColour (juce::Colour (0xff60d060));
                g.fillEllipse ((float) 8, (float) y + (float) kRowH * 0.5f - 3.5f, 7.0f, 7.0f);
            }
            if (r.subMenuRows != nullptr)
            {
                // Right-pointing arrow indicator.
                g.setColour (juce::Colour (0xffa0a0a8));
                juce::Path arrow;
                const float ax = (float) getWidth() - 12.0f;
                const float ay = (float) y + (float) kRowH * 0.5f;
                arrow.addTriangle (ax, ay - 4.0f, ax, ay + 4.0f, ax + 5.0f, ay);
                g.fillPath (arrow);
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
        if (r.isSep || r.isHeader || ! r.isEnabled) return;

        if (r.subMenuRows != nullptr)
        {
            openSubmenu (row, e.getScreenPosition());
            return;
        }

        // Fire per-item action (juce::PopupMenu::Item::action), then
        // user's onResult with the item ID. Capture locals BEFORE
        // closing the modal so the panel can deref them safely.
        auto action = r.action;
        const int id = r.itemId;
        auto onPick = onPickFn;
        // Close BOTH modals unconditionally - submenu first so the
        // parent doesn't briefly paint on top of a stale submenu, then
        // the parent. Earlier code skipped the parent close when the
        // pick came from a submenu, which left the parent visible after
        // a submenu selection.
        sharedSubmenuModal().close();
        sharedContextModal().close();
        if (action) action();
        if (onPick && id != 0) onPick (id);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            if (submenuPanel) sharedSubmenuModal().close();
            else              sharedContextModal().close();
            if (onPickFn) onPickFn (0);
            return true;
        }
        return false;
    }

private:
    int rowAtY (int y) const
    {
        constexpr int kRowH      = 32;   // sized for 16-pt body font
        constexpr int kSepH      = 10;
        constexpr int kHeaderH   = 26;
        int yy = 6;
        for (int i = 0; i < (int) rowsData.size(); ++i)
        {
            const int h = rowsData[(size_t) i].isSep ? kSepH
                              : (rowsData[(size_t) i].isHeader ? kHeaderH : kRowH);
            if (y >= yy && y < yy + h) return i;
            yy += h;
        }
        return -1;
    }

    void openSubmenu (int row, juce::Point<int> screenPos);

    std::vector<Row> rowsData;
    std::function<void (int)> onPickFn;
    bool submenuPanel = false;
    int hoveredRow = -1;
};

void DuskContextMenuPanel::openSubmenu (int row, juce::Point<int> screenPos)
{
    if (row < 0 || row >= (int) rowsData.size()) return;
    const auto& sub = rowsData[(size_t) row].subMenuRows;
    if (sub == nullptr) return;

    auto* host = getTopLevelComponent();
    if (host == nullptr) host = this;

    // Submenu rows were already deep-walked at parent-walk time so
    // we don't need to re-walk a juce::PopupMenu here (the original
    // is typically a stack local that's long dead by now).
    auto subRows = *sub;
    auto chainOnPick = onPickFn;
    auto subPanel = std::make_unique<DuskContextMenuPanel> (
        std::move (subRows),
        [chainOnPick] (int id)
        {
            if (chainOnPick && id != 0) chainOnPick (id);
        },
        /*isSubmenu*/ true);

    const int w = subPanel->getWidth();
    const int h = subPanel->getHeight();

    // Position: to the right of the parent row by default; flip left
    // if it would clip the host edge.
    const auto hostBounds = host->getScreenBounds();
    const auto myScreen   = getScreenBounds();
    int x = myScreen.getRight() + 2;
    if (x + w > hostBounds.getRight() - 8)
        x = std::max (hostBounds.getX() + 8, myScreen.getX() - w - 2);
    int y = screenPos.y - 4;
    y = std::clamp (y, hostBounds.getY() + 8, hostBounds.getBottom() - h - 8);

    // EmbeddedModal::show expects parent-local bounds; submodal sizes
    // itself, so pre-set bounds in screen and trust the modal helper
    // to convert. Simpler path: set the panel size + let the modal
    // host centre it, then manually move it after show().
    subPanel->setSize (w, h);
    sharedSubmenuModal().show (*host, std::move (subPanel),
                                 /*onDismiss*/ [] { sharedSubmenuModal().close(); },
                                 /*dismissOnClickOutside*/ true,
                                 /*dismissOnEscape*/ true);

    // Move the now-attached body + backdrop to the desired screen
    // position (centred-show + later reposition pattern).
    {
        const auto localTopLeft = host->getLocalPoint (nullptr,
                                                          juce::Point<int> (x, y));
        sharedSubmenuModal().repositionBody (localTopLeft);
    }
}

void showContextMenuAt (const juce::PopupMenu& menu,
                          juce::Component& host,
                          juce::Point<int> screenAnchor,
                          std::function<void (int)> onResult)
{
    auto rows = walkMenu (menu);
    if (rows.empty()) { if (onResult) onResult (0); return; }

    auto panel = std::make_unique<DuskContextMenuPanel> (
        std::move (rows), onResult, /*isSubmenu*/ false);

    const int w = panel->getWidth();
    const int h = panel->getHeight();

    const auto hostBounds = host.getScreenBounds();
    int x = screenAnchor.x;
    int y = screenAnchor.y;
    // Flip if the menu would clip the bottom edge.
    if (y + h > hostBounds.getBottom() - 8)
        y = std::max (hostBounds.getY() + 8, y - h);
    // Clamp right edge.
    if (x + w > hostBounds.getRight() - 8)
        x = std::max (hostBounds.getX() + 8, hostBounds.getRight() - w - 8);
    x = std::max (hostBounds.getX() + 8, x);
    y = std::max (hostBounds.getY() + 8, y);

    sharedContextModal().show (host, std::move (panel),
                                 /*onDismiss*/ [onResult]
                                 {
                                     sharedContextModal().close();
                                     if (onResult) onResult (0);
                                 },
                                 /*dismissOnClickOutside*/ true,
                                 /*dismissOnEscape*/ true);

    {
        const auto localTopLeft = host.getLocalPoint (nullptr,
                                                         juce::Point<int> (x, y));
        sharedContextModal().repositionBody (localTopLeft);
    }
}
} // namespace

void showContextMenu (const juce::PopupMenu& menu,
                       juce::Component& target,
                       std::function<void (int)> onResult,
                       juce::Point<int> screenPos)
{
    auto* host = target.getTopLevelComponent();
    if (host == nullptr) host = &target;
    juce::Point<int> anchor = screenPos;
    if (anchor.x < 0 || anchor.y < 0)
    {
        // Anchor below the target by default - matches PopupMenu's
        // withTargetComponent fallback positioning.
        const auto tb = target.getScreenBounds();
        anchor = { tb.getX(), tb.getBottom() + 2 };
    }
    showContextMenuAt (menu, *host, anchor, std::move (onResult));
}

void showContextMenu (const juce::PopupMenu& menu,
                       juce::Component& hostParent,
                       juce::Point<int> screenPos,
                       std::function<void (int)> onResult)
{
    auto* host = hostParent.getTopLevelComponent();
    if (host == nullptr) host = &hostParent;
    showContextMenuAt (menu, *host, screenPos, std::move (onResult));
}
} // namespace duskstudio
