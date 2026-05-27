#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../engine/multisample/AriaGui.h"

#include <memory>
#include <unordered_map>

namespace duskstudio
{
class DuskMultisampleProcessor;

// Renders an AriaGuiDoc using JUCE. Each AriaWidget becomes a child
// Component:
//   StaticImage    -> cached juce::Image painted to fit
//   StaticText     -> juce::Label
//   Slider/Knob    -> juce::Slider with a filmstrip LookAndFeel
//   CommandButton  -> juce::ImageButton (launch_url currently the only
//                       command wired; rest log + ignore)
//   OptionMenu     -> juce::ComboBox driven by OptionItem children
//   Unknown        -> debug-only labelled rect, draws the original tag
//
// Controls are bound to the processor: Slider/Knob value changes push
// through bindControl -> processor.setHDCC() so they move audio (see the
// onChange handlers in the .cpp).
class AriaGuiComponent : public juce::Component
{
public:
    AriaGuiComponent(DuskMultisampleProcessor& proc, AriaGuiDoc doc);
    ~AriaGuiComponent() override;

    juce::Rectangle<int> nativeSize() const noexcept
    {
        return { doc_.width, doc_.height };
    }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::Image& loadImageCached(const juce::String& relPath);
    void buildChildren();
    // Wire a slider (knob or fader) to CC `cc`: set range 0..1, seed
    // from the processor's current CC value, and push changes back.
    void bindControl(juce::Slider& s, int cc);

    DuskMultisampleProcessor& processor_;
    AriaGuiDoc                doc_;

    std::unordered_map<juce::String, juce::Image> imageCache_;

    // Custom LookAndFeels keyed by the filmstrip image path so multiple
    // knobs sharing the same filmstrip share one LAF instance.
    struct FilmstripKnobLAF;
    struct FilmstripFaderLAF;
    std::unordered_map<juce::String, std::unique_ptr<FilmstripKnobLAF>>  knobLAFs_;
    std::unordered_map<juce::String, std::unique_ptr<FilmstripFaderLAF>> faderLAFs_;

    std::vector<std::unique_ptr<juce::Component>> children_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AriaGuiComponent)
};
} // namespace duskstudio
