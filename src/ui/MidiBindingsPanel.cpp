#include "MidiBindingsPanel.h"
#include "DuskAlerts.h"
#include "DuskFileBrowser.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"
#include "../session/MidiBindings.h"
#include <algorithm>

namespace duskstudio
{
MidiBindingsPanel::Row::Row()
{
    targetLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0e0e4));
    targetLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (targetLabel);

    sourceLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    sourceLabel.setFont (juce::Font (juce::FontOptions (
        juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    addAndMakeVisible (sourceLabel);

    removeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff4a2828));
    removeButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0a0a0));
    addAndMakeVisible (removeButton);
}

void MidiBindingsPanel::Row::resized()
{
    auto area = getLocalBounds().reduced (4, 2);
    removeButton.setBounds (area.removeFromRight (80));
    area.removeFromRight (8);
    sourceLabel.setBounds  (area.removeFromRight (160));
    area.removeFromRight (8);
    targetLabel.setBounds  (area);
}

MidiBindingsPanel::MidiBindingsPanel (Session& s,
                                       AudioEngine& e,
                                       std::function<void()> onDone)
    : session (s), engine (e), onDoneCallback (std::move (onDone))
{
    setSize (kPanelW, kPanelH);

    headerLabel.setText ("MIDI Bindings", juce::dontSendNotification);
    headerLabel.setJustificationType (juce::Justification::centred);
    headerLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    headerLabel.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    addAndMakeVisible (headerLabel);

    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setColour (juce::Label::textColourId, juce::Colour (0xff707074));
    emptyHint.setFont (juce::Font (juce::FontOptions (12.0f)));
    emptyHint.setText (
        "No MIDI bindings yet. Right-click any fader / knob / button / "
        "transport control to set one up.",
        juce::dontSendNotification);
    addAndMakeVisible (emptyHint);

    rowsViewport.setViewedComponent (&rowsContainer, false);
    rowsViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (rowsViewport);

    exportButton.setTooltip ("Save the current bindings to a .json file "
                              "for sharing across sessions or machines.");
    exportButton.onClick = [this] { exportPreset(); };
    addAndMakeVisible (exportButton);

    importButton.setTooltip ("Replace the current bindings with a saved "
                              "preset. Existing bindings are dropped.");
    importButton.onClick = [this] { importPreset(); };
    addAndMakeVisible (importButton);

    clearAllButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a2a2a));
    clearAllButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0a0a0));
    clearAllButton.onClick = [this] { clearAll(); };
    addAndMakeVisible (clearAllButton);

    doneButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a5a3a));
    doneButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    doneButton.onClick = [this] { if (onDoneCallback) onDoneCallback(); };
    addAndMakeVisible (doneButton);

    rebuildRows();
}

void MidiBindingsPanel::rebuildRows()
{
    rows.clear();

    const auto& binds = session.midiBindings.current();
    rowsContainer.removeAllChildren();
    rows.reserve (binds.size());

    constexpr int kRowH = 28;
    int y = 0;
    for (int i = 0; i < (int) binds.size(); ++i)
    {
        auto row = std::make_unique<Row>();
        row->targetLabel.setText (describeBindingTarget (binds[(size_t) i], &engine),
                                    juce::dontSendNotification);
        row->sourceLabel.setText (describeBindingSource (binds[(size_t) i]),
                                    juce::dontSendNotification);
        row->removeButton.onClick = [this, i] { removeBindingAt (i); };
        row->setBounds (0, y, rowsViewport.getWidth() - 12, kRowH);
        rowsContainer.addAndMakeVisible (row.get());
        rows.push_back (std::move (row));
        y += kRowH;
    }
    rowsContainer.setSize (rowsViewport.getWidth() - 12, y);
    emptyHint.setVisible (binds.empty());
    rowsViewport.setVisible (! binds.empty());
    clearAllButton.setEnabled (! binds.empty());
}

void MidiBindingsPanel::removeBindingAt (int displayIndex)
{
    // Mutate the bindings vector lock-free via AtomicSnapshot. The
    // audio thread sees the new (shorter) vector on its next read.
    session.midiBindings.mutate ([displayIndex] (std::vector<MidiBinding>& binds)
    {
        if (displayIndex >= 0 && displayIndex < (int) binds.size())
            binds.erase (binds.begin() + displayIndex);
    });
    rebuildRows();
}

void MidiBindingsPanel::clearAll()
{
    session.midiBindings.mutate ([] (std::vector<MidiBinding>& binds)
    {
        binds.clear();
    });
    rebuildRows();
}

void MidiBindingsPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a20));
}

void MidiBindingsPanel::resized()
{
    auto bounds = getLocalBounds().reduced (16, 12);
    headerLabel.setBounds (bounds.removeFromTop (28));
    bounds.removeFromTop (8);

    auto footer = bounds.removeFromBottom (32);
    doneButton.setBounds (footer.removeFromRight (100).reduced (2, 4));
    footer.removeFromRight (8);
    clearAllButton.setBounds (footer.removeFromRight (110).reduced (2, 4));
    // Left-aligned preset I/O buttons so the destructive "Clear all"
    // stays visually separated from them on the right.
    exportButton.setBounds (footer.removeFromLeft (100).reduced (2, 4));
    footer.removeFromLeft (8);
    importButton.setBounds (footer.removeFromLeft (100).reduced (2, 4));
    bounds.removeFromBottom (8);

    rowsViewport.setBounds (bounds);
    emptyHint.setBounds (bounds);
    rebuildRows();  // viewport width changed; re-flow rows
}

void MidiBindingsPanel::exportPreset()
{
    // Default to ~/Documents/<dusk-studio-bindings>.json. The user picks the
    // final path; we just seed the dialog with something sensible.
    const auto defaultDir = juce::File::getSpecialLocation (
        juce::File::userDocumentsDirectory);
    juce::Component::SafePointer<MidiBindingsPanel> safe (this);
    filebrowser::open (*this, {
        /*title*/                  "Save MIDI bindings preset",
        /*initialFileOrDirectory*/ defaultDir.getChildFile ("dusk-studio-bindings.json"),
        /*filePatternsAllowed*/    "*.json",
        /*mode*/                   filebrowser::Mode::Save,
        /*warnAboutOverwriting*/   true,
        /*selectDirectories*/      false,
    },
    [safe] (juce::File file)
    {
        auto* self = safe.getComponent();
        if (self == nullptr || file == juce::File()) return;
        const auto json = serializeBindingsPreset (
            self->session.midiBindings.current());
        auto target = file.hasFileExtension ("json")
                        ? file : file.withFileExtension ("json");
        if (! target.replaceWithText (json))
        {
            if (auto* tlw = self->getTopLevelComponent())
                showDuskAlert (*tlw, "Export failed",
                                  "Could not write to " + target.getFullPathName());
        }
    });
}

void MidiBindingsPanel::importPreset()
{
    const auto defaultDir = juce::File::getSpecialLocation (
        juce::File::userDocumentsDirectory);
    juce::Component::SafePointer<MidiBindingsPanel> safe (this);
    filebrowser::open (*this, {
        /*title*/                  "Load MIDI bindings preset",
        /*initialFileOrDirectory*/ defaultDir,
        /*filePatternsAllowed*/    "*.json",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [safe] (juce::File file)
    {
        auto* self = safe.getComponent();
        if (self == nullptr || file == juce::File()) return;
        const auto json = file.loadFileAsString();
        auto parsed = deserializeBindingsPreset (json);
        if (! parsed.has_value())
        {
            if (auto* tlw = self->getTopLevelComponent())
                showDuskAlert (*tlw, "Import failed",
                                  "Could not read bindings from " + file.getFullPathName()
                                      + ". File is missing or malformed.");
            return;
        }
        // An empty parsed vector is a valid "clear all bindings" preset.
        self->session.midiBindings.mutate (
            [&parsed] (std::vector<MidiBinding>& binds)
            {
                binds = *parsed;
            });
        self->rebuildRows();
    });
}
} // namespace duskstudio
