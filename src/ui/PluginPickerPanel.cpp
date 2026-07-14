#include "PluginPickerPanel.h"

#include <algorithm>

namespace duskstudio
{

// Self-contained scrollable list. Rolls its own scroll offset instead
// of relying on juce::Viewport - Viewport's internal contentHolder
// sizing under our nested-EmbeddedModal setup left rows invisible on
// first open.
class PluginPickerPanel::ListBody final : public juce::Component
{
public:
    enum class Group { Manufacturer, Type };

    ListBody (juce::Array<juce::PluginDescription> descs,
              std::function<void (const juce::PluginDescription&)> picker)
        : rawDescs (std::move (descs)), onPick (std::move (picker))
    {
        rebuild();
    }

    void setGroup (Group g)
    {
        if (g == group) return;
        group = g;
        rebuild();
    }

    // Header key a description sorts/groups under, per current group mode.
    static juce::String groupKeyFor (const juce::PluginDescription& d, Group g)
    {
        if (g == Group::Manufacturer)
            return d.manufacturerName.isEmpty() ? juce::String ("(unknown)")
                                                : d.manufacturerName;

        // Type: the plugin category, reduced to its most specific token
        // ("Fx|EQ" -> "EQ"). Falls back to instrument/effect when the
        // scanned description carries no category (e.g. native CLAP).
        auto cat = d.category.trim();
        if (cat.containsChar ('|'))
            cat = cat.fromLastOccurrenceOf ("|", false, false).trim();
        if (cat.isNotEmpty())
            return cat;
        return d.isInstrument ? juce::String ("Instrument") : juce::String ("Effect");
    }

    void rebuild()
    {
        auto descs = rawDescs;
        const auto g = group;
        std::sort (descs.begin(), descs.end(),
            [g] (const juce::PluginDescription& a, const juce::PluginDescription& b)
            {
                const auto ka = groupKeyFor (a, g);
                const auto kb = groupKeyFor (b, g);
                if (ka != kb) return ka.compareIgnoreCase (kb) < 0;
                return a.name.compareIgnoreCase (b.name) < 0;
            });

        allEntries.clear();
        juce::String currentKey;
        bool first = true;
        for (int i = 0; i < descs.size(); ++i)
        {
            const auto& d = descs.getReference (i);
            const auto key = groupKeyFor (d, g);
            if (first || key != currentKey)
            {
                allEntries.push_back ({ true, key, {} });
                currentKey = key;
                first = false;
            }
            juce::String label = d.name;
            if (d.pluginFormatName.isNotEmpty())
                label += "  (" + d.pluginFormatName + ")";
            allEntries.push_back ({ false, label, d });
        }
        applyFilter (currentFilter);
    }

    void applyFilter (const juce::String& needle)
    {
        currentFilter = needle;
        visibleEntries.clear();
        if (needle.isEmpty())
        {
            visibleEntries = allEntries;
        }
        else
        {
            const auto lc = needle.toLowerCase();
            size_t i = 0;
            while (i < allEntries.size())
            {
                if (allEntries[i].isHeader)
                {
                    size_t j = i + 1;
                    std::vector<Entry> matches;
                    while (j < allEntries.size() && ! allEntries[j].isHeader)
                    {
                        if (allEntries[j].text.toLowerCase().contains (lc))
                            matches.push_back (allEntries[j]);
                        ++j;
                    }
                    if (! matches.empty())
                    {
                        visibleEntries.push_back (allEntries[i]);
                        for (auto& m : matches) visibleEntries.push_back (m);
                    }
                    i = j;
                }
                else
                {
                    ++i;
                }
            }
        }
        clampScroll();
        repaint();
    }

    int getContentHeight() const noexcept
    {
        int total = 6;
        for (auto& e : visibleEntries)
            total += e.isHeader ? kHeaderH : kRowH;
        return total + 6;
    }

    void resized() override { clampScroll(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141418));

        const int w = getWidth();
        const int viewH = getHeight();
        const auto mouse = getMouseXYRelative();
        const bool mouseInside = getLocalBounds().contains (mouse);

        int y = 6 - scrollOffset;
        for (size_t i = 0; i < visibleEntries.size(); ++i)
        {
            const auto& e = visibleEntries[i];
            const int h = e.isHeader ? kHeaderH : kRowH;
            if (y + h > 0 && y < viewH)
            {
                juce::Rectangle<int> row (0, y, w - kScrollbarW, h);
                if (e.isHeader)
                {
                    g.setColour (juce::Colour (0xff1f1f26));
                    g.fillRect (row.reduced (4, 2));
                    g.setColour (juce::Colour (0xff9090a0));
                    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                    g.drawText (e.text.toUpperCase(),
                                  row.reduced (12, 0), juce::Justification::centredLeft, false);
                }
                else
                {
                    const bool hovered = mouseInside && row.contains (mouse);
                    if (hovered)
                    {
                        g.setColour (juce::Colour (0xff2a2a36));
                        g.fillRect (row);
                    }
                    g.setColour (juce::Colour (0xffdddde0));
                    g.setFont (juce::Font (juce::FontOptions (12.5f)));
                    g.drawText (e.text,
                                  row.reduced (22, 0), juce::Justification::centredLeft, false);
                }
            }
            y += h;
        }

        if (visibleEntries.empty())
        {
            g.setColour (juce::Colour (0xff70707a));
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText ("No plugins match filter.",
                          getLocalBounds().reduced (12), juce::Justification::centred, false);
        }

        drawScrollbar (g);
    }

    void mouseMove (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent& ev) override
    {
        const int row = rowForY (ev.y);
        if (row < 0 || row >= (int) visibleEntries.size()) return;
        const auto& e = visibleEntries[(size_t) row];
        if (e.isHeader) return;
        if (onPick) onPick (e.desc);
    }

    void mouseWheelMove (const juce::MouseEvent&,
                          const juce::MouseWheelDetails& w) override
    {
        const int delta = (int) (-w.deltaY * 80.0f);
        scrollOffset += delta;
        clampScroll();
        repaint();
    }

private:
    struct Entry
    {
        bool isHeader;
        juce::String text;
        juce::PluginDescription desc;
    };

    static constexpr int kRowH       = 22;
    static constexpr int kHeaderH    = 22;
    static constexpr int kScrollbarW = 8;

    int rowForY (int y) const noexcept
    {
        int curY = 6 - scrollOffset;
        for (size_t i = 0; i < visibleEntries.size(); ++i)
        {
            const int h = visibleEntries[i].isHeader ? kHeaderH : kRowH;
            if (y >= curY && y < curY + h) return (int) i;
            curY += h;
        }
        return -1;
    }

    void clampScroll() noexcept
    {
        const int maxOffset = std::max (0, getContentHeight() - getHeight());
        scrollOffset = std::clamp (scrollOffset, 0, maxOffset);
    }

    void drawScrollbar (juce::Graphics& g)
    {
        const int contentH = getContentHeight();
        const int viewH    = getHeight();
        if (contentH <= viewH) return;

        const float ratio  = (float) viewH / (float) contentH;
        const int thumbH   = std::max (20, (int) (viewH * ratio));
        const int trackH   = viewH;
        const int maxOff   = contentH - viewH;
        const float prog   = maxOff > 0 ? (float) scrollOffset / (float) maxOff : 0.0f;
        const int thumbY   = (int) ((trackH - thumbH) * prog);
        const int x        = getWidth() - kScrollbarW;

        g.setColour (juce::Colour (0xff202028));
        g.fillRect (x, 0, kScrollbarW, viewH);
        g.setColour (juce::Colour (0xff5a5a68));
        g.fillRect (x + 1, thumbY, kScrollbarW - 2, thumbH);
    }

    juce::Array<juce::PluginDescription> rawDescs;
    Group group = Group::Manufacturer;
    juce::String currentFilter;
    std::vector<Entry> allEntries;
    std::vector<Entry> visibleEntries;
    std::function<void (const juce::PluginDescription&)> onPick;
    int scrollOffset = 0;
};

PluginPickerPanel::PluginPickerPanel (juce::Array<juce::PluginDescription> descriptions,
                                        Kind kind, Callbacks cb)
    : kind_ (kind), callbacks_ (std::move (cb))
{
    setOpaque (true);

    titleLabel.setText (kind_ == Kind::Instruments ? "Insert instrument" : "Insert effect",
                          juce::dontSendNotification);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

    filterEditor.setPopupMenuEnabled (false);   // XWayland popup flash (see DuskComboBox)
    filterEditor.setTextToShowWhenEmpty ("Filter...", juce::Colour (0xff707078));
    filterEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14141a));
    filterEditor.setColour (juce::TextEditor::textColourId,        juce::Colour (0xffe0e0e4));
    filterEditor.setColour (juce::TextEditor::outlineColourId,     juce::Colour (0xff34343c));
    filterEditor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff6a5aa0));
    filterEditor.onTextChange = [this]
    {
        if (listBody == nullptr) return;
        listBody->applyFilter (filterEditor.getText());
    };
    addAndMakeVisible (filterEditor);

    auto styleBtn = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff262630));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5a4880));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd0d0d4));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    };
    styleBtn (cancelBtn);
    styleBtn (scanBtn);
    styleBtn (browseBtn);
    styleBtn (hwInsertBtn);
    styleBtn (soundfontBtn);
    styleBtn (groupBtn);

    groupBtn.setTooltip ("Toggle plugin list grouping between manufacturer and type.");
    groupBtn.onClick = [this]
    {
        groupByType = ! groupByType;
        groupBtn.setButtonText (groupByType ? "Group: Type" : "Group: Maker");
        if (listBody != nullptr)
            listBody->setGroup (groupByType ? ListBody::Group::Type
                                            : ListBody::Group::Manufacturer);
    };
    addAndMakeVisible (groupBtn);

    cancelBtn.onClick     = [this] { if (callbacks_.onCancel)         callbacks_.onCancel(); };
    scanBtn.onClick       = [this] { if (callbacks_.onScan)           callbacks_.onScan(); };
    browseBtn.onClick     = [this] { if (callbacks_.onBrowseFile)     callbacks_.onBrowseFile(); };
    hwInsertBtn.onClick   = [this] { if (callbacks_.onHardwareInsert) callbacks_.onHardwareInsert(); };
    soundfontBtn.onClick  = [this] { if (callbacks_.onLoadSoundfont)  callbacks_.onLoadSoundfont(); };

    addAndMakeVisible (cancelBtn);
    addAndMakeVisible (scanBtn);
    addAndMakeVisible (browseBtn);
    if (callbacks_.onHardwareInsert) addAndMakeVisible (hwInsertBtn);
    if (callbacks_.onLoadSoundfont)  addAndMakeVisible (soundfontBtn);

    auto pickFn = [this] (const juce::PluginDescription& desc)
    {
        if (callbacks_.onPickPlugin) callbacks_.onPickPlugin (desc);
    };
    listBody = std::make_unique<ListBody> (std::move (descriptions), std::move (pickFn));
    addAndMakeVisible (listBody.get());

    setWantsKeyboardFocus (true);

    // setSize last - all children are now created and addAndMakeVisible'd,
    // so the resized() that setSize fires lays everything out correctly.
    // If a child is added after this line, the next setBounds from
    // EmbeddedModal won't trigger resized() (same size, position-only
    // change), and the new child renders at (0,0,0,0). Add new children
    // ABOVE this line.
    setSize (560, 540);
}

PluginPickerPanel::~PluginPickerPanel() = default;

void PluginPickerPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a22));
}

void PluginPickerPanel::resized()
{
    auto area = getLocalBounds().reduced (kPad);

    auto titleRow = area.removeFromTop (kTitleH);
    titleLabel.setBounds (titleRow);
    area.removeFromTop (6);

    auto filterRow = area.removeFromTop (kFilterH);
    groupBtn.setBounds (filterRow.removeFromRight (110));
    filterRow.removeFromRight (6);
    filterEditor.setBounds (filterRow);
    area.removeFromTop (8);

    auto bottomRow = area.removeFromBottom (kActionsH);
    cancelBtn.setBounds (bottomRow.removeFromRight (90));
    bottomRow.removeFromRight (6);
    scanBtn.setBounds (bottomRow.removeFromRight (130));
    bottomRow.removeFromRight (6);
    browseBtn.setBounds (bottomRow.removeFromRight (130));
    if (callbacks_.onHardwareInsert)
    {
        bottomRow.removeFromRight (6);
        hwInsertBtn.setBounds (bottomRow.removeFromRight (140));
    }
    if (callbacks_.onLoadSoundfont)
    {
        bottomRow.removeFromRight (6);
        soundfontBtn.setBounds (bottomRow.removeFromRight (140));
    }
    area.removeFromBottom (8);

    if (listBody != nullptr)
        listBody->setBounds (area);
}

bool PluginPickerPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (callbacks_.onCancel) callbacks_.onCancel();
        return true;
    }
    return false;
}

} // namespace duskstudio
