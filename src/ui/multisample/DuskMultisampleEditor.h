#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
class DuskMultisampleProcessor;

// Phase 1 editor for the native Multisample instrument. Minimal
// surface: shows the loaded .sfz path, master volume / tune /
// polyphony knobs that drive the processor's override atoms, and
// a read-only region count. Reload + Clear + Browse buttons let
// the user swap the loaded soundfont without going back to the
// plugin picker. ADSR / filter / LFO controls + zone mapping
// editor land in Phase 2 / Phase 3 per the plan.
class DuskMultisampleEditor final : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit DuskMultisampleEditor (DuskMultisampleProcessor& proc);
    ~DuskMultisampleEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void openFileChooser();
    void clearLoadedFile();

    DuskMultisampleProcessor& processor;

    juce::Label    titleLabel       { {}, "Soundfont" };
    juce::Label    filePathLabel    { {}, "(no file)" };
    juce::TextButton browseButton   { "Browse..." };
    juce::TextButton reloadButton   { "Reload" };
    juce::TextButton clearButton    { "Clear" };

    juce::Label    volLabel         { {}, "Volume" };
    juce::Slider   volSlider;
    juce::Label    tuneLabel        { {}, "Tune (cents)" };
    juce::Slider   tuneSlider;
    juce::Label    polyLabel        { {}, "Polyphony" };
    juce::Slider   polySlider;

    juce::Label    zoneCountLabel   { {}, "Regions: 0" };

    std::unique_ptr<juce::FileChooser> fileChooser;
};
} // namespace duskstudio
