#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

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
                    juce::AudioDeviceManager& dm,
                    Session& session);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    AudioEngine& engine;
    juce::AudioDeviceManager& deviceManager;
    Session& session;

    juce::TextEditor   logView;
    juce::TextButton   runButton  { "Run" };
    juce::TextButton   copyButton { "Copy" };
    juce::TextButton   closeButton { "Close" };

    void runTest();
    void copyToClipboard();
};
} // namespace duskstudio
