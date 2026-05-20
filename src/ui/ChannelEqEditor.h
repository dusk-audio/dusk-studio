#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace duskstudio
{
// Overlay editor for a channel's 4-band EQ. Constructed each time the user
// clicks the strip's "EQ" button - controls bind to the same atomics on the
// Track, so values persist across open/close. Designed for juce::CallOutBox.
class ChannelEqEditor final : public juce::Component
{
public:
    explicit ChannelEqEditor (Track& trackRef);
    ~ChannelEqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Track& track;

    juce::Label titleLabel;
    juce::TextButton typeButton { "E" };

    struct BandRow
    {
        juce::Label nameLabel;
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
        std::unique_ptr<juce::Slider> q;     // bell bands only (HM, LM); null for shelves
        juce::Label qLabel;                  // "Q" caption, only used when q != null
    };
    std::array<BandRow, 4> rows;

    // HPF row at the top of the popup, mirroring the strip's HPF control.
    juce::Label hpfLabel;
    juce::Slider hpfKnob;

    void refreshTypeButton();
};
} // namespace duskstudio
