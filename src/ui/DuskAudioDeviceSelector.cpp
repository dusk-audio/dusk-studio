#include "DuskAudioDeviceSelector.h"
#include "DuskAlerts.h"

#include <algorithm>

namespace duskstudio
{
namespace
{
constexpr int kNoneId = 1;   // "(None)" entry in the device combos

// How many device channels to request open. The engine imposes no fixed
// channel cap - it bounds-checks every output-pair tap against the device's
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

DuskAudioDeviceSelector::DuskAudioDeviceSelector (device::DeviceManager& dm)
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
        const auto types = deviceManager.getAvailableDeviceTypes();
        const int idx = typeCombo.getSelectedId() - 1;
        if (idx >= 0 && idx < (int) types.size())
        {
            deviceManager.setCurrentDeviceType (types[(size_t) idx]->getTypeName(),
                                                /*treatAsChosen*/ true);
            rebuildFromManager();
            if (onDeviceChanged) onDeviceChanged();
        }
    };
    outputCombo.onChange = [this] { applySetupChange (/*deviceChanged*/ true); };
    inputCombo .onChange = [this] { applySetupChange (/*deviceChanged*/ true); };
    rateCombo  .onChange = [this] { applySetupChange (/*deviceChanged*/ false); };
    bufferCombo.onChange = [this] { applySetupChange (/*deviceChanged*/ false); };

    // External change (hot-plug, another panel) - re-sync our combos.
    deviceManager.addChangeListener (this, [this] { if (! updating) rebuildFromManager(); });
    rebuildFromManager();
}

DuskAudioDeviceSelector::~DuskAudioDeviceSelector()
{
    deviceManager.removeChangeListener (this);
}

void DuskAudioDeviceSelector::rebuildFromManager()
{
    const juce::ScopedValueSetter<bool> guard (updating, true);

    const auto setup = deviceManager.getSetup();
    auto* currentType = deviceManager.getCurrentDeviceType();
    auto* device = deviceManager.getCurrentDevice();

    // Backend (device type).
    typeCombo.clear (juce::dontSendNotification);
    const auto types = deviceManager.getAvailableDeviceTypes();
    for (int i = 0; i < (int) types.size(); ++i)
        typeCombo.addItem (juce::String (types[(size_t) i]->getTypeName()), i + 1);
    if (currentType != nullptr)
        for (int i = 0; i < (int) types.size(); ++i)
            if (types[(size_t) i] == currentType)
                typeCombo.setSelectedId (i + 1, juce::dontSendNotification);

    // Output + input device lists (current backend).
    auto fillDevices = [&] (DuskComboBox& combo, bool isInput, const std::string& chosen)
    {
        combo.clear (juce::dontSendNotification);
        combo.addItem ("(None)", kNoneId);
        int sel = kNoneId;
        if (currentType != nullptr)
        {
            const auto names = currentType->getDeviceNames (isInput);
            for (int i = 0; i < (int) names.size(); ++i)
            {
                combo.addItem (juce::String (names[(size_t) i]), i + 2);
                if (names[(size_t) i] == chosen) sel = i + 2;
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

    auto setup = deviceManager.getSetup();

    const int outId = outputCombo.getSelectedId();
    const int inId  = inputCombo.getSelectedId();
    setup.outputDeviceName = outId == kNoneId ? std::string() : outputCombo.getText().toStdString();
    setup.inputDeviceName  = inId  == kNoneId ? std::string() : inputCombo.getText().toStdString();

    if (! deviceChanged)
    {
        if (rateCombo.getSelectedId()   > 0) setup.sampleRate = (double) rateCombo.getSelectedId();
        if (bufferCombo.getSelectedId() > 0) setup.bufferSize = bufferCombo.getSelectedId();
    }
    else
    {
        // New device may not support the previous device's rate/buffer - clear
        // them (0 = "pick a valid default for this device") so the open doesn't
        // fail when switching between devices with disjoint capabilities.
        setup.sampleRate = 0.0;
        setup.bufferSize = 0;
    }

    // Open a generous fixed number of channels so the main-output pair menu can
    // offer every active pair - we don't draw per-channel check-boxes. JUCE
    // clamps the request to what the device actually exposes.
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    if (! setup.outputDeviceName.empty())
        setup.outputChannels.setRange (0, kMaxDeviceOutputChannels, true);

    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (! setup.inputDeviceName.empty())
        setup.inputChannels.setRange (0, kMaxDeviceInputChannels, true);

    const auto error = deviceManager.setSetup (setup, /*treatAsChosen*/ true);

    if (! error.empty())
    {
        if (auto* top = getTopLevelComponent())
            showDuskAlert (*top, "Audio device error", juce::String (error));
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
    const int labelW = std::min (140, area.getWidth() / 3);

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
