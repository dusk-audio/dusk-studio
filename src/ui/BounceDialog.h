#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../engine/BounceEngine.h"

namespace duskstudio
{
class AudioEngine;
class Session;

// Modal panel that runs a master-mix bounce to a user-chosen WAV file.
// Owns its own BounceEngine; the engine's worker-thread callbacks are
// marshalled to the message thread via SafePointer + MessageManager::callAsync
// so the panel can update its progress bar / status label safely.
//
// Lifetime: launched from MainComponent's "Bounce..." button. The bounce
// runs to completion or cancellation before the dialog is dismissed; closing
// the dialog while a render is in flight requests a cancel and waits.
class BounceDialog final : public juce::Component,
                            private juce::Timer
{
public:
    BounceDialog (AudioEngine& engine,
                   Session& session,
                   juce::AudioDeviceManager& deviceManager,
                   const juce::File& outputFile,
                   BounceEngine::Mode mode = BounceEngine::Mode::MasterMix);
    ~BounceDialog() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // Fired once when the dialog is dismissed AFTER a successful render.
    // Not fired on cancel or failure. Caller can use it to chain post-bounce
    // workflow (e.g. Mixdown → switch to Mastering with the new file loaded).
    std::function<void(juce::File)> onSuccessfulFinish;

private:
    void timerCallback() override;
    void closeDialog();

    AudioEngine& engine;
    Session& session;
    juce::AudioDeviceManager& deviceManager;
    juce::File outputFile;
    BounceEngine::Mode renderMode;

    std::unique_ptr<BounceEngine> bounceEngine;

    juce::Label       titleLabel;
    juce::Label       statusLabel;
    juce::ProgressBar progressBar;
    juce::TextButton  cancelButton { "Cancel" };
    juce::TextButton  closeButton  { "Close" };

    double progressValue = 0.0;  // bound to ProgressBar
    bool   finished      = false;
    bool   succeeded     = false;
};
} // namespace duskstudio
