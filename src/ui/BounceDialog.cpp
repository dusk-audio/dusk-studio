#include "BounceDialog.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
BounceDialog::BounceDialog (AudioEngine& e,
                              Session& s,
                              juce::AudioDeviceManager& dm,
                              const juce::File& f,
                              BounceEngine::Mode mode)
    : engine (e), session (s), deviceManager (dm), outputFile (f),
      renderMode (mode), progressBar (progressValue)
{
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
    titleLabel.setText (renderMode == BounceEngine::Mode::MasteringChain
                          ? "Exporting master..."
                          : "Bouncing master mix...",
                         juce::dontSendNotification);
    addAndMakeVisible (titleLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setText (outputFile.getFullPathName(), juce::dontSendNotification);
    addAndMakeVisible (statusLabel);

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
    cancelButton.onClick = [this]
    {
        if (bounceEngine != nullptr && bounceEngine->isRendering())
        {
            statusLabel.setText ("Cancelling...", juce::dontSendNotification);
            bounceEngine->cancel();
        }
        else
        {
            closeDialog();
        }
    };
    closeButton.onClick = [this] { closeDialog(); };
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (closeButton);

    bounceEngine = std::make_unique<BounceEngine> (engine, session, deviceManager);

    // BounceEngine fires its callbacks on the worker thread. We don't touch
    // UI state from there - instead each frame the timer reads the engine's
    // atomics and updates the bar / status. This dodges all the lifetime
    // complexity of marshalling callbacks back to a Component that might be
    // closing.
    if (! bounceEngine->start (outputFile, 0.0, 1024, 5.0, renderMode))
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
        // Defensive: should not happen because closeDialog() blocks until the
        // worker exits before letting the user dismiss, but if the dialog is
        // torn down some other way (app shutdown) we still want to stop the
        // worker cleanly before its destructor runs.
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

    if (! bounceEngine->isRendering() && ! finished)
    {
        finished = true;
        const auto err = bounceEngine->getLastError();
        succeeded = err.isEmpty();

        if (succeeded)
        {
            titleLabel.setText ("Bounce complete", juce::dontSendNotification);
            statusLabel.setText ("Wrote " + outputFile.getFullPathName(),
                                 juce::dontSendNotification);
            progressValue = 1.0;
            progressBar.repaint();
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
}

void BounceDialog::closeDialog()
{
    // If the user hits Cancel and immediately closes, the worker may still be
    // unwinding. Wait for it before returning so the engine is back on the
    // device by the time the dialog is gone.
    if (bounceEngine != nullptr && bounceEngine->isRendering())
    {
        bounceEngine->cancel();
        // Pump the message loop briefly so the worker can publish its final
        // state. We can't block the message thread outright - the worker
        // re-attaches the engine via the device manager, which doesn't need
        // the message thread, but Timer callbacks scheduled on this dialog
        // would otherwise pile up.
        const auto deadline = juce::Time::getMillisecondCounter() + 5000;
        while (bounceEngine->isRendering()
               && juce::Time::getMillisecondCounter() < deadline)
        {
            juce::Thread::sleep (20);
        }
    }

    if (succeeded && onSuccessfulFinish)
        onSuccessfulFinish (outputFile);

    if (auto* parent = findParentComponentOfClass<juce::DialogWindow>())
        parent->exitModalState (succeeded ? 1 : 0);
}
} // namespace duskstudio
