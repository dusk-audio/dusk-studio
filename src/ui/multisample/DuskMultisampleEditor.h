#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../DuskComboBox.h"

#include <memory>

namespace duskstudio
{
class AriaGuiComponent;
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

    // SF2 program switcher: visible only when an .sf2 with >1 preset is
    // loaded (SF2 has no ARIA skin, so it sits in the default layout).
    juce::Label    sf2PresetLabel   { {}, "Preset" };
    DuskComboBox   sf2PresetSelector;

    // ARIA bank program switcher: visible when the loaded .sfz belongs
    // to a multi-program ARIA bank (e.g. Swirly's 8 kits). Sits in a
    // thin row above the rendered ARIA skin.
    juce::Label              ariaProgramLabel { {}, "Program" };
    DuskComboBox             ariaProgramSelector;
    std::vector<juce::File>  ariaProgramFiles;   // parallel to selector items

    std::unique_ptr<juce::FileChooser> fileChooser;

    // Skin = the per-soundfont custom UI (when the loaded .sfz ships
    // an ARIA bank.xml + GUI XML). nullptr = fall back to the
    // 3-knob default. Rebuilt whenever the loaded file path changes OR
    // the same file's on-disk mtime changes (Reload after an external
    // edit re-loads the same path, so a path-only check would miss it).
    std::unique_ptr<AriaGuiComponent> ariaSkin;
    juce::String                      currentSkinPath;
    juce::Time                        currentSkinModTime;

    void rebuildSkin();
};
} // namespace duskstudio
