#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace duskstudio
{
// In-window keyboard-shortcut reference. Opened from the Settings menu or the
// '?' key (shown via EmbeddedModal). Static content — the bindings live in
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

        sections = {
            { "Stages", {
                { "1", "Recording" }, { "2", "Mixing" },
                { "3", "Mastering" }, { "4", "Aux" } } },
            { "Channel banks", {
                { mod ('1'), "Bank 1" }, { mod ('2'), "Bank 2" }, { mod ('3'), "Bank 3" } } },
            { "Transport", {
                { "Space", "Play / Stop" }, { "R", "Record" },
                { "Home", "Playhead to start" }, { ".", "Stop + rewind to start" } } },
            { "Markers & loop", {
                { "M", "Drop marker" }, { "[", "Set loop / punch in" },
                { "]", "Set loop / punch out" }, { "L", "Toggle loop" },
                { "P", "Toggle punch" } } },
            { "Selected track", {
                { "A", "Arm" }, { "S", "Solo" }, { "X", "Mute" } } },
            { "Tools & view", {
                { "G", "Grab / move edit mode" }, { "C", "Metronome on / off" },
                { "K", "Virtual MIDI keyboard" }, { mod ('\\'), "Show / hide timeline" },
                { "F11", "Fullscreen" }, { alt + "T", "Cycle take (Shift = back)" },
                { "?", "This shortcut list" } } },
            { "Zoom", {
                { "-", "Zoom out" }, { "=", "Zoom in" }, { mod ('0'), "Zoom to fit" } } },
            { "File", {
                { mod ('N'), "New session" }, { mod ('O'), "Open" }, { mod ('S'), "Save" },
                { mod ('S', juce::ModifierKeys::shiftModifier), "Save as" },
                { mod ('I'), "Import audio" }, { mod ('B'), "Bounce" }, { mod ('Q'), "Quit" } } },
        };
        setSize (560, 660);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141418));
        g.setColour (juce::Colour (0xff2a2a32));
        g.drawRect (getLocalBounds(), 1);

        auto area = getLocalBounds().reduced (18, 14);

        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        g.drawText ("Keyboard Shortcuts", area.removeFromTop (26),
                     juce::Justification::centredLeft, false);
        area.removeFromTop (8);

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

        auto drawSections = [this, &g] (juce::Rectangle<int> col, size_t from, size_t to)
        {
            for (size_t i = from; i < to && i < sections.size(); ++i)
            {
                const auto& s = sections[i];
                g.setColour (juce::Colour (0xff8a9ad0));
                g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
                g.drawText (s.title.toUpperCase(), col.removeFromTop (20),
                             juce::Justification::bottomLeft, false);
                for (const auto& r : s.rows)
                {
                    auto row = col.removeFromTop (19);
                    g.setColour (juce::Colour (0xff20222a));
                    auto keyBox = row.removeFromLeft (70);
                    g.fillRoundedRectangle (keyBox.reduced (1, 1).toFloat(), 3.0f);
                    g.setColour (juce::Colour (0xffe0e0e0));
                    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                    g.drawText (r.keys, keyBox.reduced (4, 0), juce::Justification::centred, false);
                    g.setColour (juce::Colour (0xffb0b0b8));
                    g.setFont (juce::Font (juce::FontOptions (12.0f)));
                    g.drawText (r.action, row.withTrimmedLeft (8),
                                 juce::Justification::centredLeft, false);
                }
                col.removeFromTop (8);
            }
        };
        drawSections (colL, 0, splitAt);
        drawSections (colR, splitAt, sections.size());
    }

private:
    struct Row { juce::String keys, action; };
    struct Section { juce::String title; std::vector<Row> rows; };
    std::vector<Section> sections;
};
} // namespace duskstudio
