#include "DuskMultisampleEditor.h"
#include "AriaGuiComponent.h"
#include "../DuskFileBrowser.h"
#include "../../engine/multisample/AriaBank.h"
#include "../../engine/multisample/AriaGui.h"
#include "../../engine/multisample/DuskMultisampleProcessor.h"

namespace duskstudio
{
DuskMultisampleEditor::DuskMultisampleEditor (DuskMultisampleProcessor& proc)
    : juce::AudioProcessorEditor (proc), processor (proc)
{
    setSize (520, 260);

    titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xfff0f0f0));
    addAndMakeVisible (titleLabel);

    filePathLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    filePathLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    filePathLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (filePathLabel);

    browseButton.setTooltip ("Open an .sfz, .sf2, or ARIA .bank.xml soundfont.");
    browseButton.onClick = [this] { openFileChooser(); };
    addAndMakeVisible (browseButton);

    reloadButton.setTooltip ("Re-parse the current .sfz from disk - useful when editing the file externally.");
    reloadButton.onClick = [this]
    {
        juce::String err;
        if (! processor.reloadCurrentFile (err))
            filePathLabel.setText ("(" + err + ")", juce::dontSendNotification);
    };
    addAndMakeVisible (reloadButton);

    clearButton.setTooltip ("Unload the current soundfont. Instrument goes silent until a new file is loaded.");
    clearButton.onClick = [this] { clearLoadedFile(); };
    addAndMakeVisible (clearButton);

    auto styleKnob = [] (juce::Slider& s, double minV, double maxV, double init)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
        s.setRange (minV, maxV, 0.01);
        s.setValue (init, juce::dontSendNotification);
    };
    auto& ov = processor.getOverrides();

    styleKnob (volSlider, -60.0, 12.0,
               (double) ov.masterVolDb.load (std::memory_order_relaxed));
    volSlider.setTextValueSuffix (" dB");
    volSlider.onValueChange = [this]
    {
        processor.getOverrides().masterVolDb.store ((float) volSlider.getValue(),
                                                      std::memory_order_relaxed);
    };
    addAndMakeVisible (volSlider);
    volLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (volLabel);

    styleKnob (tuneSlider, -100.0, 100.0,
               (double) ov.masterTuneCents.load (std::memory_order_relaxed));
    tuneSlider.setTextValueSuffix (" c");
    tuneSlider.onValueChange = [this]
    {
        processor.getOverrides().masterTuneCents.store ((float) tuneSlider.getValue(),
                                                          std::memory_order_relaxed);
    };
    addAndMakeVisible (tuneSlider);
    tuneLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (tuneLabel);

    styleKnob (polySlider, 1.0, 256.0,
               (double) ov.polyphony.load (std::memory_order_relaxed));
    polySlider.setNumDecimalPlacesToDisplay (0);
    polySlider.onValueChange = [this]
    {
        // Polyphony goes via setPolyphony (message-thread only) -
        // sfizz_set_num_voices is marked OFF in sfizz.h and can't be
        // invoked while the audio thread is rendering.
        processor.setPolyphony ((int) polySlider.getValue());
    };
    addAndMakeVisible (polySlider);
    polyLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (polyLabel);

    zoneCountLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    zoneCountLabel.setColour (juce::Label::textColourId, juce::Colour (0xff707074));
    addAndMakeVisible (zoneCountLabel);

    sf2PresetLabel.setJustificationType (juce::Justification::centredLeft);
    sf2PresetLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    sf2PresetLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addChildComponent (sf2PresetLabel);
    sf2PresetSelector.setTooltip ("Switch between the SoundFont's presets.");
    sf2PresetSelector.onChange = [this]
    {
        const int idx = sf2PresetSelector.getSelectedId() - 1;   // IDs are 1-based
        if (idx < 0 || idx >= processor.getSf2PresetNames().size()) return;
        startAsyncLoad ({}, idx);
    };
    addChildComponent (sf2PresetSelector);

    ariaProgramLabel.setJustificationType (juce::Justification::centredLeft);
    ariaProgramLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    ariaProgramLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addChildComponent (ariaProgramLabel);
    ariaProgramSelector.setTooltip ("Switch between the bank's programs.");
    ariaProgramSelector.onChange = [this]
    {
        const int idx = ariaProgramSelector.getSelectedId() - 1;
        if (idx < 0 || idx >= (int) ariaProgramFiles.size()) return;
        // loadedFilePath changes → timerCallback rebuilds the skin for
        // the newly selected program on the next tick after the load lands.
        startAsyncLoad (ariaProgramFiles[(size_t) idx]);
    };
    addChildComponent (ariaProgramSelector);

    // 4 Hz refresh - just enough that file-path / zone-count update
    // promptly after a load without flooding paint.
    startTimerHz (4);
    timerCallback();   // initial sync
}

DuskMultisampleEditor::~DuskMultisampleEditor()
{
    // Explicit stopTimer before any member destructs so the 4 Hz
    // poller can't fire one final tick against half-torn-down state.
    // juce::Timer's dtor stops too, but that runs AFTER the derived
    // class's members destruct - too late if a tick is mid-flight.
    stopTimer();
}

void DuskMultisampleEditor::timerCallback()
{
    const auto err  = processor.getLastLoadError();
    const auto path = processor.getLoadedFilePath();
    juce::String display;
    if (err.isNotEmpty())
        display = "(" + err + ")";
    else if (path.isEmpty())
        display = "(no file)";
    else
        display = juce::File (path).getFileName();
    if (filePathLabel.getText() != display)
        filePathLabel.setText (display, juce::dontSendNotification);

    const auto zones = "Regions: " + juce::String (processor.getNumRegions());
    if (zoneCountLabel.getText() != zones)
        zoneCountLabel.setText (zones, juce::dontSendNotification);

    const bool fileLoaded = processor.hasLoadedFile();
    reloadButton.setEnabled (fileLoaded);
    clearButton .setEnabled (fileLoaded);

    // Skin lives or dies with the loaded file path. Rebuild on a path
    // change (load, clear, different file, state restore) OR on a same-
    // path mtime change — Reload after an external edit re-loads the
    // same path, which a path-only check would miss.
    const auto modTime = path.isNotEmpty() ? juce::File (path).getLastModificationTime()
                                            : juce::Time();
    if (path != currentSkinPath || modTime != currentSkinModTime)
    {
        currentSkinPath    = path;
        currentSkinModTime = modTime;
        rebuildSkin();
    }
}

void DuskMultisampleEditor::rebuildSkin()
{
    // Tear down the old skin first so its LookAndFeels detach cleanly
    // before any new children allocate.
    if (ariaSkin != nullptr)
    {
        removeChildComponent (ariaSkin.get());
        ariaSkin.reset();
    }

    ariaProgramFiles.clear();
    int ariaSelectedProgram = -1;

    const auto path = processor.getLoadedFilePath();
    if (path.isNotEmpty())
    {
        const juce::File sfz (path);
        if (auto bank = AriaBank::tryLoadFromSfz (sfz);
            bank.has_value() && bank->selectedIndex >= 0
            && (size_t) bank->selectedIndex < bank->programs.size())
        {
            // Remember the whole program list so the switcher can load
            // any sibling program; preselect the one we just opened.
            ariaProgramSelector.clear (juce::dontSendNotification);
            for (size_t i = 0; i < bank->programs.size(); ++i)
            {
                ariaProgramFiles.push_back (bank->programs[i].sfzFile);
                ariaProgramSelector.addItem (bank->programs[i].name, (int) i + 1);
            }
            ariaSelectedProgram = bank->selectedIndex;

            const auto& prog = bank->programs[(size_t) bank->selectedIndex];
            if (prog.guiFile.existsAsFile())
            {
                if (auto doc = AriaGuiDoc::parse (prog.guiFile); doc.has_value())
                {
                    ariaSkin = std::make_unique<AriaGuiComponent> (processor, std::move (*doc));
                    addAndMakeVisible (*ariaSkin);
                }
            }
        }

        // Stock auto-skin fallback: no ARIA bank GUI, but the .sfz
        // declares <control> image= + label_cc&. Build a synthetic skin
        // (background + auto-laid-out labelled knobs bound to those CCs).
        if (ariaSkin == nullptr)
        {
            const auto img    = processor.getControlImagePath();
            const auto labels = processor.getControlCcLabels();
            if (img.existsAsFile() && ! labels.empty())
            {
                auto doc = AriaGuiDoc::buildAutoSkin (img, labels);
                ariaSkin = std::make_unique<AriaGuiComponent> (processor, std::move (doc));
                addAndMakeVisible (*ariaSkin);
            }
        }
    }

    // 3-knob default visible only when no skin took over.
    const bool showDefaults = (ariaSkin == nullptr);
    volSlider .setVisible (showDefaults);
    volLabel  .setVisible (showDefaults);
    tuneSlider.setVisible (showDefaults);
    tuneLabel .setVisible (showDefaults);
    polySlider.setVisible (showDefaults);
    polyLabel .setVisible (showDefaults);
    zoneCountLabel.setVisible (showDefaults);

    // SF2 program switcher: populate + show when the loaded SF2 has
    // more than one preset. (SF2 never has an ARIA skin, so it always
    // rides in the default layout.)
    const auto& presets = processor.getSf2PresetNames();
    const bool showSf2Presets = showDefaults && presets.size() > 1;
    if (showSf2Presets)
    {
        sf2PresetSelector.clear (juce::dontSendNotification);
        for (int i = 0; i < presets.size(); ++i)
            sf2PresetSelector.addItem (presets[i], i + 1);   // IDs 1-based
        sf2PresetSelector.setSelectedId (juce::jmax (1, processor.getSf2PresetIndex() + 1),
                                          juce::dontSendNotification);
    }
    sf2PresetLabel   .setVisible (showSf2Presets);
    sf2PresetSelector.setVisible (showSf2Presets);

    // ARIA bank program switcher: show when the skin belongs to a
    // multi-program bank. Sits in a thin row above the skin.
    const bool showAriaPrograms = (ariaSkin != nullptr) && ariaProgramFiles.size() > 1;
    if (showAriaPrograms)
        ariaProgramSelector.setSelectedId (ariaSelectedProgram + 1, juce::dontSendNotification);
    ariaProgramLabel   .setVisible (showAriaPrograms);
    ariaProgramSelector.setVisible (showAriaPrograms);

    if (ariaSkin != nullptr)
    {
        const auto natW = ariaSkin->nativeSize().getWidth();
        const auto natH = ariaSkin->nativeSize().getHeight();
        // Header (62) + optional program-switcher row (28) + skin + 12.
        const int progRow = showAriaPrograms ? 28 : 0;
        setSize (juce::jmax (natW + 24, 520), 62 + progRow + natH + 12);
    }
    else
    {
        setSize (520, 260);
    }
    resized();
}

void DuskMultisampleEditor::openFileChooser()
{
    // In-window Dusk-native browser — no standalone window, no
    // Wayland positioning workarounds. SafePointer still guards the
    // async result against tear-down (plugin removed / track torn
    // down while the dialog is open).
    juce::Component::SafePointer<DuskMultisampleEditor> safe (this);
    filebrowser::open (*this, {
        /*title*/                  "Load soundfont (.sfz / .sf2 / .bank.xml)",
        /*initialFileOrDirectory*/ juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        /*filePatternsAllowed*/    "*.sfz;*.sf2;*.bank.xml",
        /*mode*/                   filebrowser::Mode::Open,
        /*warnAboutOverwriting*/   false,
        /*selectDirectories*/      false,
    },
    [safe] (juce::File file)
    {
        auto* self = safe.getComponent();
        if (self == nullptr || ! file.existsAsFile()) return;

        const auto fileName = file.getFileName().toLowerCase();

        // ARIA bank manifest - find the first program's .sfz inside the
        // bank and load that. Phase 4 adds the program switcher; for now
        // pick the first valid AriaProgram so the user can audition the
        // pack without drilling into Programs/ themselves.
        if (fileName.endsWith (".bank.xml"))
        {
            // tryLoadFromSfz walks up from the passed file's directory
            // looking for *.bank.xml - passing the bank file itself
            // works because its parent dir contains it.
            auto bankOpt = AriaBank::tryLoadFromSfz(file);
            if (! bankOpt.has_value() || bankOpt->programs.empty())
            {
                self->filePathLabel.setText ("(bank manifest had no programs)",
                                              juce::dontSendNotification);
                return;
            }
            self->startAsyncLoad (bankOpt->programs.front().sfzFile);
            return;
        }

        // .sf2 and .sfz both go through the background load — an SF2 GM bank
        // extracts every preset sample first (~seconds) and used to wedge the
        // message thread for the duration.
        self->startAsyncLoad (file);
    });
}

void DuskMultisampleEditor::setLoadControlsEnabled (bool enabled)
{
    browseButton       .setEnabled (enabled);
    reloadButton       .setEnabled (enabled);
    clearButton        .setEnabled (enabled);
    sf2PresetSelector  .setEnabled (enabled);
    ariaProgramSelector.setEnabled (enabled);
}

void DuskMultisampleEditor::startAsyncLoad (const juce::File& file, int presetIndex)
{
    if (processor.isLoadPending()) return;

    setLoadControlsEnabled (false);
    filePathLabel.setText ("(loading " + (presetIndex >= 0
                               ? "preset " + juce::String (presetIndex + 1)
                               : file.getFileName()) + " ...)",
                            juce::dontSendNotification);

    juce::Component::SafePointer<DuskMultisampleEditor> safe (this);
    auto onDone = [safe] (bool ok, juce::String err)
    {
        // The processor outlives a closed editor (worker joins in ITS
        // destructor) — only the UI refresh needs the guard.
        if (auto* self = safe.getComponent())
        {
            self->setLoadControlsEnabled (true);
            if (! ok)
                self->filePathLabel.setText ("(" + err + ")", juce::dontSendNotification);
            else
                self->timerCallback();
        }
    };

    if (presetIndex >= 0)
        processor.loadSf2PresetAsync (presetIndex, std::move (onDone));
    else
        processor.loadFileAsync (file, std::move (onDone));
}

void DuskMultisampleEditor::clearLoadedFile()
{
    processor.clearLoadedFile();
    timerCallback();
}

void DuskMultisampleEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a20));
    g.setColour (juce::Colour (0xff2a2a32));
    g.drawRect (getLocalBounds(), 1);
}

void DuskMultisampleEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Header row: title (left) + browse / reload / clear buttons (right).
    auto header = area.removeFromTop (28);
    titleLabel.setBounds (header.removeFromLeft (200));
    const int btnW = 80;
    browseButton.setBounds (header.removeFromLeft (btnW).reduced (2));
    reloadButton.setBounds (header.removeFromLeft (btnW).reduced (2));
    clearButton .setBounds (header.removeFromLeft (btnW).reduced (2));
    area.removeFromTop (4);

    // File path row.
    filePathLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (8);

    if (ariaSkin != nullptr)
    {
        // Optional program-switcher row above the skin.
        if (ariaProgramSelector.isVisible())
        {
            auto progRow = area.removeFromTop (24);
            ariaProgramLabel.setBounds (progRow.removeFromLeft (64));
            ariaProgramSelector.setBounds (progRow.removeFromLeft (juce::jmin (260, progRow.getWidth())));
            area.removeFromTop (4);
        }

        // Centre the ARIA skin in the remaining area at its native
        // pixel size. ARIA GUIs are pixel-fixed (Sforzando convention);
        // scaling would render fuzzy filmstrips.
        const auto nat = ariaSkin->nativeSize();
        const int  cx  = area.getCentreX() - nat.getWidth()  / 2;
        const int  cy  = area.getY();
        ariaSkin->setBounds (cx, cy, nat.getWidth(), nat.getHeight());
        return;
    }

    // SF2 preset switcher row (only visible for multi-preset SF2s).
    if (sf2PresetSelector.isVisible())
    {
        auto presetRow = area.removeFromTop (24);
        sf2PresetLabel.setBounds (presetRow.removeFromLeft (50));
        sf2PresetSelector.setBounds (presetRow);
        area.removeFromTop (8);
    }

    // Knob row: 3 knobs evenly spaced.
    auto knobRow = area.removeFromTop (90);
    const int knobW = knobRow.getWidth() / 3;
    auto layKnob = [&] (juce::Slider& s, juce::Label& l)
    {
        auto col = knobRow.removeFromLeft (knobW);
        l.setBounds (col.removeFromTop (16));
        s.setBounds (col.reduced (8, 0));
    };
    layKnob (volSlider,  volLabel);
    layKnob (tuneSlider, tuneLabel);
    layKnob (polySlider, polyLabel);
    area.removeFromTop (10);

    // Footer: zone count.
    zoneCountLabel.setBounds (area.removeFromBottom (20));
}
} // namespace duskstudio
