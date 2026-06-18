#include "DuskAudioDeviceSelector.h"
#include "DuskAlerts.h"

namespace duskstudio
{
namespace
{
constexpr int kNoneId = 1;   // "(None)" entry in the device combos

// How many device channels to request open. The engine imposes no fixed
// channel cap — it bounds-checks every output-pair tap against the device's
// actual open outputs at runtime (OutputPairRouting::tapStereoPairInto). These
// are generous open-request ceilings so the output-pair menu sees every usable
// pair without per-channel check-boxes.
constexpr int kMaxDeviceOutputChannels = 32;
constexpr int kMaxDeviceInputChannels  = 16;

juce::String formatRate (double r)
{
    if (std::abs (r - std::floor (r)) < 0.001)
        return juce::String ((int) r) + " Hz";
    return juce::String (r, 1) + " Hz";
}
} // namespace

DuskAudioDeviceSelector::DuskAudioDeviceSelector (juce::AudioDeviceManager& dm)
    : deviceManager (dm)
{
    auto styleRow = [this] (juce::Label& l, DuskComboBox& c)
    {
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
        addAndMakeVisible (c);
    };
    styleRow (typeLabel,   typeCombo);
    styleRow (outputLabel, outputCombo);
    styleRow (inputLabel,  inputCombo);
    styleRow (rateLabel,   rateCombo);
    styleRow (bufferLabel, bufferCombo);

    typeCombo.onChange = [this]
    {
        if (updating) return;
        if (auto* types = &deviceManager.getAvailableDeviceTypes())
        {
            const int idx = typeCombo.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (idx, types->size()))
            {
                deviceManager.setCurrentAudioDeviceType ((*types)[idx]->getTypeName(),
                                                          /*treatAsChosen*/ true);
                rebuildFromManager();
                if (onDeviceChanged) onDeviceChanged();
            }
        }
    };
    outputCombo.onChange = [this] { applySetupChange (/*deviceChanged*/ true); };
    inputCombo .onChange = [this] { applySetupChange (/*deviceChanged*/ true); };
    rateCombo  .onChange = [this] { applySetupChange (/*deviceChanged*/ false); };
    bufferCombo.onChange = [this] { applySetupChange (/*deviceChanged*/ false); };

    deviceManager.addChangeListener (this);
    rebuildFromManager();
}

DuskAudioDeviceSelector::~DuskAudioDeviceSelector()
{
    deviceManager.removeChangeListener (this);
}

void DuskAudioDeviceSelector::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // External change (hot-plug, another panel) — re-sync our combos.
    if (! updating)
        rebuildFromManager();
}

void DuskAudioDeviceSelector::rebuildFromManager()
{
    const juce::ScopedValueSetter<bool> guard (updating, true);

    const auto setup = deviceManager.getAudioDeviceSetup();
    auto* currentType = deviceManager.getCurrentDeviceTypeObject();
    auto* device = deviceManager.getCurrentAudioDevice();

    // Backend (device type).
    typeCombo.clear (juce::dontSendNotification);
    auto& types = deviceManager.getAvailableDeviceTypes();
    for (int i = 0; i < types.size(); ++i)
        typeCombo.addItem (types[i]->getTypeName(), i + 1);
    if (currentType != nullptr)
        for (int i = 0; i < types.size(); ++i)
            if (types[i] == currentType)
                typeCombo.setSelectedId (i + 1, juce::dontSendNotification);

    // Output + input device lists (current backend).
    auto fillDevices = [&] (DuskComboBox& combo, bool isInput, const juce::String& chosen)
    {
        combo.clear (juce::dontSendNotification);
        combo.addItem ("(None)", kNoneId);
        int sel = kNoneId;
        if (currentType != nullptr)
        {
            const auto names = currentType->getDeviceNames (isInput);
            for (int i = 0; i < names.size(); ++i)
            {
                combo.addItem (names[i], i + 2);
                if (names[i] == chosen) sel = i + 2;
            }
        }
        combo.setSelectedId (sel, juce::dontSendNotification);
    };
    fillDevices (outputCombo, false, setup.outputDeviceName);
    fillDevices (inputCombo,  true,  setup.inputDeviceName);

    // Sample rate + buffer size come from the open device.
    rateCombo.clear (juce::dontSendNotification);
    bufferCombo.clear (juce::dontSendNotification);
    if (device != nullptr)
    {
        for (auto r : device->getAvailableSampleRates())
        {
            const int id = (int) r;
            rateCombo.addItem (formatRate (r), id);
        }
        const double sr = setup.sampleRate > 0 ? setup.sampleRate : device->getCurrentSampleRate();
        rateCombo.setSelectedId ((int) sr, juce::dontSendNotification);

        for (auto b : device->getAvailableBufferSizes())
        {
            const double ms = device->getCurrentSampleRate() > 0
                                ? 1000.0 * b / device->getCurrentSampleRate() : 0.0;
            bufferCombo.addItem (juce::String (b) + " (" + juce::String (ms, 1) + " ms)", b);
        }
        const int bs = setup.bufferSize > 0 ? setup.bufferSize : device->getCurrentBufferSizeSamples();
        bufferCombo.setSelectedId (bs, juce::dontSendNotification);
    }

    const bool haveDevice = device != nullptr;
    rateCombo.setEnabled (haveDevice);
    bufferCombo.setEnabled (haveDevice);
}

void DuskAudioDeviceSelector::applySetupChange (bool deviceChanged)
{
    if (updating) return;

    auto setup = deviceManager.getAudioDeviceSetup();

    const int outId = outputCombo.getSelectedId();
    const int inId  = inputCombo.getSelectedId();
    setup.outputDeviceName = outId == kNoneId ? juce::String() : outputCombo.getText();
    setup.inputDeviceName  = inId  == kNoneId ? juce::String() : inputCombo.getText();

    if (! deviceChanged)
    {
        if (rateCombo.getSelectedId()   > 0) setup.sampleRate = (double) rateCombo.getSelectedId();
        if (bufferCombo.getSelectedId() > 0) setup.bufferSize = bufferCombo.getSelectedId();
    }
    else
    {
        // New device may not support the previous device's rate/buffer — clear
        // them (0 = "pick a valid default for this device") so the open doesn't
        // fail when switching between devices with disjoint capabilities.
        setup.sampleRate = 0.0;
        setup.bufferSize = 0;
    }

    // Open a generous fixed number of channels so the main-output pair menu can
    // offer every active pair — we don't draw per-channel check-boxes. JUCE
    // clamps the request to what the device actually exposes.
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    if (setup.outputDeviceName.isNotEmpty())
        setup.outputChannels.setRange (0, kMaxDeviceOutputChannels, true);

    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (setup.inputDeviceName.isNotEmpty())
        setup.inputChannels.setRange (0, kMaxDeviceInputChannels, true);

    const auto error = deviceManager.setAudioDeviceSetup (setup, /*treatAsChosen*/ true);

    if (error.isNotEmpty())
    {
        if (auto* top = getTopLevelComponent())
            showDuskAlert (*top, "Audio device error", error);
    }
    else if (onDeviceChanged)
    {
        onDeviceChanged();
    }

    // Re-sync (revert on failure, refresh rate/buffer lists on success).
    rebuildFromManager();
}

int DuskAudioDeviceSelector::getPreferredHeight() const noexcept
{
    constexpr int kRowH = 26, kGap = 6, kRows = 5;
    return kRows * kRowH + (kRows - 1) * kGap;
}

void DuskAudioDeviceSelector::resized()
{
    auto area = getLocalBounds();
    constexpr int kRowH = 26, kGap = 6;
    const int labelW = juce::jmin (140, area.getWidth() / 3);

    auto placeRow = [&] (juce::Label& l, DuskComboBox& c)
    {
        auto row = area.removeFromTop (kRowH);
        l.setBounds (row.removeFromLeft (labelW).reduced (2, 2));
        row.removeFromLeft (6);
        c.setBounds (row.reduced (0, 2));
        area.removeFromTop (kGap);
    };
    placeRow (typeLabel,   typeCombo);
    placeRow (outputLabel, outputCombo);
    placeRow (inputLabel,  inputCombo);
    placeRow (rateLabel,   rateCombo);
    placeRow (bufferLabel, bufferCombo);
}
} // namespace duskstudio
