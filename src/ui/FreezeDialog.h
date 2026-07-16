#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../engine/BounceEngine.h"

namespace duskstudio
{
class AudioEngine;
class Session;

// Modal progress panel for a track FREEZE. Owns a BounceEngine, kicks the
// pre-fader render on the worker thread (BounceEngine::startFreeze) so a long
// render never wedges the message thread, and polls the engine's atomics on a
// 20 Hz timer - the same no-callback-marshalling pattern as BounceDialog. On
// success it commits the freeze on the message thread (AudioEngine::commitFreeze)
// before the user dismisses it; Cancel aborts the render and writes nothing.
class FreezeDialog final : public juce::Component,
                            private juce::Timer
{
public:
    FreezeDialog (AudioEngine& engine, Session& session, int trackIndex);
    ~FreezeDialog() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    // Host wires this to close the embedded modal (and refresh the strip's
    // FREEZE button). Fired from the Close button, after success or failure.
    std::function<void()> onRequestClose;

private:
    void timerCallback() override;
    void finalizeIfStopped();
    void closeDialog();

    AudioEngine& engine;
    Session& session;
    int trackIndex;
    juce::File outFile;
    std::int64_t lenSamples = 0;

    std::unique_ptr<BounceEngine> bounceEngine;

    // progressValue is declared BEFORE progressBar: the ctor constructs progressBar
    // with a reference to it, so it must already be alive at that point.
    double progressValue = 0.0;   // bound to ProgressBar

    juce::Label       titleLabel;
    juce::Label       statusLabel;
    juce::ProgressBar progressBar;
    juce::TextButton  cancelButton { "Cancel" };
    juce::TextButton  closeButton  { "Close" };

    bool   isMidiTrack = false;   // cached in ctor - picks the audio vs instrument message
    bool   finished  = false;
    bool   succeeded = false;
    bool   committed = false;     // commitFreeze runs exactly once
};
} // namespace duskstudio
