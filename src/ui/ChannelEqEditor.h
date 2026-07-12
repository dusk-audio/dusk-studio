#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace duskstudio
{
// Overlay editor for a channel's 4-band EQ. Constructed each time the user
// clicks the strip's "EQ" button - controls bind to the same atomics on the
// Track, so values persist across open/close. Hosted in an EmbeddedModal.
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
    // EQ section ON/OFF toggle - mirrors the strip's EQ-header LED so
    // the popup can engage or bypass the whole 4-band + HPF/LPF stack
    // without closing the editor.
    juce::TextButton enableButton { "EQ" };

    struct BandRow
    {
        juce::Label nameLabel;
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
        std::unique_ptr<juce::Slider> q;     // bell bands only (HM, LM); null for shelves
        juce::Label qLabel;                  // "Q" caption, only used when q != null
    };
    std::array<BandRow, 4> rows;

    // HPF + LPF row at the top of the popup, mirroring the strip's
    // SSL 9000 J white-filter top section.
    juce::Label hpfLabel, lpfLabel;
    juce::Slider hpfKnob, lpfKnob;

    void refreshTypeButton();
};
} // namespace duskstudio
