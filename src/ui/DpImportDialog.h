#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/DpImporter.h"
#include <algorithm>
#include <functional>

namespace duskstudio
{
// In-window modal body confirming a DP song-folder import. Shows what the
// parser found (track count, format, stereo pairs, caveats) and two
// experimental opt-ins (mixer recall, timeline reconstruction), each enabled
// only when the parser decoded that data with confidence. Hosted inside an
// EmbeddedModal owned by MainComponent.
class DpImportDialog final : public juce::Component
{
public:
    // onCommit(importMixer, importTimeline) fires on Import; onCancel on
    // Cancel / Esc / click-outside. Both should close the host modal.
    DpImportDialog (const dp::SongScan& scan,
                    int maxTracks,
                    std::function<void (bool importMixer, bool importTimeline)> onCommit,
                    std::function<void()> onCancel)
        : onCommitFn (std::move (onCommit)), onCancelFn (std::move (onCancel))
    {
        const int shown = std::min ((int) scan.tracks.size(), maxTracks);

        title.setText ("Import DP Song", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
        title.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (title);

        // Em-dash and middle dots are UTF-8 multibyte - route through
        // CharPointer_UTF8 (operator<< on a char* reads bytes as single
        // characters and renders mojibake, GH issue #27).
        const juce::String dot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
        juce::String s;
        s << shown << (shown == 1 ? " track" : " tracks");
        if ((int) scan.tracks.size() > maxTracks)
            s << " (of " << (int) scan.tracks.size()
              << juce::String (juce::CharPointer_UTF8 (" found \xe2\x80\x94 "))
              << maxTracks << "-track limit)";
        if (scan.stereoPairs > 0)
            s << dot << scan.stereoPairs
              << (scan.stereoPairs == 1 ? " stereo pair" : " stereo pairs");
        if (scan.sampleRate > 0.0)
            s << dot << juce::String (scan.sampleRate / 1000.0, 1)
              << " kHz / " << scan.bitDepth << "-bit";
        summary.setText (s, juce::dontSendNotification);
        summary.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b8));
        addAndMakeVisible (summary);

        warnings.setMultiLine (true);
        warnings.setReadOnly (true);
        warnings.setCaretVisible (false);
        warnings.setScrollbarsShown (true);
        warnings.setPopupMenuEnabled (false);   // XWayland popup flash (see DuskComboBox)
        warnings.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14141a));
        warnings.setColour (juce::TextEditor::textColourId, juce::Colour (0xffd0a060));
        warnings.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff35404a));
        warnings.setFont (juce::Font (juce::FontOptions (13.0f)));
        warnings.setText (scan.warnings.empty()
                              ? juce::String ("Ready to import.")
                              : juce::String (scan.warnings),
                          juce::dontSendNotification);
        addAndMakeVisible (warnings);

        mixerToggle.setButtonText ("Import mixer settings (experimental)");
        mixerToggle.setEnabled (scan.mixerDecoded);
        mixerToggle.setColour (juce::ToggleButton::textColourId,
                               scan.mixerDecoded ? juce::Colour (0xffe0e0e4)
                                                 : juce::Colour (0xff606068));
        addAndMakeVisible (mixerToggle);

        const bool canPlace = scan.timelineDecoded || scan.hasMixdown;
        timelineToggle.setButtonText (scan.timelineDecoded
                                          ? "Reconstruct timeline positions (from song.sys)"
                                          : scan.hasMixdown
                                                ? "Align regions to mixdown (experimental)"
                                                : "Reconstruct timeline positions (none stored)");
        timelineToggle.setToggleState (canPlace, juce::dontSendNotification);
        timelineToggle.setEnabled (canPlace);
        timelineToggle.setColour (juce::ToggleButton::textColourId,
                                  canPlace ? juce::Colour (0xffe0e0e4)
                                           : juce::Colour (0xff606068));
        addAndMakeVisible (timelineToggle);

        cancelButton.onClick = [this] { if (onCancelFn) onCancelFn(); };
        addAndMakeVisible (cancelButton);

        importButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a5a3a));
        importButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        importButton.setEnabled (scan.ok && shown > 0);
        importButton.onClick = [this]
        {
            if (onCommitFn)
                onCommitFn (mixerToggle.isEnabled() && mixerToggle.getToggleState(),
                            timelineToggle.isEnabled() && timelineToggle.getToggleState());
        };
        addAndMakeVisible (importButton);

        setSize (520, 420);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1a1a20));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        title.setBounds (r.removeFromTop (28));
        r.removeFromTop (4);
        summary.setBounds (r.removeFromTop (22));
        r.removeFromTop (10);

        auto footer = r.removeFromBottom (40);
        importButton.setBounds (footer.removeFromRight (120));
        footer.removeFromRight (8);
        cancelButton.setBounds (footer.removeFromRight (120));
        r.removeFromBottom (10);

        timelineToggle.setBounds (r.removeFromBottom (28));
        mixerToggle.setBounds (r.removeFromBottom (28));
        r.removeFromBottom (10);

        warnings.setBounds (r);
    }

private:
    std::function<void (bool, bool)> onCommitFn;
    std::function<void()>            onCancelFn;

    juce::Label       title, summary;
    juce::TextEditor  warnings;
    juce::ToggleButton mixerToggle, timelineToggle;
    juce::TextButton  cancelButton { "Cancel" };
    juce::TextButton  importButton { "Import" };
};
} // namespace duskstudio
