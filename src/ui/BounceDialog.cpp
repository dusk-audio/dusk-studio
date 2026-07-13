#include "BounceDialog.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
BounceDialog::BounceDialog (AudioEngine& e,
                              Session& s,
                              juce::AudioDeviceManager& dm,
                              const juce::File& f,
                              BounceEngine::Mode mode,
                              BounceEngine::Format format,
                              int mp3BitrateKbps,
                              double sampleRate,
                              int bitDepth,
                              bool realtime)
    : engine (e), session (s), deviceManager (dm), outputFile (f),
      renderMode (mode), renderFormat (format), mp3Bitrate (mp3BitrateKbps),
      renderSampleRate (sampleRate), wavBitDepth (bitDepth),
      renderRealtime (realtime),
      progressBar (progressValue)
{
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
    titleLabel.setText (renderMode == BounceEngine::Mode::MasteringChain
                          ? juce::String ("Exporting master...")
                          : juce::String (renderMode == BounceEngine::Mode::Stems
                                            ? "Bouncing stems"
                                            : "Bouncing master mix")
                              + (renderRealtime ? " (realtime)..." : "..."),
                         juce::dontSendNotification);
    addAndMakeVisible (titleLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setText (outputFile.getFullPathName(), juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

    // Offline renders drive the engine detached from the audio device, so an
    // external hardware-insert loop can't run: those inserts print dry (or
    // silent at full wet). Mastering renders skip the strips, and a realtime
    // bounce runs the loop for real, so only the offline strip-driven modes
    // warn. Components are visible by default, so hide first - resized()
    // reserves the warning row off isVisible().
    hwWarnLabel.setVisible (false);
    if (! renderRealtime
        && renderMode != BounceEngine::Mode::MasteringChain
        && BounceEngine::anyHardwareInsertActive (session))
    {
        hwWarnLabel.setJustificationType (juce::Justification::centredLeft);
        hwWarnLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0b050));
        hwWarnLabel.setText ("Hardware inserts are bypassed in an offline render "
                             "and will print dry.",
                             juce::dontSendNotification);
        addAndMakeVisible (hwWarnLabel);
    }

    progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff101012));
    progressBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff5fa8ff));
    addAndMakeVisible (progressBar);

    auto styleButton = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff202024));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd0d0d0));
    };
    styleButton (cancelButton);
    styleButton (closeButton);
    closeButton.setVisible (false);  // shown after the render finishes
    cancelButton.onClick = [this] { closeDialog(); };
    closeButton.onClick  = [this] { closeDialog(); };
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (closeButton);

    bounceEngine = std::make_unique<BounceEngine> (engine, session, deviceManager);

    // BounceEngine fires its callbacks on the worker thread. We don't touch
    // UI state from there - instead each frame the timer reads the engine's
    // atomics and updates the bar / status. This dodges all the lifetime
    // complexity of marshalling callbacks back to a Component that might be
    // closing.
    if (! bounceEngine->start (outputFile, renderSampleRate, 1024, 5.0,
                                renderMode, renderFormat, mp3Bitrate, wavBitDepth,
                                renderRealtime))
    {
        finished = true;
        succeeded = false;
        statusLabel.setText ("Could not start bounce (already in progress?)",
                             juce::dontSendNotification);
        cancelButton.setVisible (false);
        closeButton.setVisible (true);
        return;
    }

    startTimerHz (20);
}

BounceDialog::~BounceDialog()
{
    stopTimer();
    if (bounceEngine != nullptr && bounceEngine->isRendering())
    {
        // Defensive: shouldn't happen during normal use because the Close
        // button stays hidden while a render is in flight, so the user can't
        // dismiss us mid-bounce - closeDialog() just requests cancel() and
        // returns, keeping the message loop free for the worker's teardown
        // (which re-attaches the audio device). But if the dialog is torn down
        // some other way (app shutdown) we still stop the worker cleanly before
        // its destructor runs.
        bounceEngine->cancel();
    }
    bounceEngine.reset();
}

void BounceDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202024));
}

void BounceDialog::resized()
{
    auto area = getLocalBounds().reduced (16);
    titleLabel.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);
    statusLabel.setBounds (area.removeFromTop (20));
    if (hwWarnLabel.isVisible())
    {
        area.removeFromTop (2);
        hwWarnLabel.setBounds (area.removeFromTop (18));
    }
    area.removeFromTop (12);
    progressBar.setBounds (area.removeFromTop (24));
    area.removeFromTop (16);

    auto buttons = area.removeFromBottom (30);
    cancelButton.setBounds (buttons.removeFromRight (90));
    closeButton.setBounds  (cancelButton.getBounds());
}

void BounceDialog::timerCallback()
{
    if (bounceEngine == nullptr) return;

    progressValue = (double) bounceEngine->getProgress();
    progressBar.repaint();

    if (renderMode == BounceEngine::Mode::Stems && bounceEngine->isRendering())
    {
        const int total = bounceEngine->getTotalStemsToRender();
        if (total > 0)
        {
            statusLabel.setText ("Rendering " + juce::String (total) + " stem"
                                  + juce::String (total == 1 ? "" : "s")
                                  + " -> " + outputFile.getParentDirectory().getFullPathName(),
                                  juce::dontSendNotification);
        }
    }

    finalizeIfStopped();
}

// Drive the dialog into its finished state once the worker stops. The `finished`
// flag latches this so it runs exactly once - whether the 20 Hz timer notices
// the stop first, or a Cancel click lands in the gap between the worker stopping
// and the next tick (the button is still visible then). Calling it from both
// paths guarantees `succeeded` is computed before closeDialog() decides whether
// to fire onSuccessfulFinish, so a just-completed render isn't lost.
void BounceDialog::finalizeIfStopped()
{
    if (bounceEngine == nullptr || finished || bounceEngine->isRendering())
        return;

    finished = true;
    const auto err = bounceEngine->getLastError();
    succeeded = err.isEmpty();

    if (succeeded)
    {
        titleLabel.setText ("Bounce complete", juce::dontSendNotification);
        if (renderMode == BounceEngine::Mode::Stems)
        {
            const int total = bounceEngine->getTotalStemsToRender();
            statusLabel.setText ("Wrote " + juce::String (total) + " stem"
                                  + juce::String (total == 1 ? "" : "s")
                                  + " to " + outputFile.getParentDirectory().getFullPathName(),
                                  juce::dontSendNotification);
        }
        else
        {
            statusLabel.setText ("Wrote " + outputFile.getFullPathName(),
                                  juce::dontSendNotification);
        }
        progressValue = 1.0;
        progressBar.repaint();
    }
    else if (err == BounceEngine::kCancelledError)
    {
        titleLabel.setText ("Bounce cancelled", juce::dontSendNotification);
        statusLabel.setText ("No file was written.", juce::dontSendNotification);
    }
    else
    {
        titleLabel.setText ("Bounce failed", juce::dontSendNotification);
        statusLabel.setText (err, juce::dontSendNotification);
    }

    cancelButton.setVisible (false);
    closeButton.setVisible (true);
    stopTimer();
}

void BounceDialog::closeDialog()
{
    // Still rendering (Cancel pressed before the bounce finished): request a
    // cancel and return. The 20 Hz timer flips the dialog to its finished
    // state once the worker actually stops, then the Close button appears.
    // We must NOT block the message thread waiting here - the worker's
    // teardown re-attaches the audio device and needs the loop to keep
    // turning, so a blocking wait deadlocks it (and freezes the UI).
    if (bounceEngine != nullptr && bounceEngine->isRendering())
    {
        statusLabel.setText ("Cancelling...", juce::dontSendNotification);
        cancelButton.setEnabled (false);
        bounceEngine->cancel();
        return;
    }

    // The worker may have stopped between timer ticks with the Cancel button
    // still up; finalize now so `succeeded` reflects the real outcome before we
    // decide whether to fire onSuccessfulFinish.
    finalizeIfStopped();

    if (succeeded && onSuccessfulFinish)
        onSuccessfulFinish (outputFile);

    // Embedded modal: the host closes us. Fall back to exitModalState for the
    // rare DialogWindow host.
    if (onRequestClose)
        onRequestClose();
    else if (auto* parent = findParentComponentOfClass<juce::DialogWindow>())
        parent->exitModalState (succeeded ? 1 : 0);
}
} // namespace duskstudio
