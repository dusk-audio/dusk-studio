#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>

namespace duskstudio
{
// Session notepad: a multi-line markdown editor for lyrics and notes, hosted
// in an EmbeddedModal off the transport bar. The text IS markdown - the
// toolbar buttons only insert or wrap markdown syntax around the caret or
// selection, so what the user types is exactly what lands in the session's
// notepad.md sidecar.
class NotepadPanel final : public juce::Component
{
public:
    std::function<void()> onCloseRequested;
    std::function<void (const juce::String&)> onTextChanged;

    NotepadPanel()
    {
        setOpaque (true);

        editor.setMultiLine (true);
        editor.setReturnKeyStartsNewLine (true);
        editor.setTabKeyUsedAsCharacter (true);
        editor.setScrollbarsShown (true);
        editor.setPopupMenuEnabled (false);   // XWayland popup flash (see DuskComboBox)
        editor.setColour (juce::TextEditor::backgroundColourId,     juce::Colour (0xff14141a));
        editor.setColour (juce::TextEditor::textColourId,           juce::Colour (0xffe0e0e4));
        editor.setColour (juce::TextEditor::outlineColourId,        juce::Colour (0xff34343c));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff6a5aa0));
        editor.setFont (juce::Font (juce::FontOptions (15.0f)));
        editor.onTextChange = [this] { if (onTextChanged) onTextChanged (editor.getText()); };
        addAndMakeVisible (editor);

        struct Tool { const char* label; const char* tip;
                      void (NotepadPanel::* apply)(); };
        static constexpr Tool tools[] = {
            { "H",  "Heading",       &NotepadPanel::applyHeading   },
            { "B",  "Bold",          &NotepadPanel::applyBold      },
            { "I",  "Italic",        &NotepadPanel::applyItalic    },
            { ">",  "Quote",         &NotepadPanel::applyQuote     },
            { "<>", "Code",          &NotepadPanel::applyCode      },
            { "[]", "Link",          &NotepadPanel::applyLink      },
            { "-",  "Bullet list",   &NotepadPanel::applyBullets   },
            { "1.", "Numbered list", &NotepadPanel::applyNumbers   },
            { "[x]","Task list",     &NotepadPanel::applyTasks     },
        };
        for (const auto& t : tools)
        {
            auto btn = std::make_unique<juce::TextButton> (t.label);
            btn->setTooltip (juce::String (t.tip) + " (markdown)");
            btn->setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff24242c));
            btn->setColour (juce::TextButton::textColourOffId, juce::Colour (0xffc0c0c8));
            btn->onClick = [this, apply = t.apply]
            {
                (this->*apply)();
                editor.grabKeyboardFocus();
            };
            addAndMakeVisible (*btn);
            toolButtons.push_back (std::move (btn));
        }

        closeBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff305a82));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onCloseRequested) onCloseRequested(); };
        addAndMakeVisible (closeBtn);

        setSize (560, 480);
    }

    void setText (const juce::String& text)
    {
        editor.setText (text, juce::dontSendNotification);
    }

    juce::String getText() const { return editor.getText(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a22));
        g.setColour (juce::Colour (0xff3a3a44));
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);

        auto r = getLocalBounds().reduced (14);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText ("Notepad", r.removeFromTop (22), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        r.removeFromTop (26);

        auto toolbar = r.removeFromTop (26);
        for (auto& b : toolButtons)
        {
            b->setBounds (toolbar.removeFromLeft (34));
            toolbar.removeFromLeft (4);
        }

        auto footer = r.removeFromBottom (34);
        closeBtn.setBounds (footer.removeFromBottom (28).withSizeKeepingCentre (100, 28));

        r.removeFromTop (6);
        r.removeFromBottom (6);
        editor.setBounds (r);
    }

    void visibilityChanged() override
    {
        if (isShowing())
            editor.grabKeyboardFocus();
    }

private:
    // Wraps the selection in prefix/suffix, or inserts the pair and parks the
    // caret between them when nothing is selected.
    void wrapSelection (const juce::String& prefix, const juce::String& suffix)
    {
        const auto sel = editor.getHighlightedRegion();
        const auto text = editor.getTextInRange (sel);
        editor.insertTextAtCaret (prefix + text + suffix);
        if (text.isEmpty())
            editor.setCaretPosition (editor.getCaretPosition() - suffix.length());
    }

    // Prepends `prefix` to every line the selection (or caret) touches.
    // numbered=true emits "1. ", "2. ", ... instead of the fixed prefix.
    void prefixSelectedLines (const juce::String& prefix, bool numbered = false)
    {
        const auto all = editor.getText();
        const auto sel = editor.getHighlightedRegion();
        const int caret = sel.isEmpty() ? editor.getCaretPosition() : sel.getStart();

        int lineStart = all.substring (0, caret).lastIndexOfChar ('\n');
        lineStart = lineStart < 0 ? 0 : lineStart + 1;
        int end = sel.isEmpty() ? caret : sel.getEnd();

        auto block = all.substring (lineStart, end);
        // A selection ending right after a newline must keep that newline in the
        // replacement, otherwise the prefixed block is glued to the line below.
        const bool trailingNewline = block.endsWithChar ('\n');
        juce::StringArray lines;
        lines.addLines (block);
        if (trailingNewline && ! lines.isEmpty() && lines[lines.size() - 1].isEmpty())
            lines.remove (lines.size() - 1);
        if (lines.isEmpty()) lines.add ({});
        juce::StringArray out;
        for (int i = 0; i < lines.size(); ++i)
            out.add ((numbered ? juce::String (i + 1) + ". " : prefix) + lines[i]);

        editor.setHighlightedRegion ({ lineStart, end });
        editor.insertTextAtCaret (out.joinIntoString ("\n")
                                  + (trailingNewline ? "\n" : ""));
    }

    void applyHeading() { prefixSelectedLines ("# "); }
    void applyBold()    { wrapSelection ("**", "**"); }
    void applyItalic()  { wrapSelection ("*", "*"); }
    void applyQuote()   { prefixSelectedLines ("> "); }
    void applyCode()
    {
        if (editor.getTextInRange (editor.getHighlightedRegion()).containsChar ('\n'))
            wrapSelection ("```\n", "\n```");
        else
            wrapSelection ("`", "`");
    }
    void applyLink()
    {
        const auto sel = editor.getTextInRange (editor.getHighlightedRegion());
        if (sel.isNotEmpty())
        {
            editor.insertTextAtCaret ("[" + sel + "](url)");
            editor.setHighlightedRegion ({ editor.getCaretPosition() - 4,
                                           editor.getCaretPosition() - 1 });
        }
        else
        {
            wrapSelection ("[", "](url)");
        }
    }
    void applyBullets() { prefixSelectedLines ("- "); }
    void applyNumbers() { prefixSelectedLines ({}, true); }
    void applyTasks()   { prefixSelectedLines ("- [ ] "); }

    juce::TextEditor editor;
    std::vector<std::unique_ptr<juce::TextButton>> toolButtons;
    juce::TextButton closeBtn { "Close" };
};
} // namespace duskstudio
