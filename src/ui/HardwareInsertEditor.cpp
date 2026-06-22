#include "HardwareInsertEditor.h"
#include "../dsp/OutputPairRouting.h"

#include <memory>

namespace duskstudio
{
using outputpair::encodePair;
using outputpair::decodePairL;
using outputpair::decodePairR;

HardwareInsertEditor::HardwareInsertEditor (HardwareInsertParams& paramsRef,
                                                juce::AudioDeviceManager& dm,
                                                std::function<void()> onDone,
                                                bool embedded)
    : params (paramsRef),
      deviceManager (dm),
      onDoneCallback (std::move (onDone)),
      isEmbedded (embedded)
{
    setSize (kPanelW, kPanelH);

    headerLabel.setText ("I/O", juce::dontSendNotification);
    headerLabel.setJustificationType (juce::Justification::centred);
    headerLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    headerLabel.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    addAndMakeVisible (headerLabel);

    auto setupLabel = [this] (juce::Label& lbl, const juce::String& text,
                                juce::Justification j = juce::Justification::centredRight)
    {
        lbl.setText (text, juce::dontSendNotification);
        lbl.setJustificationType (j);
        lbl.setColour (juce::Label::textColourId, juce::Colour (0xffc0c0c8));
        lbl.setFont (juce::Font (juce::FontOptions (12.0f)));
        addAndMakeVisible (lbl);
    };
    setupLabel (outVolLabel, "Output Volume:");
    setupLabel (outChLabel,  "Output:");
    setupLabel (inChLabel,   "Input:");
    setupLabel (inVolLabel,  "Input Volume:");
    setupLabel (latencyLabel, "Latency Detection:");
    setupLabel (latencySamplesLabel, "Latency Offset:");
    setupLabel (dryWetLabel,  "Dry/Wet:");
    setupLabel (formatLabel,  "Format:");

    auto setupGainSlider = [this] (juce::Slider& s, std::atomic<float>& atom)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
        s.setRange (-24.0, 12.0, 0.1);
        s.setValue ((double) atom.load (std::memory_order_relaxed),
                     juce::dontSendNotification);
        s.setTextValueSuffix (" dB");
        s.setDoubleClickReturnValue (true, 0.0);
        s.onValueChange = [this, &s, &atom]
        {
            atom.store ((float) s.getValue(), std::memory_order_relaxed);
        };
        addAndMakeVisible (s);
    };
    setupGainSlider (outVolSlider, params.outputGainDb);
    setupGainSlider (inVolSlider,  params.inputGainDb);

    auto setupCombo = [this] (juce::ComboBox& cb)
    {
        cb.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202024));
        cb.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffe0e0e4));
        cb.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff404048));
        addAndMakeVisible (cb);
    };
    setupCombo (outChCombo);
    setupCombo (inChCombo);
    outChCombo.onChange = [this] { publishRoutingFromUi(); };
    inChCombo .onChange = [this] { publishRoutingFromUi(); };
    populateDropdowns();

    pingButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a4a6a));
    pingButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    pingButton.setTooltip ("Send a chirp out the SEND, capture the RETURN, "
                            "measure round-trip latency in samples. Make sure "
                            "the outboard is connected.");
    pingButton.onClick = [this]
    {
        // Clear any stale result before triggering. The audio thread
        // writes pingResult only after capture + correlation finish.
        params.pingResult .store (-1,    std::memory_order_relaxed);
        params.pingPending.store (true,  std::memory_order_release);
        pingButton.setButtonText ("Pinging...");
        pingButton.setEnabled (false);
        pingStatusLabel.setText ("Measuring...", juce::dontSendNotification);
        pingStatusLabel.setColour (juce::Label::textColourId,
                                     juce::Colour (0xff909094));
    };
    addAndMakeVisible (pingButton);

    pingStatusLabel.setJustificationType (juce::Justification::centredLeft);
    pingStatusLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    pingStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff909094));
    addAndMakeVisible (pingStatusLabel);

    // Poll the audio-thread result + the strip's measured latency at
    // 10 Hz. Result-snapshot pattern: pingPending flips back to false
    // when the audio thread writes pingResult. UI then surfaces the
    // measured lag (or a failure alert).
    startTimerHz (10);

    latencySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    latencySlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
    // Range matches HardwareInsertSlot::kMaxDelaySamples so a measured
    // ping result is always representable without truncation. The audio
    // thread already caps the effective delay to the same value.
    latencySlider.setRange (0.0, 16384.0, 1.0);
    latencySlider.setTextValueSuffix (" sam");
    latencySlider.setDoubleClickReturnValue (true, 0.0);
    {
        const auto routing = currentRouting();
        latencySlider.setValue ((double) routing.latencySamples,
                                  juce::dontSendNotification);
    }
    latencySlider.onValueChange = [this] { publishRoutingFromUi(); };
    addAndMakeVisible (latencySlider);

    dryWetSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    dryWetSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 70, 22);
    dryWetSlider.setRange (0.0, 100.0, 0.1);
    dryWetSlider.setTextValueSuffix (" %");
    dryWetSlider.setValue ((double) (params.dryWet.load (std::memory_order_relaxed) * 100.0f),
                            juce::dontSendNotification);
    dryWetSlider.onValueChange = [this]
    {
        params.dryWet.store ((float) (dryWetSlider.getValue() / 100.0),
                              std::memory_order_relaxed);
    };
    addAndMakeVisible (dryWetSlider);

    formatStereoButton.setRadioGroupId (8001);
    formatMidSideButton.setRadioGroupId (8001);
    {
        const auto routing = currentRouting();
        formatStereoButton .setToggleState (routing.format == 0, juce::dontSendNotification);
        formatMidSideButton.setToggleState (routing.format == 1, juce::dontSendNotification);
    }
    formatStereoButton .onClick = [this] { publishRoutingFromUi(); };
    formatMidSideButton.onClick = [this] { publishRoutingFromUi(); };
    addAndMakeVisible (formatStereoButton);
    addAndMakeVisible (formatMidSideButton);

    // Cancel + Done are popup-modal vestiges. When embedded inline in
    // the AUX lane, the X (remove) button on the slot header handles
    // teardown — the editor doesn't need its own footer actions, and
    // we don't even wire the onClicks so the dead state can't surprise
    // anyone later.
    doneButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a5a3a));
    doneButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    if (! isEmbedded)
    {
        cancelButton.onClick = [this] { if (onDoneCallback) onDoneCallback(); };
        doneButton  .onClick = [this] { if (onDoneCallback) onDoneCallback(); };
        addAndMakeVisible (cancelButton);
        addAndMakeVisible (doneButton);
    }

    // Mark the params as actively in use so the audio thread's strip
    // dispatcher (Phase 3) flips into Hardware mode. The strip's
    // insertMode atom is updated by the caller; the editor sets
    // `enabled` so the panel reflects "this slot is active".
    params.enabled.store (true, std::memory_order_relaxed);
}

HardwareInsertEditor::~HardwareInsertEditor() { stopTimer(); }   // before derived members destruct

void HardwareInsertEditor::timerCallback()
{
    // While the ping is in flight, pingPending stays true. When the
    // audio thread completes capture + correlation it writes the
    // measured lag to pingResult and clears pingPending. We pick up the
    // result here, push it into the routing snapshot + latency slider,
    // and re-enable the button.
    if (! pingButton.isEnabled()
        && ! params.pingPending.load (std::memory_order_acquire))
    {
        const int lag = params.pingResult.load (std::memory_order_relaxed);
        pingButton.setEnabled (true);
        pingButton.setButtonText ("Ping");
        if (lag < 0)
        {
            // Inline failure status - replaces the AlertWindow popup so
            // the user can immediately retry without dismissing a modal.
            // Distinguish -2 (capture stall = no input samples arrived)
            // from -1 (captured but correlation weak) so the user gets
            // the right troubleshooting hint.
            const juce::String msg = (lag == -2)
                ? juce::String ("No input signal - check routing / device.")
                : juce::String ("Ping failed - check level / cables.");
            pingStatusLabel.setText (msg, juce::dontSendNotification);
            pingStatusLabel.setColour (juce::Label::textColourId,
                                         juce::Colour (0xffe06060));
        }
        else
        {
            // Slider range is sized to HardwareInsertSlot::kMaxDelaySamples
            // so the measured lag is always representable without
            // truncation. The publishRoutingFromUi() that follows reads
            // back via getValue(), so storing the lag here is the value
            // that lands in the routing snapshot.
            latencySlider.setValue ((double) lag, juce::dontSendNotification);
            publishRoutingFromUi();

            // Inline success status. Adds the ms equivalent so the user
            // can sanity-check the result against the interface's buffer
            // size without doing arithmetic. When no audio device is
            // open / sample rate is unknown, fall back to "? ms" rather
            // than printing a misleading "0.00 ms".
            auto* device = deviceManager.getCurrentAudioDevice();
            const double sr = device != nullptr ? device->getCurrentSampleRate() : 0.0;
            const juce::String msPart = sr > 0.0
                ? juce::String::formatted ("%.2f ms", (double) lag * 1000.0 / sr)
                : juce::String ("? ms");
            pingStatusLabel.setText (
                "Detected: " + juce::String (lag) + " sam (" + msPart + ")",
                juce::dontSendNotification);
            pingStatusLabel.setColour (juce::Label::textColourId,
                                         juce::Colour (0xff70d090));
        }
    }
}

void HardwareInsertEditor::populateDropdowns()
{
    outChCombo.clear (juce::dontSendNotification);
    inChCombo .clear (juce::dontSendNotification);

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        outChCombo.addItem ("(no audio device)", 1);
        outChCombo.setSelectedId (1, juce::dontSendNotification);
        inChCombo .addItem ("(no audio device)", 1);
        inChCombo .setSelectedId (1, juce::dontSendNotification);
        return;
    }

    const auto outNames = device->getOutputChannelNames();
    const auto inNames  = device->getInputChannelNames();
    const auto routing = currentRouting();

    auto addPairs = [] (juce::ComboBox& cb, const juce::StringArray& names)
    {
        // Pair adjacent channels - typical interface convention is
        // odd/even = L/R. Add a leading "(none)" entry so the user can
        // explicitly clear the routing.
        cb.addItem ("(none)", 1);
        for (int i = 0; i + 1 < names.size(); i += 2)
        {
            const auto label = juce::String (i + 1) + "-" + juce::String (i + 2)
                              + "  " + names[i] + " / " + names[i + 1];
            cb.addItem (label, encodePair (i, i + 1));
        }
    };
    addPairs (outChCombo, outNames);
    addPairs (inChCombo,  inNames);

    // Preselect the routing the params currently hold. Falls back to
    // "(none)" if the stored pair isn't in the menu (device hot-swap).
    auto selectMatching = [] (juce::ComboBox& cb, int chL, int chR)
    {
        if (chL < 0 || chR < 0)
        {
            cb.setSelectedId (1, juce::dontSendNotification);
            return;
        }
        const int targetId = encodePair (chL, chR);
        for (int i = 0; i < cb.getNumItems(); ++i)
        {
            if (cb.getItemId (i) == targetId)
            {
                cb.setSelectedId (targetId, juce::dontSendNotification);
                return;
            }
        }
        cb.setSelectedId (1, juce::dontSendNotification);
    };
    selectMatching (outChCombo, routing.outputChL, routing.outputChR);
    selectMatching (inChCombo,  routing.inputChL,  routing.inputChR);
}

HardwareInsertRouting HardwareInsertEditor::currentRouting() const
{
    return params.routing.current();
}

void HardwareInsertEditor::publishRoutingFromUi()
{
    auto fresh = std::make_unique<HardwareInsertRouting> (currentRouting());

    const int outId = outChCombo.getSelectedId();
    if (outId == 1 || outId == 0)
    {
        fresh->outputChL = -1;
        fresh->outputChR = -1;
    }
    else
    {
        fresh->outputChL = decodePairL (outId);
        fresh->outputChR = decodePairR (outId);
    }

    const int inId = inChCombo.getSelectedId();
    if (inId == 1 || inId == 0)
    {
        fresh->inputChL = -1;
        fresh->inputChR = -1;
    }
    else
    {
        fresh->inputChL = decodePairL (inId);
        fresh->inputChR = decodePairR (inId);
    }

    fresh->latencySamples = (int) latencySlider.getValue();
    fresh->format = formatMidSideButton.getToggleState() ? 1 : 0;

    params.routing.publish (std::move (fresh));
}

void HardwareInsertEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a20));
}

void HardwareInsertEditor::resized()
{
    auto bounds = getLocalBounds().reduced (16, 12);

    headerLabel.setBounds (bounds.removeFromTop (28));
    bounds.removeFromTop (8);

    constexpr int kRowH        = 28;
    constexpr int kRowGap      = 8;
    constexpr int kLabelW      = 140;
    constexpr int kControlPadL = 8;

    auto layoutRow = [&] (juce::Component& label, juce::Component& control)
    {
        auto row = bounds.removeFromTop (kRowH);
        auto lblArea = row.removeFromLeft (kLabelW);
        label.setBounds (lblArea);
        row.removeFromLeft (kControlPadL);
        control.setBounds (row);
        bounds.removeFromTop (kRowGap);
    };

    layoutRow (outVolLabel, outVolSlider);
    layoutRow (outChLabel,  outChCombo);
    bounds.removeFromTop (8);
    layoutRow (inChLabel,   inChCombo);
    layoutRow (inVolLabel,  inVolSlider);
    bounds.removeFromTop (12);

    {
        auto row = bounds.removeFromTop (kRowH);
        latencyLabel.setBounds (row.removeFromLeft (kLabelW));
        row.removeFromLeft (kControlPadL);
        pingButton.setBounds (row.removeFromLeft (90));
        row.removeFromLeft (8);
        pingStatusLabel.setBounds (row);
        bounds.removeFromTop (kRowGap);
    }
    layoutRow (latencySamplesLabel, latencySlider);
    bounds.removeFromTop (12);
    layoutRow (dryWetLabel, dryWetSlider);
    bounds.removeFromTop (12);

    {
        auto row = bounds.removeFromTop (kRowH);
        formatLabel.setBounds (row.removeFromLeft (kLabelW));
        row.removeFromLeft (kControlPadL);
        formatStereoButton .setBounds (row.removeFromLeft (110));
        row.removeFromLeft (8);
        formatMidSideButton.setBounds (row.removeFromLeft (110));
    }

    // Footer row, pinned to the bottom. Skip layout when embedded —
    // the buttons aren't addAndMakeVisible'd, but setBounds on a hidden
    // child is wasted work and also keeps the buttons offscreen in
    // case someone later flips them visible without re-layout.
    if (! isEmbedded)
    {
        auto footer = getLocalBounds().reduced (16, 12).removeFromBottom (kRowH);
        doneButton  .setBounds (footer.removeFromRight (100));
        footer.removeFromRight (8);
        cancelButton.setBounds (footer.removeFromRight (90));
    }
}
} // namespace duskstudio
