#include "DuskComboBox.h"
#include "EmbeddedModal.h"

#include <cmath>

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

constexpr int kBorder      = 4;
constexpr int kTitleH      = 24;
constexpr int kRowH        = 26;
constexpr int kSepH        = 8;
constexpr int kScrollbarW  = 8;
constexpr int kTextXPad    = 24;  // tick column on the left of each row
constexpr int kTrailPad    = 18;
constexpr int kColGap       = 12; // gutter (holding the divider) between grid columns
constexpr int kHScrollH     = 10; // horizontal scrollbar strip at the popup's foot
constexpr int kGridCapRows  = 22; // keep the grid short: cap a column at this many rows
constexpr int kSearchH      = 30; // type-to-filter box atop the grid
constexpr int kCaretBlinkMs = 500; // filter-box caret blink half-period

struct Row
{
    juce::String text;
    int          itemId    = 0;       // 0 = non-clickable header / separator
    bool         isHeader  = false;
    bool         isSep     = false;
    bool         isTicked  = false;
    bool         isEnabled = true;
};

// One vertically-scrolling column of rows. Owns its rows, viewport `bounds`
// (in panel coordinates) and its scroll/hover state; paints and hit-tests
// entirely within `bounds`. An ordinary combo is one ListView spanning the
// popup. (The grid browser flows a flat list into columns instead.)
struct ListView
{
    std::vector<Row>     rows;
    juce::Rectangle<int> bounds;
    float scrollOffset = 0.0f;
    int   hovered      = -1;
    bool  dragging     = false;
    int   dragStartY   = 0;
    float dragStartOffset = 0.0f;

    int count() const noexcept { return (int) rows.size(); }

    int rowExtent (int index) const noexcept
    {
        return rows[(size_t) index].isSep ? kSepH : kRowH;
    }

    int contentHeight() const noexcept
    {
        int h = 0;
        for (int i = 0; i < count(); ++i)
            h += rowExtent (i);
        return h;
    }

    float maxScroll() const noexcept
    {
        return (float) juce::jmax (0, contentHeight() - bounds.getHeight());
    }

    int scrollbarW() const noexcept { return maxScroll() > 0.0f ? kScrollbarW : 0; }

    void setScroll (float offset) noexcept
    {
        scrollOffset = juce::jlimit (0.0f, maxScroll(), offset);
    }

    int rowTop (int index) const noexcept
    {
        int y = 0;
        for (int i = 0; i < index && i < count(); ++i)
            y += rowExtent (i);
        return y;
    }

    bool isSelectable (int index) const noexcept
    {
        if (index < 0 || index >= count()) return false;
        const auto& r = rows[(size_t) index];
        return ! r.isSep && ! r.isHeader && r.isEnabled && r.itemId != 0;
    }

    void ensureVisible (int index) noexcept
    {
        if (index < 0 || index >= count()) return;
        const int top    = rowTop (index);
        const int bottom = top + rowExtent (index);
        const int vh     = bounds.getHeight();
        if ((float) top < scrollOffset)
            setScroll ((float) top);
        else if ((float) bottom > scrollOffset + (float) vh)
            setScroll ((float) (bottom - vh));
    }

    int rowAtY (juce::Point<int> p) const noexcept
    {
        if (! bounds.contains (p)) return -1;
        int yy = bounds.getY() - juce::roundToInt (scrollOffset);
        for (int i = 0; i < count(); ++i)
        {
            const int h = rowExtent (i);
            if (p.y >= yy && p.y < yy + h) return i;
            yy += h;
        }
        return -1;
    }

    void moveHover (int direction, int steps) noexcept
    {
        int next = hovered;
        if (next < 0 || next >= count())
            next = direction > 0 ? -1 : count();

        int moved = 0;
        while (moved < steps)
        {
            do { next += direction; }
            while (next >= 0 && next < count() && ! isSelectable (next));
            if (next < 0 || next >= count()) break;
            hovered = next;
            ++moved;
        }
        ensureVisible (hovered);
    }

    void moveToBoundary (int direction) noexcept
    {
        int next = direction > 0 ? 0 : count() - 1;
        while (next >= 0 && next < count() && ! isSelectable (next))
            next += direction;
        if (isSelectable (next))
        {
            hovered = next;
            ensureVisible (hovered);
        }
    }

    juce::Rectangle<int> scrollbarTrack() const noexcept
    {
        if (scrollbarW() == 0) return {};
        return { bounds.getRight() - kScrollbarW, bounds.getY(), kScrollbarW, bounds.getHeight() };
    }

    juce::Rectangle<int> scrollbarThumb() const noexcept
    {
        const auto track = scrollbarTrack();
        if (track.isEmpty()) return {};
        const float visibleRatio = (float) bounds.getHeight() / (float) contentHeight();
        const int thumbHeight = juce::jmax (20, juce::roundToInt (
                                                    (float) track.getHeight() * visibleRatio));
        const int travel = juce::jmax (0, track.getHeight() - thumbHeight);
        const float progress = maxScroll() > 0.0f ? scrollOffset / maxScroll() : 0.0f;
        return { track.getX(), track.getY() + juce::roundToInt ((float) travel * progress),
                 track.getWidth(), thumbHeight };
    }

    bool pressScrollbar (juce::Point<int> p) noexcept
    {
        const auto track = scrollbarTrack();
        if (track.isEmpty() || ! track.contains (p)) return false;
        auto thumb = scrollbarThumb();
        if (! thumb.contains (p))
        {
            const int travel = juce::jmax (1, track.getHeight() - thumb.getHeight());
            const float progress = juce::jlimit (
                0.0f, 1.0f,
                (float) (p.y - track.getY() - thumb.getHeight() / 2) / (float) travel);
            setScroll (progress * maxScroll());
        }
        dragging = true;
        dragStartY = p.y;
        dragStartOffset = scrollOffset;
        return true;
    }

    void dragScrollbar (juce::Point<int> p) noexcept
    {
        if (! dragging) return;
        const auto track = scrollbarTrack();
        const auto thumb = scrollbarThumb();
        const int travel = juce::jmax (1, track.getHeight() - thumb.getHeight());
        setScroll (dragStartOffset + (float) (p.y - dragStartY)
                                        * maxScroll() / (float) travel);
    }

    void paint (juce::Graphics& g) const
    {
        g.saveState();
        g.reduceClipRegion (bounds);
        const int rowW = bounds.getWidth() - scrollbarW();
        int y = bounds.getY() - juce::roundToInt (scrollOffset);
        for (int i = 0; i < count(); ++i)
        {
            const auto& r = rows[(size_t) i];
            if (r.isSep)
            {
                g.setColour (juce::Colour (0xff2a2a30));
                g.drawHorizontalLine (y + kSepH / 2,
                                        (float) bounds.getX() + 4.0f,
                                        (float) (bounds.getX() + rowW - 4));
                y += kSepH;
                continue;
            }

            const auto rowR = juce::Rectangle<int> (bounds.getX(), y, rowW, kRowH);
            paintRow (g, rowR, r.text, r.isHeader, r.isEnabled, r.isTicked, i == hovered);
            y += kRowH;
        }
        g.restoreState();

        const auto track = scrollbarTrack();
        if (! track.isEmpty())
        {
            g.setColour (juce::Colour (0xff202028));
            g.fillRect (track);
            g.setColour (juce::Colour (0xff5a5a68));
            g.fillRect (scrollbarThumb().reduced (1, 0));
        }
    }

    // Shared row painter, also used by the grid.
    static void paintRow (juce::Graphics& g, juce::Rectangle<int> rowR,
                          const juce::String& text, bool isHeader, bool isEnabled,
                          bool isTicked, bool isHovered)
    {
        if (isHeader)
        {
            g.setColour (juce::Colour (0xff8090a0));
            g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
            g.drawText (text.toUpperCase(), rowR.reduced (10, 0),
                         juce::Justification::centredLeft, false);
            return;
        }
        if (isHovered)
        {
            g.setColour (juce::Colour (0xff2a3a50));
            g.fillRect (rowR.reduced (2, 0));
        }
        g.setColour (isEnabled ? juce::Colours::white : juce::Colour (0xff707080));
        g.setFont (juce::Font (juce::FontOptions (14.0f)));
        g.drawText (text,
                     rowR.withX (rowR.getX() + kTextXPad)
                          .withWidth (rowR.getWidth() - kTextXPad - 8),
                     juce::Justification::centredLeft, false);
        if (isTicked)
        {
            g.setColour (juce::Colour (0xff60d060));
            g.fillEllipse ((float) rowR.getX() + 8.0f,
                            (float) rowR.getCentreY() - 3.5f, 7.0f, 7.0f);
        }
    }
};

// Popup body. Ordinary combos render a single flat scrolling ListView. When the
// owning combo opts into the grid browser, the item list is flowed into a short,
// wide set of columns with a type-to-filter box on top: a long preset list
// (hundreds of SoundFont programs) reads across instead of scrolling forever.
// The list is presented in its incoming order (the SF2 editor sorts it
// program-first so an instrument's bank variants group together).
class DuskComboPanel final : public juce::Component,
                             private juce::Timer
{
public:
    using Row = duskstudio::Row;

    DuskComboPanel (juce::String headerTitle,
                     std::vector<Row> rows,
                     std::function<void (int)> onPick,
                     std::function<void()> onDismiss,
                     int maximumHeight,
                     int maximumWidth,
                     bool gridBrowser)
        : title (std::move (headerTitle)),
          onPickFn (std::move (onPick)),
          onDismissFn (std::move (onDismiss))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);

        // Grid needs room for the fixed chrome (title + search + h-scrollbar)
        // plus at least one preset row; a pathologically short window falls back
        // to the flat scrolling list.
        if (gridBrowser)
        {
            const int chrome = kBorder * 2 + titleHeight() + kSearchH + kHScrollH;
            useGrid = (juce::jmax (1, maximumHeight) - chrome) / kRowH >= 1;
        }

        if (useGrid) layoutGrid (std::move (rows), maximumHeight, maximumWidth);
        else         layoutFlat (std::move (rows), maximumHeight);

        setTitle (title.isNotEmpty() ? title : juce::String ("Options"));
        setDescription (useGrid
                            ? "Preset browser. Type to filter, arrow keys to move, "
                              "Return to choose, Escape to close."
                            : "Arrow keys to move, Return to choose, Escape to close.");
        announceActive();

        if (useGrid) startTimer (kCaretBlinkMs);   // blinking caret in the filter box
    }

    void timerCallback() override
    {
        caretOn = ! caretOn;
        repaint (searchRect());
    }

    // The wide grid reads better centred in the window than crammed under the
    // combo at the screen edge; the flat list stays anchored to its combo.
    bool prefersCentred() const noexcept { return useGrid; }

    void resized() override
    {
        if (useGrid)
        {
            setHScroll (hScroll);
            ensureColumnVisible (hoverCol);
        }
        else
        {
            auto area = getLocalBounds().reduced (kBorder);
            area.removeFromTop (titleHeight());
            flat.bounds = area;
            flat.setScroll (flat.scrollOffset);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);

        auto area = getLocalBounds().reduced (kBorder);
        if (title.isNotEmpty())
        {
            auto header = area.removeFromTop (kTitleH);
            g.setColour (juce::Colour (0xffe8e8ec));
            g.setFont (juce::Font (juce::FontOptions (14.5f, juce::Font::bold)));
            g.drawText (title, header.reduced (8, 0),
                         juce::Justification::centredLeft, false);
            g.setColour (juce::Colour (0xff2a2a30));
            g.drawHorizontalLine (header.getBottom() - 1,
                                    (float) header.getX() + 4.0f,
                                    (float) header.getRight() - 4.0f);
        }

        if (useGrid) { paintSearch (g); paintGrid (g); }
        else         flat.paint (g);
    }

    // hoverCol/hoverRow (and flat.hovered) are the ACTIVE cell - what Return
    // picks and what arrow keys move from - not a pure pointer-hover highlight.
    // So a pointer that lands on the search box, a scrollbar, a column gutter,
    // or leaves the panel entirely only stops tracking; it must not wipe the
    // active cell out from under the keyboard.
    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto p = e.getPosition();
        if (useGrid)
        {
            const auto cell = cellAt (p);
            if (! isSelectableCell (cell.first, cell.second)) return;
            if (cell.first != hoverCol || cell.second != hoverRow)
            {
                // Not setGridHover: that scrolls the column fully into view,
                // which would yank the grid sideways under a pointer resting on
                // a partially-visible edge column.
                hoverCol = cell.first; hoverRow = cell.second;
                announceActive();
                repaint();
            }
            return;
        }
        if (flat.scrollbarTrack().contains (p)) return;
        const int nh = flat.rowAtY (p);
        if (flat.isSelectable (nh) && nh != flat.hovered) { flat.hovered = nh; announceActive(); repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.getPosition();

        if (useGrid)
        {
            if (filterText.isNotEmpty() && clearButtonRect().contains (p)) { setFilter ({}); return; }
            if (pressHScrollbar (p)) { hbarDragging = true; repaint(); return; }
            const auto cell = cellAt (p);
            if (isSelectableCell (cell.first, cell.second))
                pickItem (cellRef (cell.first, cell.second).itemId);
            return;
        }

        if (flat.pressScrollbar (p)) { repaint(); return; }
        const int row = flat.rowAtY (p);
        if (flat.isSelectable (row))
            pickItem (flat.rows[(size_t) row].itemId);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (useGrid)
        {
            if (hbarDragging) { dragHScrollbar (e.getPosition()); repaint(); }
            return;
        }
        if (flat.dragging) { flat.dragScrollbar (e.getPosition()); repaint(); }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        flat.dragging = false;
        hbarDragging = false;
    }

    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (useGrid)
        {
            if (maxHScroll <= 0) return;
            // A vertical wheel scrolls the grid sideways (its only scroll axis);
            // a horizontal wheel maps straight through.
            const float delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY)
                                    ? wheel.deltaX : wheel.deltaY;
            setHScroll (hScroll - delta * 90.0f);
            repaint();
            return;
        }
        if (flat.maxScroll() <= 0.0f) return;
        flat.setScroll (flat.scrollOffset - wheel.deltaY * 80.0f);
        // Follow the row that scrolled under the pointer, but keep the active
        // row when the pointer isn't over one (see mouseMove).
        if (! flat.scrollbarTrack().contains (e.getPosition()))
        {
            const int nh = flat.rowAtY (e.getPosition());
            if (flat.isSelectable (nh) && nh != flat.hovered) { flat.hovered = nh; announceActive(); }
        }
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            // In the grid, Esc first clears an active filter; a second Esc closes.
            if (useGrid && filterText.isNotEmpty()) { setFilter ({}); return true; }
            if (onDismissFn) { auto cb = onDismissFn; cb(); }   // survives panel teardown
            return true;
        }

        if (useGrid)
            return gridKey (k);

        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            flat.moveHover (k == juce::KeyPress::upKey ? -1 : 1, 1);
            announceActive();
            repaint();
            return true;
        }
        if (k == juce::KeyPress::pageUpKey || k == juce::KeyPress::pageDownKey)
        {
            flat.moveHover (k == juce::KeyPress::pageUpKey ? -1 : 1,
                            juce::jmax (1, flat.bounds.getHeight() / kRowH));
            announceActive();
            repaint();
            return true;
        }
        if (k == juce::KeyPress::homeKey || k == juce::KeyPress::endKey)
        {
            flat.moveToBoundary (k == juce::KeyPress::homeKey ? 1 : -1);
            announceActive();
            repaint();
            return true;
        }
        if (k == juce::KeyPress::returnKey && flat.isSelectable (flat.hovered))
        {
            activateActive();
            return true;
        }
        return false;
    }

    // The rows are painted, not child components, so there is nothing for a
    // screen reader to walk. The panel itself takes keyboard focus, so expose it
    // as the popup and report the active row through its accessible title,
    // re-announced on every navigation. `press` activates the active row, the
    // same as Return.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (
            *this,
            juce::AccessibilityRole::popupMenu,
            juce::AccessibilityActions().addAction (
                juce::AccessibilityActionType::press, [this] { activateActive(); }));
    }

private:
    struct Cell
    {
        juce::String text;
        int  itemId    = 0;
        bool isTicked  = false;
        bool isEnabled = true;
    };

    int titleHeight() const noexcept { return title.isEmpty() ? 0 : kTitleH; }

    juce::String activeItemText() const
    {
        if (useGrid)
            return isSelectableCell (hoverCol, hoverRow) ? cellRef (hoverCol, hoverRow).text
                                                          : juce::String();
        return flat.isSelectable (flat.hovered) ? flat.rows[(size_t) flat.hovered].text
                                                 : juce::String();
    }

    juce::String emptyResultsMessage() const
    {
        return filterText.isEmpty() ? juce::String ("No presets")
                                    : "No presets match \"" + filterText + "\"";
    }

    void announceActive()
    {
        auto label = activeItemText();
        if (label.isEmpty())
        {
            // A no-match filter has no active cell; announce that state to a
            // screen reader rather than leaving the stale last-preset title.
            if (! useGrid || ! columns.empty()) return;
            label = emptyResultsMessage();
        }
        setTitle (label);
        if (auto* h = getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::titleChanged);
    }

    // onPickFn closes the shared modal, which defer-destroys this panel. A
    // nested message pump inside the caller's onChange could run that teardown
    // mid-call and free the member std::function we're executing - the stack
    // copy outlives it.
    void pickItem (int itemId)
    {
        if (! onPickFn) return;
        auto cb = onPickFn;
        cb (itemId);
    }

    void activateActive()
    {
        if (useGrid)
        {
            if (isSelectableCell (hoverCol, hoverRow))
                pickItem (cellRef (hoverCol, hoverRow).itemId);
        }
        else if (flat.isSelectable (flat.hovered))
        {
            pickItem (flat.rows[(size_t) flat.hovered].itemId);
        }
    }

    // ---- flat combo ----------------------------------------------------

    void layoutFlat (std::vector<Row> rows, int maximumHeight)
    {
        flat.rows = std::move (rows);

        const juce::Font rowFont   { juce::FontOptions (14.0f) };
        const juce::Font headerFont{ juce::FontOptions (14.5f, juce::Font::bold) };
        int maxTextW = 0;
        for (const auto& r : flat.rows)
        {
            if (r.isSep) continue;
            const auto& f = r.isHeader ? headerFont : rowFont;
            maxTextW = juce::jmax (maxTextW, (int) std::ceil (f.getStringWidthFloat (r.text)));
        }
        if (title.isNotEmpty())
            maxTextW = juce::jmax (maxTextW, (int) std::ceil (headerFont.getStringWidthFloat (title)));

        for (int i = 0; i < flat.count(); ++i)
            if (flat.rows[(size_t) i].isTicked)
                flat.hovered = i;

        const int w = juce::jlimit (100, 320, maxTextW + kTextXPad + kTrailPad);
        const int naturalHeight = kBorder * 2 + titleHeight() + flat.contentHeight();
        setSize (w, juce::jlimit (1, juce::jmax (60, naturalHeight),
                                  juce::jmax (1, maximumHeight)));
        flat.ensureVisible (flat.hovered);
    }

    // ---- grid browser: flat list flowed into short, wide columns -------

    void layoutGrid (std::vector<Row> rows, int maximumHeight, int maximumWidth)
    {
        // Keep the full item list for the lifetime of the popup so the
        // type-to-filter can rebuild the columns without re-parsing. Headers /
        // separators (if any) are not part of the flat program list.
        const juce::Font rowFont { juce::FontOptions (14.0f) };
        int widest = 0;
        for (auto& r : rows)
        {
            if (r.isSep || r.isHeader) continue;
            Cell c;
            c.text = r.text; c.itemId = r.itemId;
            c.isTicked = r.isTicked; c.isEnabled = r.isEnabled;
            widest = juce::jmax (widest, (int) std::ceil (rowFont.getStringWidthFloat (c.text)));
            allItems.push_back (std::move (c));
        }
        colWidth = juce::jlimit (150, 300, widest + kTextXPad + kTrailPad);

        // Rows per column: derived from the available height and bounded so the
        // popup stays short. Overflow flows sideways into more columns.
        const int chrome = kBorder * 2 + titleHeight() + kSearchH + kHScrollH;
        rowsPerColumn = juce::jlimit (1, kGridCapRows,
                                       (juce::jmax (1, maximumHeight) - chrome) / kRowH);

        // Size from the unfiltered layout (its widest case); the filter only ever
        // shrinks the live column set within this fixed frame.
        buildColumnsFiltered();

        int usedRows = 0;
        for (const auto& c : columns)
            usedRows = juce::jmax (usedRows, (int) c.size());
        usedRows = juce::jmax (1, usedRows);

        const int totalCols   = (int) columns.size();
        const int availW      = juce::jmax (1, maximumWidth) - kBorder * 2;
        const int visibleCols = juce::jlimit (1, juce::jmax (1, totalCols),
                                               (availW + kColGap) / colStride());
        const int viewportW   = visibleCols * colWidth + (visibleCols - 1) * kColGap;

        const int w = kBorder * 2 + viewportW;
        const int h = kBorder * 2 + titleHeight() + kSearchH + usedRows * kRowH + kHScrollH;
        setSize (w, juce::jlimit (1, h, juce::jmax (1, maximumHeight)));

        finishGrid (/*fromFilter*/ false);
    }

    // Rebuild `columns` by filtering the flat item list on the current text and
    // flowing the survivors top-to-bottom, wrapping into the next column.
    void buildColumnsFiltered()
    {
        columns.clear();
        std::vector<Cell> kept;
        kept.reserve (allItems.size());
        for (const auto& it : allItems)
            if (filterText.isEmpty() || it.text.containsIgnoreCase (filterText))
                kept.push_back (it);
        if (kept.empty()) return;

        const int rpc = juce::jmax (1, rowsPerColumn);
        for (size_t i = 0; i < kept.size(); i += (size_t) rpc)
        {
            columns.push_back ({});
            const size_t end = juce::jmin (i + (size_t) rpc, kept.size());
            for (size_t j = i; j < end; ++j)
                columns.back().push_back (kept[j]);
        }
    }

    // Recompute horizontal-scroll extent and the focused cell.
    void finishGrid (bool fromFilter)
    {
        const int totalCols = (int) columns.size();
        const int contentW  = totalCols > 0 ? totalCols * colWidth + (totalCols - 1) * kColGap : 0;
        maxHScroll = juce::jmax (0, contentW - gridViewport().getWidth());

        hoverCol = hoverRow = -1;
        if (fromFilter)
        {
            hScroll = 0.0f;
            for (int c = 0; c < totalCols && hoverCol < 0; ++c)
                for (int r = 0; r < (int) columns[(size_t) c].size(); ++r)
                    if (isSelectableCell (c, r)) { hoverCol = c; hoverRow = r; break; }
            setHScroll (0.0f);
        }
        else
        {
            for (int c = 0; c < totalCols && hoverCol < 0; ++c)
                for (int r = 0; r < (int) columns[(size_t) c].size(); ++r)
                    if (columns[(size_t) c][(size_t) r].isTicked) { hoverCol = c; hoverRow = r; break; }
            setHScroll (hScroll);
            ensureColumnVisible (hoverCol);
        }
    }

    void setFilter (juce::String f)
    {
        if (f == filterText) return;
        filterText = std::move (f);
        buildColumnsFiltered();
        finishGrid (/*fromFilter*/ true);
        announceActive();
        caretOn = true;             // solid right after a keystroke, then resume blinking
        startTimer (kCaretBlinkMs);
        repaint();
    }

    juce::Rectangle<int> searchRect() const noexcept
    {
        auto area = getLocalBounds().reduced (kBorder);
        area.removeFromTop (titleHeight());
        return area.removeFromTop (kSearchH).reduced (4, 4);
    }

    juce::Rectangle<int> clearButtonRect() const noexcept
    {
        auto r = searchRect();
        return r.removeFromRight (24);
    }

    juce::Rectangle<int> gridViewport() const noexcept
    {
        auto area = getLocalBounds().reduced (kBorder);
        area.removeFromTop (titleHeight());
        area.removeFromTop (kSearchH);
        area.removeFromBottom (kHScrollH);
        return area;
    }

    int colStride() const noexcept { return colWidth + kColGap; }

    const Cell& cellRef (int col, int row) const noexcept
    {
        return columns[(size_t) col][(size_t) row];
    }

    bool isSelectableCell (int col, int row) const noexcept
    {
        if (col < 0 || col >= (int) columns.size()) return false;
        if (row < 0 || row >= (int) columns[(size_t) col].size()) return false;
        const auto& c = cellRef (col, row);
        return c.isEnabled && c.itemId != 0;
    }

    std::pair<int,int> cellAt (juce::Point<int> p) const noexcept
    {
        const auto vp = gridViewport();
        if (! vp.contains (p)) return { -1, -1 };
        const float lx = (float) (p.x - vp.getX()) + hScroll;
        const int col = (int) std::floor (lx / (float) colStride());
        if (col < 0 || col >= (int) columns.size()) return { -1, -1 };
        if (lx - (float) (col * colStride()) >= (float) colWidth) return { -1, -1 }; // in the gutter
        const int row = (p.y - vp.getY()) / kRowH;
        if (row < 0 || row >= (int) columns[(size_t) col].size()) return { -1, -1 };
        return { col, row };
    }

    void setHScroll (float x) noexcept
    {
        hScroll = juce::jlimit (0.0f, (float) maxHScroll, x);
    }

    void ensureColumnVisible (int col) noexcept
    {
        if (col < 0) return;
        const int left  = col * colStride();
        const int right = left + colWidth;
        const int vw    = gridViewport().getWidth();
        if ((float) left < hScroll)              setHScroll ((float) left);
        else if ((float) right > hScroll + (float) vw) setHScroll ((float) (right - vw));
    }

    void paintSearch (juce::Graphics& g) const
    {
        const auto r = searchRect();
        g.setColour (juce::Colour (0xff101014));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (r.toFloat(), 4.0f, 1.0f);

        // Magnifier glyph.
        g.setColour (juce::Colour (0xff6b7280));
        const float gx = (float) r.getX() + 9.0f, gy = (float) r.getCentreY() - 4.0f;
        g.drawEllipse (gx, gy, 7.0f, 7.0f, 1.3f);
        g.drawLine (gx + 6.0f, gy + 6.0f, gx + 9.0f, gy + 9.0f, 1.3f);

        const juce::Font searchFont { juce::FontOptions (13.5f) };
        g.setFont (searchFont);
        const auto textArea = r.withTrimmedLeft (26)
                               .withTrimmedRight (filterText.isNotEmpty() ? 26 : 8);
        const int textW = filterText.isEmpty()
                            ? 0 : (int) std::ceil (searchFont.getStringWidthFloat (filterText));
        if (filterText.isEmpty())
        {
            g.setColour (juce::Colour (0xff606672));
            g.drawText ("Type to filter presets...", textArea.withTrimmedLeft (3),
                         juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour (juce::Colour (0xffe8e8ec));
            g.drawText (filterText, textArea, juce::Justification::centredLeft, false);
            // Clear (x) glyph, drawn as two strokes so it never depends on font
            // coverage for a multi-byte character.
            const auto cb = clearButtonRect().toFloat().reduced (7.0f, 8.0f);
            g.setColour (juce::Colour (0xff9098a4));
            g.drawLine (cb.getX(), cb.getY(), cb.getRight(), cb.getBottom(), 1.4f);
            g.drawLine (cb.getX(), cb.getBottom(), cb.getRight(), cb.getY(), 1.4f);
        }

        // Blinking caret so it reads as an active text field (just type - no click).
        if (caretOn)
        {
            const int cx = juce::jmin (textArea.getX() + textW + 1, textArea.getRight());
            g.setColour (juce::Colour (0xffcfd6e2));
            g.fillRect (cx, r.getCentreY() - 8, 1, 16);
        }
    }

    void paintGrid (juce::Graphics& g)
    {
        const auto vp = gridViewport();
        if (columns.empty())
        {
            g.setColour (juce::Colour (0xff707888));
            g.setFont (juce::Font (juce::FontOptions (14.0f)));
            g.drawText (emptyResultsMessage(), vp, juce::Justification::centred, false);
            return;
        }
        g.saveState();
        g.reduceClipRegion (vp);
        for (int c = 0; c < (int) columns.size(); ++c)
        {
            const int x = vp.getX() - juce::roundToInt (hScroll) + c * colStride();
            if (x + colWidth < vp.getX() || x > vp.getRight()) continue;

            int y = vp.getY();
            for (int r = 0; r < (int) columns[(size_t) c].size(); ++r)
            {
                const auto& cell = cellRef (c, r);
                const auto rowR = juce::Rectangle<int> (x, y, colWidth, kRowH);
                ListView::paintRow (g, rowR, cell.text, /*isHeader*/ false, cell.isEnabled,
                                    cell.isTicked, c == hoverCol && r == hoverRow);
                y += kRowH;
            }
            if (c + 1 < (int) columns.size())
            {
                g.setColour (juce::Colour (0xff26262e));
                g.drawVerticalLine (x + colWidth + kColGap / 2,
                                     (float) vp.getY(), (float) vp.getBottom());
            }
        }
        g.restoreState();

        if (maxHScroll > 0)
        {
            const auto track = hScrollbarTrack();
            g.setColour (juce::Colour (0xff202028));
            g.fillRect (track);
            g.setColour (juce::Colour (0xff5a5a68));
            g.fillRect (hScrollbarThumb().reduced (0, 1));
        }
    }

    juce::Rectangle<int> hScrollbarTrack() const noexcept
    {
        if (maxHScroll <= 0) return {};
        const auto vp = gridViewport();
        return { vp.getX(), vp.getBottom(), vp.getWidth(), kHScrollH };
    }

    juce::Rectangle<int> hScrollbarThumb() const noexcept
    {
        const auto track = hScrollbarTrack();
        if (track.isEmpty()) return {};
        const int contentW = maxHScroll + track.getWidth();
        const float ratio = (float) track.getWidth() / (float) contentW;
        const int thumbW = juce::jmax (24, juce::roundToInt ((float) track.getWidth() * ratio));
        const int travel = juce::jmax (0, track.getWidth() - thumbW);
        const float progress = maxHScroll > 0 ? hScroll / (float) maxHScroll : 0.0f;
        return { track.getX() + juce::roundToInt ((float) travel * progress), track.getY(),
                 thumbW, track.getHeight() };
    }

    bool pressHScrollbar (juce::Point<int> p) noexcept
    {
        const auto track = hScrollbarTrack();
        if (track.isEmpty() || ! track.contains (p)) return false;
        auto thumb = hScrollbarThumb();
        if (! thumb.contains (p))
        {
            const int travel = juce::jmax (1, track.getWidth() - thumb.getWidth());
            const float progress = juce::jlimit (
                0.0f, 1.0f,
                (float) (p.x - track.getX() - thumb.getWidth() / 2) / (float) travel);
            setHScroll (progress * (float) maxHScroll);
        }
        hbarDragStartX = p.x;
        hbarDragStartScroll = hScroll;
        return true;
    }

    void dragHScrollbar (juce::Point<int> p) noexcept
    {
        const auto track = hScrollbarTrack();
        const auto thumb = hScrollbarThumb();
        const int travel = juce::jmax (1, track.getWidth() - thumb.getWidth());
        setHScroll (hbarDragStartScroll
                      + (float) (p.x - hbarDragStartX) * (float) maxHScroll / (float) travel);
    }

    int firstSelectableInColumn (int col, int fromRow, int dir) const noexcept
    {
        if (col < 0 || col >= (int) columns.size()) return -1;
        for (int r = fromRow; r >= 0 && r < (int) columns[(size_t) col].size(); r += dir)
            if (isSelectableCell (col, r)) return r;
        return -1;
    }

    int nearestSelectableInColumn (int col, int targetRow) const noexcept
    {
        if (col < 0 || col >= (int) columns.size()) return -1;
        const int rows = (int) columns[(size_t) col].size();
        if (rows == 0) return -1;
        // targetRow is a row index carried over from a possibly-taller source
        // column; clamp it into this column so the neighbour search reaches its
        // cells instead of scanning entirely past the shorter column.
        targetRow = juce::jlimit (0, rows - 1, targetRow);
        for (int d = 0; d < rows; ++d)
        {
            if (isSelectableCell (col, targetRow + d)) return targetRow + d;
            if (isSelectableCell (col, targetRow - d)) return targetRow - d;
        }
        return -1;
    }

    void setGridHover (int col, int row)
    {
        hoverCol = col; hoverRow = row;
        ensureColumnVisible (col);
        announceActive();
        repaint();
    }

    bool gridKey (const juce::KeyPress& k)
    {
        // Type-to-filter. Handled before the empty-grid guard so backspace can
        // recover from a filter that matched nothing.
        if (k == juce::KeyPress::backspaceKey)
        {
            if (filterText.isNotEmpty()) setFilter (filterText.dropLastCharacters (1));
            return true;
        }
        const bool isNav = k == juce::KeyPress::upKey     || k == juce::KeyPress::downKey
                        || k == juce::KeyPress::leftKey    || k == juce::KeyPress::rightKey
                        || k == juce::KeyPress::pageUpKey  || k == juce::KeyPress::pageDownKey
                        || k == juce::KeyPress::homeKey    || k == juce::KeyPress::endKey
                        || k == juce::KeyPress::returnKey  || k == juce::KeyPress::tabKey;
        if (! isNav)
        {
            // Shortcut chords (Cmd+W, Alt+F, ...) must not type into the filter;
            // the popup is modal, so swallow them instead.
            const auto mods = k.getModifiers();
            if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
                return true;
            const juce::juce_wchar ch = k.getTextCharacter();
            if (ch >= 32 && ch != 127)
            {
                setFilter (filterText + juce::String::charToString (ch));
                return true;
            }
        }

        const int totalCols = (int) columns.size();
        // Filter matched nothing: no cells to navigate, but the popup is still
        // modal - swallow the key so it can't leak to the app behind it.
        if (totalCols == 0) return true;

        if (k == juce::KeyPress::returnKey)
        {
            activateActive();
            return true;
        }
        if (k == juce::KeyPress::homeKey || k == juce::KeyPress::endKey)
        {
            const int dir = (k == juce::KeyPress::homeKey) ? 1 : -1;
            const int startCol = dir > 0 ? 0 : totalCols - 1;
            for (int c = startCol; c >= 0 && c < totalCols; c += dir)
            {
                const int rows = (int) columns[(size_t) c].size();
                const int r = firstSelectableInColumn (c, dir > 0 ? 0 : rows - 1, dir);
                if (r >= 0) { setGridHover (c, r); return true; }
            }
            return true;
        }

        if (hoverCol < 0)   // nothing focused yet: land on the first preset
        {
            for (int c = 0; c < totalCols; ++c)
            {
                const int r = firstSelectableInColumn (c, 0, 1);
                if (r >= 0) { setGridHover (c, r); return true; }
            }
            return true;
        }

        if (k == juce::KeyPress::downKey)
        {
            int r = firstSelectableInColumn (hoverCol, hoverRow + 1, 1);
            if (r >= 0) { setGridHover (hoverCol, r); return true; }
            for (int c = hoverCol + 1; c < totalCols; ++c)
                if ((r = firstSelectableInColumn (c, 0, 1)) >= 0) { setGridHover (c, r); return true; }
            return true;
        }
        if (k == juce::KeyPress::upKey)
        {
            int r = firstSelectableInColumn (hoverCol, hoverRow - 1, -1);
            if (r >= 0) { setGridHover (hoverCol, r); return true; }
            for (int c = hoverCol - 1; c >= 0; --c)
                if ((r = firstSelectableInColumn (c, (int) columns[(size_t) c].size() - 1, -1)) >= 0)
                    { setGridHover (c, r); return true; }
            return true;
        }
        if (k == juce::KeyPress::rightKey || k == juce::KeyPress::pageDownKey)
        {
            for (int c = hoverCol + 1; c < totalCols; ++c)
            {
                const int r = nearestSelectableInColumn (c, hoverRow);
                if (r >= 0) { setGridHover (c, r); return true; }
            }
            return true;
        }
        if (k == juce::KeyPress::leftKey || k == juce::KeyPress::pageUpKey)
        {
            for (int c = hoverCol - 1; c >= 0; --c)
            {
                const int r = nearestSelectableInColumn (c, hoverRow);
                if (r >= 0) { setGridHover (c, r); return true; }
            }
            return true;
        }
        // A dropdown is modal: swallow every remaining key so none leak to the
        // app's transport / edit shortcuts behind the popup.
        return true;
    }

    juce::String title;
    std::function<void (int)> onPickFn;
    std::function<void()> onDismissFn;

    bool useGrid = false;
    ListView flat;                 // used when !useGrid

    std::vector<Cell> allItems;               // full program-sorted list; source for the filter
    juce::String filterText;
    std::vector<std::vector<Cell>> columns;   // filtered + flowed view (useGrid)
    int   colWidth      = 0;
    int   rowsPerColumn = 0;
    int   maxHScroll    = 0;
    float hScroll       = 0.0f;
    int   hoverCol      = -1;
    int   hoverRow      = -1;
    bool  hbarDragging  = false;
    int   hbarDragStartX = 0;
    float hbarDragStartScroll = 0.0f;
    bool  caretOn       = true;
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

    std::vector<Row> rows;
    rows.reserve ((size_t) getNumItems() + 4);
    juce::PopupMenu::MenuItemIterator it (*root, /*recursive*/ false);
    while (it.next())
    {
        const auto& item = it.getItem();
        Row r;
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
    auto dismiss = [safeSelf]
    {
        sharedComboModal().close();
        if (auto* s = safeSelf.getComponent())
            s->hidePopup();
    };
    // Click-outside dismissal: close WITHOUT restoring focus so the control the
    // user clicked outside the popup keeps the focus it just took (useOverlay is
    // false here, so the click lands on a real sibling, not a dim overlay).
    auto dismissOutside = [safeSelf]
    {
        sharedComboModal().close (/*restoreFocus*/ false);
        if (auto* s = safeSelf.getComponent())
            s->hidePopup();
    };
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
                // Notify asynchronously. sendNotificationSync runs juce::
                // ComboBox's post-action path (accessibility handler focus
                // grab + dynamic_cast into the JUCE peer) inline, which has
                // been observed to abort with __dynamic_cast on Linux/
                // XWayland when called from inside our modal teardown.
                // sendNotificationAsync defers the full notification path
                // (ComboBox::Listeners, onChange, accessibility event) to
                // the next message-loop tick, after teardown has finished.
                // ComboBox itself skips the notification when the id is
                // unchanged.
                s->setSelectedId (chosenId, juce::sendNotificationAsync);
            }
        },
        dismiss,
        juce::jmax (1, parent->getHeight() - 16),
        juce::jmax (1, parent->getWidth() - 16),
        gridBrowser);

    // Anchor the popup BELOW the combo's screen bounds (or above when it
    // would clip the bottom edge), instead of centring in the parent
    // window. Without this the popup floats in the middle of the screen
    // far from the click - confusing and easy to miss. The wide grid
    // browser is the exception: it stays centred (see repositionBody below).
    const bool centred = panel->prefersCentred();
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

    // A combo is a lightweight popup, not a new modal task: retain the normal
    // undimmed view of the editor behind it. Combos can live inside a tagged
    // plugin editor (the SoundFont editor is one example) - hiding plugin
    // editors there would hide the combo's own editor and expose only its
    // opaque EmbeddedModal backdrop as a large black rectangle. Combos outside
    // any tagged editor still need the hide: native editor child windows paint
    // above JUCE components and would bury the popup.
    bool insideTaggedEditor = false;
    for (auto* c = static_cast<juce::Component*> (this); c != nullptr; c = c->getParentComponent())
        if ((bool) c->getProperties().getWithDefault (kPluginEditorTag, false))
            { insideTaggedEditor = true; break; }

    // Only the type-to-filter grid captures typing; flat dropdowns keep
    // forwarding transport shortcuts (Space, R, ...) to the app.
    sharedComboModal().show (*parent, std::move (panel), dismiss,
                             /*dismissOnClickOutside*/ true,
                             /*dismissOnEscape*/ true,
                             /*dimAlpha*/ 0.0f,
                             /*hidePluginEditors*/ ! insideTaggedEditor,
                             /*useOverlay*/ false,
                             /*forwardShortcuts*/ ! centred,
                             /*onDismissOutside*/ dismissOutside);

    // EmbeddedModal centred the body during show(); now slam it (and
    // its backdrop) to the anchor position. repositionBody moves both
    // together - moving the body alone leaves the centred backdrop
    // rendering as a stray blank panel where the body used to be. The
    // grid browser opts out and keeps show()'s centring.
    if (! centred)
    {
        const auto localTopLeft = parent->getLocalPoint (nullptr,
                                                            juce::Point<int> (sx, sy));
        sharedComboModal().repositionBody (localTopLeft);
    }
}
} // namespace duskstudio
