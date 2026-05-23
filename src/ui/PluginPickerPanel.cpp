#include "PluginPickerPanel.h"

namespace duskstudio
{

// Self-contained scrollable list. Rolls its own scroll offset instead
// of relying on juce::Viewport — Viewport's internal contentHolder
// sizing under our nested-EmbeddedModal setup left rows invisible on
// first open.
class PluginPickerPanel::ListBody final : public juce::Component
{
public:
    ListBody (juce::Array<juce::PluginDescription> descs,
              std::function<void (const juce::PluginDescription&)> picker)
        : onPick (std::move (picker))
    {
        std::sort (descs.begin(), descs.end(),
            [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
            {
                if (a.manufacturerName != b.manufacturerName)
                    return a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0;
                return a.name.compareIgnoreCase (b.name) < 0;
            });

        juce::String currentManu;
        for (int i = 0; i < descs.size(); ++i)
        {
            const auto& d = descs.getReference (i);
            const auto manu = d.manufacturerName.isEmpty() ? juce::String ("(unknown)")
                                                            : d.manufacturerName;
            if (manu != currentManu)
            {
                allEntries.push_back ({ true, manu, {} });
                currentManu = manu;
            }
            juce::String label = d.name;
            if (d.pluginFormatName.isNotEmpty())
                label += "  (" + d.pluginFormatName + ")";
            allEntries.push_back ({ false, label, d });
        }
        applyFilter ({});
    }

    void applyFilter (const juce::String& needle)
    {
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
        const int maxOffset = juce::jmax (0, getContentHeight() - getHeight());
        scrollOffset = juce::jlimit (0, maxOffset, scrollOffset);
    }

    void drawScrollbar (juce::Graphics& g)
    {
        const int contentH = getContentHeight();
        const int viewH    = getHeight();
        if (contentH <= viewH) return;

        const float ratio  = (float) viewH / (float) contentH;
        const int thumbH   = juce::jmax (20, (int) (viewH * ratio));
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
    setSize (560, 540);

    titleLabel.setText (kind_ == Kind::Instruments ? "Insert instrument" : "Insert effect",
                          juce::dontSendNotification);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    addAndMakeVisible (titleLabel);

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

    // Force layout now. setSize() at the top of the ctor fired resized()
    // while listBody was still null; EmbeddedModal::show() then sets
    // bounds without changing the size, so JUCE skips the second
    // resized() call and listBody never gets its setBounds(). Calling
    // resized() explicitly here positions everything before the modal
    // mounts us.
    resized();
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
