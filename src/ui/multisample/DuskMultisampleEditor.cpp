#include "DuskMultisampleEditor.h"
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

    browseButton.setTooltip ("Open an .sfz file. SF2 support lands in Dusk Studio 1.0.");
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
}

void DuskMultisampleEditor::openFileChooser()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Load soundfont (.sfz)",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.sfz;*.sf2");

    const auto flags = juce::FileBrowserComponent::openMode
                       | juce::FileBrowserComponent::canSelectFiles;
    // SafePointer guards against the user dismissing the chooser
    // after this editor has already been destroyed (plugin removed
    // / track torn down). Without it, the async callback would
    // invoke methods on a deallocated `this`.
    juce::Component::SafePointer<DuskMultisampleEditor> safe (this);
    fileChooser->launchAsync (flags, [safe] (const juce::FileChooser& fc)
    {
        auto* self = safe.getComponent();
        if (self == nullptr) return;
        const auto file = fc.getResult();
        if (! file.existsAsFile()) return;
        if (file.getFileExtension().toLowerCase() == ".sf2")
        {
            self->filePathLabel.setText ("(SF2 support lands in 1.0)",
                                          juce::dontSendNotification);
            return;
        }
        juce::String err;
        if (! self->processor.loadSfzFile (file, err))
            self->filePathLabel.setText ("(" + err + ")", juce::dontSendNotification);
        else
            self->timerCallback();
    });
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
