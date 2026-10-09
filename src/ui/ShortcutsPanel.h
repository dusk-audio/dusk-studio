#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <vector>

namespace duskstudio
{
// In-window keyboard-shortcut reference. Opened from the Settings menu or the
// '?' key (shown via EmbeddedModal). Static content - the bindings live in
// MainComponent::keyPressed; this just makes them discoverable.
class ShortcutsPanel final : public juce::Component
{
public:
    ShortcutsPanel()
    {
        // Platform-correct command-key prefix (⌘ on macOS, "Ctrl+" elsewhere).
        const auto mod = [] (int keyCode, int extraMods = 0)
        {
            return juce::KeyPress (keyCode,
                                     juce::ModifierKeys (juce::ModifierKeys::commandModifier | extraMods), 0)
                       .getTextDescriptionWithIcons();
        };
        const juce::String alt =
           #if JUCE_MAC
            juce::String (juce::CharPointer_UTF8 ("⌥"));   // ⌥
           #else
            "Alt+";
           #endif
        const auto utf8 = [] (const char* text) { return juce::String::fromUTF8 (text); };

        sections = {
            { "Stages", {
                { mod ('1'), "Recording" }, { mod ('2'), "Mixing" },
                { mod ('3'), "Mastering" }, { mod ('4'), "Aux" } } },
            { "Channel pages", {
                { "1", "Page 1" }, { "2", "Page 2" }, { "3", "Page 3" }, { "4", "Page 4" },
                { "5", "Page 5" }, { "6", "Page 6" }, { "7", "Page 7" }, { "8", "Page 8" } } },
            { "Transport", {
                { "Space", "Play / Stop" }, { "R", "Record" },
                { "Home", "Playhead to start" }, { ".", "Stop + rewind to start" },
                { utf8 ("Shift+\xe2\x86\x90/\xe2\x86\x92"), "Prev / next marker" },
                { "B", "Tap tempo" }, { "Shift+C", "Count-in on / off" },
                { "F", "Time / bars display" } } },
            { "Markers & loop", {
                { "M", "Drop marker" }, { "[", "Set loop / punch in" },
                { "]", "Set loop / punch out" }, { "L", "Toggle loop" },
                { "P", "Toggle punch" } } },
            { "Zoom", {
                { "-", "Zoom out" }, { "=", "Zoom in" }, { "0", "Zoom to fit" } } },
            { "Selected track", {
                { utf8 ("\xe2\x86\x90 / \xe2\x86\x92"), "Focus prev / next strip" },
                { "A", "Arm" }, { "S", "Solo" }, { "X", "Mute" } } },
            { "Tools & view", {
                { "G", "Grab / move edit mode" }, { "C", "Metronome on / off" },
                { "K", "Virtual MIDI keyboard" }, { "T", "Show / hide timeline" },
                { "U", "Tuner" }, { "Shift+M", "Time signature" },
                { mod ('E'), "Split region at playhead / cursor" },
                { "F11", "Fullscreen" }, { alt + "T", "Cycle MIDI take (Shift = back)" },
                { "?", "This shortcut list" } } },
            { "Audio editor", {
                { "G / R / C", "Grab / Range / Cut tool" },
                { utf8 ("\xe2\x86\x91 / \xe2\x86\x93"), "Take above / below" },
                { "T", "Solo take" },
                { utf8 ("\xe2\x86\x90 / \xe2\x86\x92"), "Nudge a beat (Shift = bar)" },
                { "[ / ]", "Loop in / out (Shift = punch)" },
                { "F", "Fade in over range (Shift = out)" },
                { "Delete", "Delete regions or range" }, { "0", "Zoom to fit" } } },
            { "File", {
                { mod ('N'), "New session" }, { mod ('O'), "Open" }, { mod ('S'), "Save" },
                { mod ('S', juce::ModifierKeys::shiftModifier), "Save as" },
                { mod ('I'), "Import Audio or MIDI" }, { mod ('B'), "Bounce" }, { mod ('Q'), "Quit" } } },
        };
        setSize (560, 734);
    }

    // Where paint puts each section title and each row, and the row height it
    // settled on. A window shorter than the list squeezes the rows instead of
    // dropping the last ones off the bottom.
    struct Layout
    {
        using Box = juce::Rectangle<int>;
        int rowH = kRowH;
        std::vector<Box> titles, rows;
    };

    Layout layout() const
    {
        auto area = getLocalBounds().reduced (18, 14);
        area.removeFromTop (kHeadingH + kHeadingGap);

        // Two balanced columns.
        const int gap = 16;
        auto colL = area.removeFromLeft ((area.getWidth() - gap) / 2);
        area.removeFromLeft (gap);
        auto colR = area;

        // Split sections across the two columns by row count so they fill evenly.
        int total = 0;
        for (const auto& s : sections) total += (int) s.rows.size() + 2;
        int half = total / 2, run = 0; size_t splitAt = sections.size();
        for (size_t i = 0; i < sections.size(); ++i)
        {
            run += (int) sections[i].rows.size() + 2;
            if (run >= half) { splitAt = i + 1; break; }
        }

        const auto rowsHeight = [this] (size_t from, size_t to, int rowH)
        {
            int h = 0;
            for (size_t i = from; i < to; ++i)
                h += kTitleH + (int) sections[i].rows.size() * rowH + kSectionGap;
            return h - kSectionGap;
        };
        Layout out;
        while (out.rowH > kMinRowH
               && std::max (rowsHeight (0, splitAt, out.rowH), rowsHeight (splitAt, sections.size(), out.rowH))
                      > colL.getHeight())
            --out.rowH;

        const auto place = [this, &out] (auto col, size_t from, size_t to)
        {
            for (size_t i = from; i < to && i < sections.size(); ++i)
            {
                out.titles.push_back (col.removeFromTop (kTitleH));
                for (size_t r = 0; r < sections[i].rows.size(); ++r)
                    out.rows.push_back (col.removeFromTop (out.rowH));
                col.removeFromTop (kSectionGap);
            }
        };
        place (colL, 0, splitAt);
        place (colR, splitAt, sections.size());
        return out;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141418));
        g.setColour (juce::Colour (0xff2a2a32));
        g.drawRect (getLocalBounds(), 1);

        auto area = getLocalBounds().reduced (18, 14);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        g.drawText ("Keyboard Shortcuts", area.removeFromTop (kHeadingH),
                     juce::Justification::centredLeft, false);

        const auto placed = layout();
        const float shrink = (float) (kRowH - placed.rowH);
        auto row = placed.rows.begin();
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const auto& s = sections[i];
            g.setColour (juce::Colour (0xff8a9ad0));
            g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            g.drawText (s.title.toUpperCase(), placed.titles[i], juce::Justification::bottomLeft, false);
            for (const auto& r : s.rows)
            {
                auto box = *row++;
                g.setColour (juce::Colour (0xff20222a));
                auto keyBox = box.removeFromLeft (70);
                g.fillRoundedRectangle (keyBox.reduced (1, 1).toFloat(), 3.0f);
                g.setColour (juce::Colour (0xffe0e0e0));
                g.setFont (juce::Font (juce::FontOptions (11.0f - shrink * 0.5f, juce::Font::bold)));
                g.drawText (r.keys, keyBox.reduced (4, 0), juce::Justification::centred, false);
                g.setColour (juce::Colour (0xffb0b0b8));
                g.setFont (juce::Font (juce::FontOptions (12.0f - shrink * 0.5f)));
                g.drawText (r.action, box.withTrimmedLeft (8),
                             juce::Justification::centredLeft, false);
            }
        }
    }

    static constexpr int kRowH = 19;
    static constexpr int kMinRowH = 14;

private:
    static constexpr int kTitleH = 20;
    static constexpr int kSectionGap = 8;
    static constexpr int kHeadingH = 26;
    static constexpr int kHeadingGap = 8;

    struct Row { juce::String keys, action; };
    struct Section { juce::String title; std::vector<Row> rows; };
    std::vector<Section> sections;
};
} // namespace duskstudio
