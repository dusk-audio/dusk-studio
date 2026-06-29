#include "FreezeDialog.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

namespace duskstudio
{
FreezeDialog::FreezeDialog (AudioEngine& e, Session& s,
                              juce::AudioDeviceManager& dm, int t)
    : engine (e), session (s), deviceManager (dm), trackIndex (t),
      progressBar (progressValue)
{
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8e8));
    addAndMakeVisible (titleLabel);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
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
    cancelButton.onClick = [this] { closeDialog(); };
    closeButton.onClick  = [this] { closeDialog(); };
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (closeButton);
    // After addAndMakeVisible (which force-shows the child): Cancel is up during
    // the render, Close is swapped in once it finishes. Both sit at the same
    // bounds and both call closeDialog(), so the label is purely cosmetic.
    closeButton.setVisible (false);

    auto failBeforeStart = [this] (const juce::String& title, const juce::String& body)
    {
        finished = true;
        succeeded = false;
        titleLabel.setText (title,  juce::dontSendNotification);
        statusLabel.setText (body,  juce::dontSendNotification);
        cancelButton.setVisible (false);
        closeButton.setVisible (true);
    };

    // Validate + compute the output path + render length (message thread).
    if (! engine.freezePrepare (trackIndex, outFile, lenSamples))
    {
        failBeforeStart ("Freeze failed", engine.getLastFreezeError());
        return;
    }

    titleLabel.setText ("Freezing " + session.track (trackIndex).name + "...",
                         juce::dontSendNotification);
    const bool isMidi = session.track (trackIndex).mode.load (std::memory_order_relaxed)
                          == (int) Track::Mode::Midi;
    statusLabel.setText (isMidi
                             ? "Rendering instrument + insert + EQ + comp to audio..."
                             : "Rendering insert + EQ + comp to audio...",
                         juce::dontSendNotification);

    // Async render on the worker thread; the timer polls progress. No worker→UI
    // callbacks, so there's no lifetime race against a closing dialog.
    bounceEngine = std::make_unique<BounceEngine> (engine, session, deviceManager);
    if (! bounceEngine->startFreeze (trackIndex, outFile, lenSamples,
                                      engine.getCurrentSampleRate()))
    {
        // Surface the engine's actual start-failure reason (e.g. "Could not start the
        // freeze render thread"); only fall back to the generic message if it's empty.
        const auto startErr = bounceEngine->getLastError();
        failBeforeStart ("Freeze failed",
                         startErr.isNotEmpty() ? startErr
                                               : juce::String ("A render is already in progress."));
        return;
    }

    startTimerHz (20);
}

FreezeDialog::~FreezeDialog()
{
    stopTimer();
    if (bounceEngine != nullptr && bounceEngine->isRendering())
        bounceEngine->cancel();   // app-shutdown teardown; normal close can't reach here mid-render
    bounceEngine.reset();
}

void FreezeDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202024));
}

void FreezeDialog::resized()
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

void FreezeDialog::timerCallback()
{
    if (bounceEngine == nullptr) return;
    progressValue = (double) bounceEngine->getProgress();
    progressBar.repaint();
    finalizeIfStopped();
}

// Drive the dialog to its finished state once the worker stops — latched by
// `finished` so it runs once whether the timer notices the stop or a Cancel
// click lands in the gap. On success it commits the freeze (frozenRegion +
// bypass + preparePlayback) on the message thread, exactly once via `committed`.
void FreezeDialog::finalizeIfStopped()
{
    if (bounceEngine == nullptr || finished || bounceEngine->isRendering())
        return;

    finished = true;
    const auto err = bounceEngine->getLastError();
    succeeded = err.isEmpty();

    if (succeeded)
    {
        if (! committed)
        {
            engine.commitFreeze (trackIndex, outFile, lenSamples);
            committed = true;
        }
        titleLabel.setText ("Track frozen", juce::dontSendNotification);
        statusLabel.setText ("Instrument bypassed — playing from rendered audio.",
                             juce::dontSendNotification);
        progressValue = 1.0;
        progressBar.repaint();
    }
    else if (err == BounceEngine::kCancelledError)
    {
        titleLabel.setText ("Freeze cancelled", juce::dontSendNotification);
        statusLabel.setText ("No file was written.", juce::dontSendNotification);
    }
    else
    {
        titleLabel.setText ("Freeze failed", juce::dontSendNotification);
        statusLabel.setText (err, juce::dontSendNotification);
    }

    cancelButton.setVisible (false);
    closeButton.setVisible (true);
    stopTimer();
}

void FreezeDialog::closeDialog()
{
    // Cancel pressed mid-render: request a cancel and return without blocking —
    // the worker's teardown re-attaches the audio device and needs the message
    // loop to keep turning. The timer flips us to the finished state once it
    // actually stops, then the Close button appears.
    if (bounceEngine != nullptr && bounceEngine->isRendering())
    {
        statusLabel.setText ("Cancelling...", juce::dontSendNotification);
        cancelButton.setEnabled (false);
        bounceEngine->cancel();
        return;
    }

    // The worker may have stopped between ticks with Cancel still up; finalize
    // now so a just-completed render still commits before we close.
    finalizeIfStopped();

    if (onRequestClose)
        onRequestClose();
    else if (auto* parent = findParentComponentOfClass<juce::DialogWindow>())
        parent->exitModalState (succeeded ? 1 : 0);
}
} // namespace duskstudio
