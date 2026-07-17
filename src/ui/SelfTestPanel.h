#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/device/DeviceManager.h"

namespace duskstudio
{
class AudioEngine;
class Session;

// Modal panel that runs the AudioPipelineSelfTest and displays the formatted
// log in a copy-able TextEditor. Spawned from AudioSettingsPanel's
// "Run Self-Test" button.
class SelfTestPanel final : public juce::Component
{
public:
    SelfTestPanel (AudioEngine& engine,
                    device::DeviceManager& dm,
                    Session& session);

    // Wired by the host to tear down the EmbeddedModal hosting this panel.
    // The panel is NOT a DialogWindow, so it can't dismiss itself.
    std::function<void()> onCloseRequested;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    AudioEngine& engine;
    device::DeviceManager& deviceManager;
    Session& session;

    juce::TextEditor   logView;
    juce::TextButton   runButton  { "Run" };
    juce::TextButton   copyButton { "Copy" };
    juce::TextButton   closeButton { "Close" };

    void runTest();
    void copyToClipboard();
};
} // namespace duskstudio
