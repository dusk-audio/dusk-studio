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
// Lifetime: launched from MainComponent's "Bounce..." button. Closing the
// dialog while a render is in flight requests a cancel and returns immediately;
// the 20 Hz timer flips the dialog to its finished state once the worker
// actually stops (the close never blocks the message thread - the worker's
// teardown re-attaches the audio device and needs the loop to keep turning).
// The host wires onRequestClose to dismiss the embedded modal.
class BounceDialog final : public juce::Component,
                            private juce::Timer
{
public:
    // renderSampleRate 0 = the engine's current rate; wavBitDepth 16 dithers
    // (see BounceEngine::start). The mastering export presets drive both.
    // realtime plays the session live and captures it (hardware inserts
    // print); MasterMix / Stems only.
    BounceDialog (AudioEngine& engine,
                   Session& session,
                   juce::AudioDeviceManager& deviceManager,
                   const juce::File& outputFile,
                   BounceEngine::Mode mode = BounceEngine::Mode::MasterMix,
                   BounceEngine::Format format = BounceEngine::Format::Wav,
                   int mp3BitrateKbps = 320,
                   double renderSampleRate = 0.0,
                   int wavBitDepth = 24,
                   bool realtime = false);
    ~BounceDialog() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // Fired once when the dialog is dismissed AFTER a successful render.
    // Not fired on cancel or failure. Caller can use it to chain post-bounce
    // workflow (e.g. Mixdown -> switch to Mastering with the new file loaded).
    // Defer any heavy work (stage switches, etc.) via callAsync - this fires
    // from inside the close-button callback.
    std::function<void(juce::File)> onSuccessfulFinish;

    // Fired to dismiss the dialog. The host (which owns the EmbeddedModal)
    // wires this to its modal.close(). Without it the embedded dialog has no
    // way to close itself (exitModalState only works for a real DialogWindow).
    std::function<void()> onRequestClose;

private:
    void timerCallback() override;
    void finalizeIfStopped();
    void closeDialog();

    AudioEngine& engine;
    Session& session;
    juce::AudioDeviceManager& deviceManager;
    juce::File outputFile;
    BounceEngine::Mode renderMode;
    BounceEngine::Format renderFormat;
    int mp3Bitrate;
    double renderSampleRate;
    int wavBitDepth;
    bool renderRealtime;

    std::unique_ptr<BounceEngine> bounceEngine;

    juce::Label       titleLabel;
    juce::Label       statusLabel;
    juce::Label       hwWarnLabel;
    juce::ProgressBar progressBar;
    juce::TextButton  cancelButton { "Cancel" };
    juce::TextButton  closeButton  { "Close" };

    double progressValue = 0.0;  // bound to ProgressBar
    bool   finished      = false;
    bool   succeeded     = false;
};
} // namespace duskstudio
