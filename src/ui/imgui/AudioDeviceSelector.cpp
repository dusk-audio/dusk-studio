#include "AudioDeviceSelector.h"
#include "../../engine/device/ClockChoice.h"
#include "../../engine/device/DeviceManager.h"
#include "../../engine/device/IODevice.h"
#include "../../engine/device/IODeviceType.h"
#include "../../foundation/MessageThread.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

constexpr float kRowH = 26.0f;
constexpr float kRowGap = 6.0f;
constexpr int kRows = 5;
constexpr float kLabelGap = 6.0f;

// How many device channels to request open. The engine imposes no fixed channel cap -
// it bounds-checks every output-pair tap against the device's actual open outputs at
// runtime. These are generous ceilings so the output-pair menu sees every usable pair
// without per-channel tick boxes.
constexpr int kMaxDeviceOutputChannels = 32;
constexpr int kMaxDeviceInputChannels = 16;

const char* const kNone = "(None)";

std::string formatRate (double rate)
{
    char buffer[32];
    if (std::abs (rate - std::floor (rate)) < 0.001)
        std::snprintf (buffer, sizeof buffer, "%d Hz", static_cast<int> (rate));
    else
        std::snprintf (buffer, sizeof buffer, "%.1f Hz", rate);
    return buffer;
}

std::string formatBufferSize (int samples, double sampleRate)
{
    const double ms = sampleRate > 0.0 ? 1000.0 * samples / sampleRate : 0.0;
    char buffer[48];
    std::snprintf (buffer, sizeof buffer, "%d (%.1f ms)", samples, ms);
    return buffer;
}

using device::savedClockEntry;
} // namespace

AudioDeviceSelector::AudioDeviceSelector (
    device::DeviceManager& dm,
    std::function<void (std::string, std::string)> alertFn,
    std::function<void()> deviceChanged)
    : deviceManager (dm), alert (std::move (alertFn)),
      onDeviceChanged (std::move (deviceChanged))
{
    deviceManager.addChangeListener (this, [this] { if (! updating) rebuildFromManager(); });
    rebuildFromManager();
}

AudioDeviceSelector::~AudioDeviceSelector()
{
    deviceManager.removeChangeListener (this);
}

float AudioDeviceSelector::preferredHeight() noexcept
{
    return kRows * kRowH + (kRows - 1) * kRowGap;
}

void AudioDeviceSelector::deferred (std::function<void()> action)
{
    // A device open is not something to run inside the frame that asked for it: it
    // takes long enough to be felt, and the change broadcast it fires would
    // repopulate the combo lists the frame is still drawing from.
    std::weak_ptr<int> alive = life;
    dusk::callAsync ([alive, action = std::move (action)]
    {
        if (alive.expired())
            return;
        action();
    });
}

void AudioDeviceSelector::rebuildFromManager()
{
    const auto setup = deviceManager.getSetup();
    auto* const currentType = deviceManager.getCurrentDeviceType();
    auto* const device = deviceManager.getCurrentDevice();

    types.clear();
    const auto available = deviceManager.getAvailableDeviceTypes();
    int selectedType = -1;
    for (std::size_t i = 0; i < available.size(); ++i)
    {
        types.add (available[i]->getTypeName());
        if (available[i] == currentType)
            selectedType = static_cast<int> (i);
    }
    types.finish (selectedType);

    const auto fillDevices = [&] (ComboModel& combo, bool wantInputs, const std::string& chosen)
    {
        combo.clear();
        combo.add (kNone);
        int selected = 0;
        if (currentType != nullptr)
        {
            const auto names = currentType->getDeviceNames (wantInputs);
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (names[i] == chosen)
                    selected = static_cast<int> (i) + 1;
                combo.add (names[i]);
            }
        }
        combo.finish (selected);
    };
    fillDevices (outputs, false, setup.outputDeviceName);
    fillDevices (inputs, true, setup.inputDeviceName);

    rates.clear();
    buffers.clear();
    rateValues.clear();
    bufferValues.clear();
    haveDevice = device != nullptr;

    if (device != nullptr)
    {
        // Each list selects what is saved. Where a graph runs the device at
        // another clock, the saved entry says so, and the running one is an
        // entry of its own that can be picked to save it.
        const auto chosen = deviceManager.getChosenSetup();
        const double runningRate = setup.sampleRate > 0.0 ? setup.sampleRate
                                                          : device->getCurrentSampleRate();
        rateValues = device->getAvailableSampleRates();
        const int savedRate = savedClockEntry (rateValues, chosen.sampleRate, runningRate);
        for (std::size_t i = 0; i < rateValues.size(); ++i)
        {
            auto label = formatRate (rateValues[i]);
            if (static_cast<int> (i) == savedRate && std::abs (rateValues[i] - runningRate) >= 0.5)
                label += " (saved; now running " + formatRate (runningRate) + ")";
            rates.add (label);
        }
        rates.finish (savedRate);

        const int runningBuffer = setup.bufferSize > 0 ? setup.bufferSize
                                                       : device->getCurrentBufferSizeSamples();
        bufferValues = device->getAvailableBufferSizes();
        const int savedBuffer = savedClockEntry (bufferValues, chosen.bufferSize, runningBuffer);
        for (std::size_t i = 0; i < bufferValues.size(); ++i)
        {
            auto label = formatBufferSize (bufferValues[i], device->getCurrentSampleRate());
            if (static_cast<int> (i) == savedBuffer && bufferValues[i] != runningBuffer)
                label += " (saved; now running " + std::to_string (runningBuffer) + ")";
            buffers.add (label);
        }
        buffers.finish (savedBuffer);
    }
    else
    {
        rates.finish (-1);
        buffers.finish (-1);
    }
}

void AudioDeviceSelector::applyTypeChange()
{
    const auto available = deviceManager.getAvailableDeviceTypes();
    if (types.selected < 0 || types.selected >= static_cast<int> (available.size()))
        return;

    deviceManager.setCurrentDeviceType (available[static_cast<std::size_t> (types.selected)]
                                            ->getTypeName(),
                                        /*treatAsChosen*/ true);
    rebuildFromManager();
    if (onDeviceChanged)
        onDeviceChanged();
}

void AudioDeviceSelector::applySetupChange (Pick pick)
{
    // From the chosen setup, and only the list the user picked from is read:
    // the other one keeps what is saved for it. Its selection can be the
    // running value standing in for a saved one this device does not list.
    auto setup = deviceManager.getChosenSetup();

    setup.outputDeviceName = outputs.selected > 0 ? outputs.label (outputs.selected)
                                                  : std::string();
    setup.inputDeviceName = inputs.selected > 0 ? inputs.label (inputs.selected)
                                                : std::string();

    if (pick == Pick::device)
    {
        // A new device may not support the previous one's rate and buffer, so both go
        // back to "pick a valid default" rather than failing the open.
        setup.sampleRate = 0.0;
        setup.bufferSize = 0;
    }
    else if (pick == Pick::rate)
    {
        if (rates.selected >= 0 && rates.selected < static_cast<int> (rateValues.size()))
            setup.sampleRate = rateValues[static_cast<std::size_t> (rates.selected)];
    }
    else if (buffers.selected >= 0 && buffers.selected < static_cast<int> (bufferValues.size()))
    {
        setup.bufferSize = bufferValues[static_cast<std::size_t> (buffers.selected)];
    }

    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    if (! setup.outputDeviceName.empty())
        setup.outputChannels.setRange (0, kMaxDeviceOutputChannels, true);

    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (! setup.inputDeviceName.empty())
        setup.inputChannels.setRange (0, kMaxDeviceInputChannels, true);

    std::string error;
    {
        updating = true;
        error = deviceManager.setSetup (setup, /*treatAsChosen*/ true);
        updating = false;
    }

    if (! error.empty())
    {
        if (alert)
            alert ("Audio device error", error);
    }
    else if (onDeviceChanged)
    {
        onDeviceChanged();
    }

    // Revert on failure, refresh the rate and buffer lists on success.
    rebuildFromManager();
}

void AudioDeviceSelector::draw (dw::Context& ctx, ImVec2 at, float width)
{
    const float rowH = ctx.s (kRowH);
    const float gap = ctx.s (kRowGap);
    const float labelW = std::min (ctx.s (140.0f), width / 3.0f);
    const float controlX = at.x + labelW + ctx.s (kLabelGap);
    const float controlW = std::max (ctx.s (40.0f), at.x + width - controlX);

    float y = at.y;
    const auto row = [&] (const char* caption, const char* id, ComboModel& combo,
                          bool enabled) -> bool
    {
        formLabel (ctx, ImVec2 (at.x, y), labelW, rowH, caption);
        const bool picked = formCombo (ctx, id, ImVec2 (controlX, y),
                                       ImVec2 (controlX + controlW, y + rowH),
                                       combo, enabled);
        y += rowH + gap;
        return picked;
    };

    if (row ("Audio backend", "##audio-backend", types, true))
        deferred ([this] { applyTypeChange(); });
    if (row ("Output device", "##audio-output", outputs, true))
        deferred ([this] { applySetupChange (Pick::device); });
    if (row ("Input device", "##audio-input", inputs, true))
        deferred ([this] { applySetupChange (Pick::device); });
    if (row ("Sample rate", "##audio-rate", rates, haveDevice))
        deferred ([this] { applySetupChange (Pick::rate); });
    if (row ("Buffer size", "##audio-buffer", buffers, haveDevice))
        deferred ([this] { applySetupChange (Pick::buffer); });
}
} // namespace duskstudio::imgui
